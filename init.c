/*
 * BlueSCSI DaynaPORT sleep/wake handler — System Extension (INIT)
 *
 * Loads at boot like any INIT in the Extensions folder.  No user
 * interaction needed: the driver is reinitialised automatically every
 * time the PowerBook wakes from sleep.
 *
 * Design
 * ──────
 * After boot the INIT code resource is locked in memory permanently.
 * Two OS hooks are installed:
 *
 *   Sleep Queue  – catches sleepWakeUp at interrupt level.  Calls
 *                  NMInstall to post a silent Notification Manager
 *                  request (NMInstall is documented interrupt-safe).
 *
 *   nmResp       – the Notification Manager invokes this callback at
 *                  task level (safe for all Toolbox calls).  Performs
 *                  KillIO + PBControlSync, then calls NMRemove.
 *
 * This replaces the earlier WNE-patch approach, which was unreliable:
 * any extension loaded after ours could re-patch _WaitNextEvent, and
 * if that extension's handler lived in a relocatable System heap block
 * the address stored in the trap table became stale after heap
 * compaction, crashing the machine on the first sleep attempt.
 *
 * The Notification Manager avoids all of this: there are no stored
 * trap addresses and no WNE chain to corrupt.
 *
 * A5-free design
 * ──────────────
 * After _start() returns, Retro68's A5 world is freed.  All callbacks
 * therefore access state exclusively through:
 *   • An ExtState block in the System heap (NewPtrSysClear).
 *   • SleepQRec is the first field of ExtState, so the sleep callback
 *     receives qRecPtr == &state — no A5 needed.
 *   • nmRec.nmRefCon holds the ExtState pointer for the nmResp callback.
 *   • ReinitViaNotification lives in the locked INIT code resource and
 *     has a stable address for the life of the session.
 *
 * Logging
 * ───────
 * A plain-text log file named "BlueSCSI Sleep Log" is written to the
 * System Folder.  It is appended on every boot and every wake, so
 * multiple sleep/wake cycles accumulate in one file.  Open it in
 * TeachText or SimpleText to inspect it.
 *
 * All log I/O uses File Manager traps directly (FSpOpenDF, FSWrite, etc.)
 * rather than stdio, because stdio depends on A5-relative globals that are
 * not available after _start() returns.
 */

#include <MacTypes.h>
#include <Power.h>
#include <Devices.h>
#include <Files.h>
#include <Folders.h>
#include <Memory.h>
#include <Notification.h>
#include <OSUtils.h>
#include <string.h>
#include "Retro68Runtime.h"

/* ── low-memory Unit Table ───────────────────────────────────────────────── */

#define LMUTableBase()      (*(DCtlHandle **)0x011C)
#define LMUnitNtryCnt()     (*(short *)0x01D2)

/* ── state allocated in System heap ─────────────────────────────────────── */

/*
 * IMPORTANT: sleepRec MUST be the first field.
 * The Sleep Queue callback receives qRecPtr (== &sleepRec).
 * Because sleepRec is first, qRecPtr == (ExtState *)state, giving the
 * callback access to all fields without any A5 reference.
 *
 * Layout:
 *   offset  0: sleepRec      (SleepQRec, 12 bytes)
 *   offset 12: driverRefNum  (short,      2 bytes)
 *   offset 14: nmPending     (byte,       1 byte ) 1 = NMRec in queue
 *   offset 15: _pad          (byte,       1 byte )
 *   offset 16: wakeCount     (short,      2 bytes)
 *   offset 18: logSpecValid  (Boolean,    1 byte )
 *   offset 19: _pad2         (byte,       1 byte )
 *   offset 20: logSpec       (FSSpec,    70 bytes)
 *   offset 90: nmRec         (NMRec,     ~36 bytes)
 */
typedef struct ExtState {
    SleepQRec       sleepRec;
    short           driverRefNum;
    unsigned char   nmPending;      /* 1 = NMRec is already in the NM queue */
    unsigned char   _pad;
    short           wakeCount;
    Boolean         logSpecValid;
    unsigned char   _pad2;
    FSSpec          logSpec;
    NMRec           nmRec;
} ExtState;

