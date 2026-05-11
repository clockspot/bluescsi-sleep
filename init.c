/*
 * BlueSCSI DaynaPORT sleep/wake handler — System Extension (INIT)
 *
 * Loads at boot like any INIT in the Extensions folder.  No user
 * interaction needed: the driver is reinitialised automatically every
 * time the PowerBook wakes from sleep.
 *
 * Design
 * ──────
 * Two OS hooks are installed in _start():
 *
 *   Sleep Queue  – catches sleepWakeUp at interrupt level.  Calls
 *                  NMInstall to post a silent Notification Manager
 *                  request (NMInstall is documented interrupt-safe).
 *
 *   nmResp       – the Notification Manager invokes this callback at
 *                  task level (safe for all Toolbox calls).  Performs
 *                  KillIO + PBControlSync, then calls NMRemove.
 *
 * Code lifetime: copy-and-relocate
 * ────────────────────────────────
 * On System 7.5.5 with Retro68's --mac-flat INIT layout, the 'INIT'
 * code resource does NOT survive past _start() returning — even with
 * Get1Resource + DetachResource + HLock + HNoPurge, the block is
 * reclaimed during boot and the memory is reused (in our testing, by a
 * 'sfnt' font resource).  The OS then dispatches sleep/wake callbacks
 * into garbage and crashes.
 *
 * To work around this, _start() copies the text+data of the resource
 * into a System-heap allocation and walks the absolute relocation
 * records to fix kind-0/code and kind-1/data references inside the
 * copy.  The Sleep Queue and Notification Manager hooks are then
 * installed with the COPIED addresses — so the OS dispatches into the
 * System-heap block, which is independent of the resource map and
 * lives for the duration of the session.
 *
 * Internal calls inside the relocated copy work because:
 *   • Absolute long JSRs (kind-0) point into the copy after fixup.
 *   • PC-relative branches survive a verbatim copy unchanged (both
 *     source and target moved by the same delta).
 *   • OS trap calls (A-traps) dispatch through the trap table and
 *     don't depend on our code's location.
 *
 * A5-free design
 * ──────────────
 * After _start() returns, Retro68's A5 world is freed.  All callbacks
 * therefore access state exclusively through:
 *   • An ExtState block in the System heap (NewPtrSysClear).
 *   • SleepQRec is the first field of ExtState, so the sleep callback
 *     receives qRecPtr == &state — no A5 needed.
 *   • nmRec.nmRefCon holds the ExtState pointer for the nmResp callback.
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
#include <Resources.h>
#include <string.h>
#include "Retro68Runtime.h"

/* Linker symbols delimit the relocatable image (text + data).
 * Relocation records sit immediately after _edata in the resource. */
extern unsigned char _stext, _edata;

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

/* Write a single byte as two uppercase hex chars. */
static void LogHex8(short ref, unsigned char v)
{
    static const char hex[] = "0123456789ABCDEF";
    char  buf[2];
    long  n = 2;
    buf[0] = hex[(v >> 4) & 0xF];
    buf[1] = hex[v & 0xF];
    FSWrite(ref, &n, buf);
}