/* ── A5-free logging helpers ─────────────────────────────────────────────── */

/*
 * All log functions use File Manager traps only (no stdio, no A5).
 * LogOpen returns a file reference number, or 0 on failure.
 * Always call LogClose if LogOpen returned non-zero.
 */

static short LogOpen(ExtState *state)
{
    short ref;
    long  eof;
    OSErr err;

    /* Lazy init: locate the System Folder on first call (task level only).
     * We defer this from _start() so that boot-time file I/O does not
     * trigger System heap compaction, which would move other extensions'
     * trap handlers and leave the trap table with stale addresses. */
    if (!state->logSpecValid) {
        short vRef;
        long  dirID;

        err = FindFolder(kOnSystemDisk, kSystemFolderType,
                         kDontCreateFolder, &vRef, &dirID);
        if (err != noErr) return 0;

        err = FSMakeFSSpec(vRef, dirID,
                           (const unsigned char *)"\pBlueSCSI Sleep Log",
                           &state->logSpec);
        if (err != noErr && err != fnfErr) return 0;

        state->logSpecValid = true;
    }

    /* Create the file if it does not yet exist; ignore error if it does. */
    FSpCreate(&state->logSpec, 'ttxt', 'TEXT', 0 /* smSystemScript */);

    err = FSpOpenDF(&state->logSpec, fsRdWrPerm, &ref);
    if (err != noErr) return 0;

    /* Seek to end so each write appends. */
    GetEOF(ref, &eof);
    SetFPos(ref, fsFromStart, eof);

    return ref;
}

static void LogClose(short ref, short vRefNum)
{
    FSClose(ref);
    FlushVol(NULL, vRefNum);   /* commit to disk — important if Mac crashes on wake */
}

/* Write a C string (no A5: length computed with a simple loop, not strlen). */
static void LogStr(short ref, const char *s)
{
    long        n = 0;
    const char *p = s;
    while (*p++) n++;
    if (n > 0) FSWrite(ref, &n, (Ptr)s);
}

/* Write a signed 16-bit decimal integer. */
static void LogShort(short ref, short val)
{
    char           buf[7];   /* worst case: "-32768" = 6 chars */
    short          i = 7;
    Boolean        neg = (val < 0);
    unsigned short u;
    long           n;

    /* Avoid overflow on SHRT_MIN by working in unsigned. */
    u = neg ? (unsigned short)(0u - (unsigned short)val) : (unsigned short)val;

    if (u == 0) {
        buf[--i] = '0';
    } else {
        while (u) {
            buf[--i] = (char)('0' + u % 10);
            u /= 10;
        }
    }
    if (neg) buf[--i] = '-';

    n = 7 - i;
    FSWrite(ref, &n, buf + i);
}

/* ── A5-free driver helpers ──────────────────────────────────────────────── */

static Boolean PStrEqualLocal(const unsigned char *a, const unsigned char *b)
{
    unsigned char len = a[0];
    unsigned char i;
    if (b[0] != len) return false;
    for (i = 1; i <= len; i++) {
        unsigned char ca = a[i], cb = b[i];
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return false;
    }
    return true;
}

static OSErr FindDriverLocal(const unsigned char *name, short *outRef)
{
    DCtlHandle *tbl = LMUTableBase();
    short       cnt = LMUnitNtryCnt();
    short       i;

    for (i = 0; i < cnt; i++) {
        DCtlHandle    h = tbl[i];
        DCtlPtr       dce;
        DRVRHeaderPtr hdr;

        if (h == NULL) continue;
        dce = *h;
        if (dce == NULL) continue;
        if (!(dce->dCtlFlags & 0x0020 /* dOpened */)) continue;

        if (dce->dCtlFlags & 0x0040 /* dRAMBased */) {
            Handle drvrH = (Handle)dce->dCtlDriver;
            if (drvrH == NULL || *drvrH == NULL) continue;
            hdr = (DRVRHeaderPtr)*drvrH;
        } else {
            hdr = (DRVRHeaderPtr)dce->dCtlDriver;
        }

        if (hdr == NULL) continue;
        if (PStrEqualLocal((const unsigned char *)&hdr->drvrName[0], name)) {
            *outRef = ~i;
            return noErr;
        }
    }
    return fnfErr;
}

/*
 * Try each candidate driver name in order.
 * If one is found, logs the match and returns its ref num in state->driverRefNum.
 * Caller passes the already-open log file ref (0 = no log).
 */
static void FindAndLogDriver(ExtState *state, short logRef)
{
    unsigned char n0[6] = {5, '.', 'E', 'N', 'E', 'T'};
    unsigned char n1[7] = {6, '.', 'E', 'N', 'E', 'T', '0'};
    unsigned char n2[7] = {6, '.', 'E', 'N', 'E', 'T', '1'};
    unsigned char n3[7] = {6, '.', 'E', 'N', 'E', 'T', '2'};
    unsigned char n4[7] = {6, '.', 'E', 'N', 'E', 'T', '3'};
    short ref = 0;

    if (FindDriverLocal(n0, &ref) == noErr) {
        if (logRef) { LogStr(logRef, "Driver .ENET ref ");   LogShort(logRef, ref); LogStr(logRef, "\r"); }
    } else if (FindDriverLocal(n1, &ref) == noErr) {
        if (logRef) { LogStr(logRef, "Driver .ENET0 ref ");  LogShort(logRef, ref); LogStr(logRef, "\r"); }
    } else if (FindDriverLocal(n2, &ref) == noErr) {
        if (logRef) { LogStr(logRef, "Driver .ENET1 ref ");  LogShort(logRef, ref); LogStr(logRef, "\r"); }
    } else if (FindDriverLocal(n3, &ref) == noErr) {
        if (logRef) { LogStr(logRef, "Driver .ENET2 ref ");  LogShort(logRef, ref); LogStr(logRef, "\r"); }
    } else if (FindDriverLocal(n4, &ref) == noErr) {
        if (logRef) { LogStr(logRef, "Driver .ENET3 ref ");  LogShort(logRef, ref); LogStr(logRef, "\r"); }
    } else {
        ref = 0;
        if (logRef) LogStr(logRef, "Driver not found\r");
    }

    state->driverRefNum = ref;
}

/* ── task-level reinit (Notification Manager nmResp, no A5) ─────────────── */

/*
 * Called by the Notification Manager at task level when needsReinit is set.
 * All state accessed via nmReqPtr->nmRefCon — no A5 required.
 *
 * The Notification Manager passes NMRecPtr on the stack (pascal convention).
 * On 68k this matches the standard C calling convention for a single pointer
 * argument, so no asm glue is needed.
 */