/* Write a sequence of bytes as colon-separated hex (e.g. "00:80:48:11:22:33"). */
static void LogHexBytes(short ref, const unsigned char *p, short len)
{
    short i;
    for (i = 0; i < len; i++) {
        if (i > 0) { long n = 1; FSWrite(ref, &n, (Ptr)":"); }
        LogHex8(ref, p[i]);
    }
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

/* ── driver status probe ─────────────────────────────────────────────────── */

/*
 * Issue PBStatusSync with csCode = 1.  On Apple-protocol .ENET drivers
 * this is "get hardware address" and returns the 6-byte MAC in csParam.
 * For other drivers it may return something else — we log the raw bytes
 * either way so the response is interpretable from the log.
 *
 * outBytes must point to a 16-byte buffer.  Returns the OSErr from
 * PBStatusSync (noErr on success).
 */
static OSErr ProbeDriverStatus(short ref, unsigned char outBytes[16])
{
    CntrlParam cpb;
    OSErr      err;

    memset(&cpb, 0, sizeof(cpb));
    cpb.ioCRefNum = ref;
    cpb.csCode    = 1;
    err = PBStatusSync((ParmBlkPtr)&cpb);
    memcpy(outBytes, (const void *)&cpb.csParam[0], 16);
    return err;
}

/*
 * Helper: probe driver, log a labelled line of err + raw bytes.
 * Safe to call only with a valid log ref and non-zero driver ref.
 */
static void LogDriverProbe(short logRef, short ref, const char *label)
{
    unsigned char buf[16];
    OSErr         err = ProbeDriverStatus(ref, buf);

    LogStr(logRef, label);
    LogStr(logRef, " err=");
    LogShort(logRef, err);
    LogStr(logRef, " bytes=");
    LogHexBytes(logRef, buf, 16);
    LogStr(logRef, "\r");
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
        /* Probe the driver before doing anything — captures whether the
         * driver is responsive in its post-wake state.  Compare against
         * the pre-sleep probe (logged on sleepDemand). */
        if (logRef) LogDriverProbe(logRef, ref, "Pre-reset");

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

            /* Probe again to see whether the reset altered the driver's
             * observable state. */
            LogDriverProbe(logRef, ref, "Post-reset");
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

    /* sleepDemand fires at task level just before the machine actually
     * sleeps.  Probe the driver here to capture a known-good baseline. */
    if (message == sleepDemand) {
        short logRef = LogOpen(state);
        short ref    = state->driverRefNum;

        if (ref == 0) FindAndLogDriver(state, logRef);
        ref = state->driverRefNum;

        if (logRef) {
            LogStr(logRef, "=== Sleep (about to wake ");
            LogShort(logRef, (short)(state->wakeCount + 1));
            LogStr(logRef, ") ===\r");
            if (ref != 0) LogDriverProbe(logRef, ref, "Pre-sleep");
            else          LogStr(logRef, "No driver found at sleep time\r");
            LogClose(logRef, state->logSpec.vRefNum);
        }
    }

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

/* ── relocate-the-copy helper ────────────────────────────────────────────── */

/*
 * Walk Retro68's first (absolute) relocation pass on a freshly copied
 * text+data block, adding `delta` to each kind-0/code and kind-1/data
 * longword so internal absolute references point into the copy.
 *
 * Format (see libretro/relocate.c): a uleb128 stream terminated by a 0
 * byte; each record encodes (offset_increment << 2) | kind.  kind 0=code,
 * 1=data, 2=bss, 3=jump-table.
 *
 * BSS (kind 2) is shared with the original allocation, so we leave those
 * longwords alone.  Jump-table (kind 3) does not apply to flat-mac code
 * resources.  We skip the relative-relocation pass entirely: PC-relative
 * offsets within the block are preserved by a verbatim copy.
 */
static void RelocateCopyAbs(unsigned char *newCode, unsigned char *reloc,
                            long delta)
{
    unsigned char *addrPtr = newCode - 1;
    while (*reloc) {
        unsigned long val = 0;
        int           shift = 0;
        unsigned char b;
        unsigned long kind;
        do {
            b = *reloc++;
            val |= (unsigned long)(b & 0x7F) << shift;
            shift += 7;
        } while (b & 0x80);
        addrPtr += val >> 2;
        kind = val & 0x3;
        if (kind == 0 || kind == 1) {
            unsigned long a;
            a  = ((unsigned long)addrPtr[0]) << 24;
            a |= ((unsigned long)addrPtr[1]) << 16;
            a |= ((unsigned long)addrPtr[2]) << 8;
            a |=  (unsigned long)addrPtr[3];
            a += (unsigned long)delta;
            addrPtr[0] = (unsigned char)(a >> 24);
            addrPtr[1] = (unsigned char)(a >> 16);
            addrPtr[2] = (unsigned char)(a >> 8);
            addrPtr[3] = (unsigned char) a;
        }
    }
}

/* ── INIT entry point ────────────────────────────────────────────────────── */

void _start(void)
{
    ExtState      *state;
    unsigned char *origBase, *newCode;
    long           textDataSize, delta;
    void          *copiedGlue, *copiedNmResp;

    RETRO68_RELOCATE();
    Retro68CallConstructors();

    /* Copy text+data into a System-heap block and fix up absolute
     * relocations so callbacks dispatched after _start() exits land in
     * the copy — the original 'INIT' resource doesn't survive boot. */
    origBase     = &_stext;
    textDataSize = (long)(&_edata - &_stext);
    newCode = (unsigned char *)NewPtrSysClear(textDataSize);
    if (newCode == NULL) goto done;
    BlockMoveData((Ptr)origBase, (Ptr)newCode, textDataSize);
    delta = (long)newCode - (long)origBase;
    RelocateCopyAbs(newCode, &_edata, delta);

    copiedGlue   = (void *)((char *)&SleepQGlue            + delta);
    copiedNmResp = (void *)((char *)&ReinitViaNotification + delta);

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
    state->nmRec.nmResp   = (NMUPP)copiedNmResp;
    state->nmRec.nmRefCon = (long)state;

    /* Install Sleep Queue entry */
    state->sleepRec.sleepQType = sleepQType;            /* must be 16 */
    state->sleepRec.sleepQProc = (SleepQUPP)copiedGlue;
    SleepQInstall(&state->sleepRec);

done:
    Retro68FreeGlobals();
}