static pascal void ReinitViaNotification(NMRecPtr nmReqPtr)
{
    ExtState  *state = (ExtState *)nmReqPtr->nmRefCon;
    short      ref   = state->driverRefNum;
    short      logRef;
    OSErr      killErr, ctrlErr, ctrl1Err;
    CntrlParam cpb;

    /* Clear the pending flag before doing any work so that a rapid
     * sleep/wake cycle during reinit can queue another notification. */
    state->nmPending = 0;
    state->wakeCount++;
    logRef = LogOpen(state);

    if (logRef) {
        /* On the very first wake, emit a boot marker so the log shows when
         * the INIT was active (boot logging was removed to avoid triggering
         * heap compaction at extension-load time). */
        if (state->wakeCount == 1)
            LogStr(logRef, "=== BlueSCSI Sleep INIT active ===\r");
        LogStr(logRef, "=== Wake ");
        LogShort(logRef, state->wakeCount);
        LogStr(logRef, " ===\r");
    }

    if (ref == 0) {
        /* Lazy discovery: driver may not have been open at boot time. */
        if (logRef) LogStr(logRef, "Retrying driver search...\r");
        FindAndLogDriver(state, logRef);
        ref = state->driverRefNum;
    }

    if (ref != 0) {
        killErr = KillIO(ref);

        memset(&cpb, 0, sizeof(cpb));
        cpb.ioCRefNum = ref;
        cpb.csCode    = 0;
        ctrlErr = PBControlSync((ParmBlkPtr)&cpb);

        /* csCode=0 is confirmed not to restore connectivity; try csCode=1
         * (Initialize — mirrors what the driver does at open time). */
        memset(&cpb, 0, sizeof(cpb));
        cpb.ioCRefNum = ref;
        cpb.csCode    = 1;
        ctrl1Err = PBControlSync((ParmBlkPtr)&cpb);

        if (logRef) {
            LogStr(logRef, "KillIO=");
            LogShort(logRef, killErr);
            LogStr(logRef, " ctrl0=");
            LogShort(logRef, ctrlErr);
            LogStr(logRef, " ctrl1=");
            LogShort(logRef, ctrl1Err);
            LogStr(logRef, "\r");
        }
    } else {
        if (logRef) LogStr(logRef, "No driver found, skipping reinit\r");
    }

    if (logRef) LogClose(logRef, state->logSpec.vRefNum);

    NMRemove(nmReqPtr);
}

/* ── Sleep Queue callback (interrupt level) ──────────────────────────────── */

long SleepQCallbackImpl(long message, SleepQRecPtr qRecPtr)
{
    /* SleepQRec is the first field of ExtState, so qRecPtr == &state */
    ExtState *state = (ExtState *)qRecPtr;
    if (message == sleepWakeUp && !state->nmPending) {
        state->nmPending = 1;
        NMInstall(&state->nmRec);   /* NMInstall is documented interrupt-safe */
    }
    return 0;
}

asm(
    ".text\n"
    ".align 2\n"
    "SleepQGlue:\n"
    "    move.l %a0, -(%sp)\n"        /* push qRecPtr as 2nd arg */
    "    move.l %d0, -(%sp)\n"        /* push message as 1st arg */
    "    jsr SleepQCallbackImpl\n"    /* result in D0 */
    "    addq.l #8, %sp\n"
    "    rts\n"
);
extern long SleepQGlue(void);

/* ── INIT entry point ────────────────────────────────────────────────────── */

void _start(void)
{
    ExtState *state;

    RETRO68_RELOCATE();
    Retro68CallConstructors();

    /* Allocate all mutable state in System heap — survives after _start() returns.
     *
     * IMPORTANT: _start() intentionally does no file I/O.  Opening a file at
     * boot time allocates File Control Blocks as relocatable System heap blocks.
     * That allocation can trigger heap compaction, which moves other extensions'
     * trap handlers to new addresses while the trap table still holds the old
     * ones — leaving stale pointers that crash on the next trap dispatch.
     * All logging is deferred to task level (first wake via ReinitViaNotification). */
    state = (ExtState *)NewPtrSysClear(sizeof(ExtState));
    if (state == NULL) goto done;

    /* Initialise the Notification Manager record.
     * Silent notification — no mark, no icon, no sound, no alert string.
     * nmResp is called at task level after each sleepWakeUp. */
    state->nmRec.qType    = nmType;
    state->nmRec.nmMark   = 0;
    state->nmRec.nmIcon   = NULL;
    state->nmRec.nmSound  = NULL;
    state->nmRec.nmStr    = NULL;
    state->nmRec.nmResp   = (NMUPP)ReinitViaNotification;
    state->nmRec.nmRefCon = (long)state;

    /* Install Sleep Queue entry */
    state->sleepRec.sleepQType = sleepQType;            /* must be 16 */
    state->sleepRec.sleepQProc = (SleepQUPP)SleepQGlue;
    SleepQInstall(&state->sleepRec);

done:
    Retro68FreeGlobals();
}
