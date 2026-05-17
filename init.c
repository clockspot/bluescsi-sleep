/*
 * BlueSCSI DaynaPORT sleep/wake handler — System Extension (INIT)
 *
 * On wake from sleep, sends Dayna SCSI/Link command 0x0E (enable
 * variant) directly to the BlueSCSI's SCSI ID.  This clears the
 * inbound packet queue in BlueSCSI's firmware and re-asserts its
 * enabled flag, restoring network connectivity that would otherwise
 * hang at OTOpenInternetServices with kEHOSTUNREACHErr (-3259) for
 * ~90 seconds before timing out.
 *
 * Build variants
 * ──────────────
 *   • Production (default): silent on success.  Errors are reported
 *     to the user via a Notification Manager modal alert (Apple
 *     menu blinks; alert appears next time user yields to the OS).
 *
 *   • Diagnostic (compile with -DBLUESCSI_SLEEP_DIAGNOSTIC=1):
 *     same alert behaviour PLUS a verbose log to "BlueSCSI Sleep
 *     Log" in the System Folder — every wake, the bus walk on
 *     first wake, and the SCSI 0x0E result on each wake.
 *
 * Architecture
 * ────────────
 *   • _start() installs a Sleep Queue entry whose callback
 *     (interrupt level) posts a Notification Manager request on
 *     sleepWakeUp.
 *   • The NM callback (task level) issues SCSI 0x0E enable via the
 *     synchronous SCSI Manager directly, bypassing the .ENET driver
 *     and OT entirely.
 *   • SCSI ID is auto-discovered on first wake by walking IDs 0–6
 *     with INQUIRY (0x12), matching "Dayna" or "SCSI/Link" in the
 *     vendor or product field.  Cached for subsequent wakes.
 *   • On error, a second NM record is installed with nmStr set to
 *     a constructed Pascal-string message — the OS displays it as a
 *     modal alert.  errPending guards against duplicate queueing.
 *     Discovery failures are sticky (one alert per session) so an
 *     unconfigured install does not spam the user every wake.
 *
 * Code lifetime: copy-and-relocate
 * ────────────────────────────────
 * Under Retro68's --mac-flat INIT layout, the 'INIT' code resource
 * does not survive _start() returning even with DetachResource +
 * HLock + HNoPurge — the OS reuses the memory after boot.  _start()
 * therefore copies text+data to a System-heap allocation, walks
 * Retro68's absolute relocation records to fix kind-0/code and
 * kind-1/data references inside the copy, and installs callbacks
 * with the copied addresses.  See RelocateCopyAbs below for the
 * format details.
 *
 * A5-free design
 * ──────────────
 * After _start() returns, Retro68's A5 world is freed.  All
 * callbacks therefore access state exclusively through:
 *   • An ExtState block in the System heap (NewPtrSysClear).
 *   • SleepQRec is the first field, so the sleep callback receives
 *     qRecPtr == &state — no A5 needed.
 *   • Both NMRecs' nmRefCon holds the ExtState pointer.
 *
 * All log I/O (diagnostic build only) uses File Manager traps
 * directly (FSpOpenDF, FSWrite, etc.) rather than stdio, because
 * stdio depends on A5-relative globals.
 */

#include <MacTypes.h>
#include <Power.h>
#include <Files.h>
#include <Folders.h>
#include <Memory.h>
#include <Notification.h>
#include <SCSI.h>
#include <string.h>
#include "Retro68Runtime.h"

#ifndef BLUESCSI_SLEEP_DIAGNOSTIC
#define BLUESCSI_SLEEP_DIAGNOSTIC 0
#endif

/* Linker symbols delimit the relocatable image (text + data).
 * Relocation records sit immediately after _edata in the resource. */
extern unsigned char _stext, _edata;

/* ── state allocated in System heap ─────────────────────────────────────── */

/*
 * IMPORTANT: sleepRec MUST be the first field.
 * The Sleep Queue callback receives qRecPtr (== &sleepRec).
 * Because sleepRec is first, qRecPtr == (ExtState *)state, giving the
 * callback access to all fields without any A5 reference.
 */
typedef struct ExtState {
    SleepQRec       sleepRec;
    unsigned char   nmPending;        /* 1 = wake NMRec queued */
    unsigned char   errPending;       /* 1 = error alert NMRec queued */
    Boolean         discoveryAlertShown; /* sticky: don't spam on every wake */
    unsigned char   _pad;
    short           wakeCount;
    short           scsiID;           /* -1 = not yet discovered */
    void           *copiedErrorAlertResp;  /* relocated, set by _start */
    NMRec           nmRec;            /* wake reinit */
    NMRec           errNmRec;         /* error alert */
    Str255          errString;        /* Pascal string for nmStr */
#if BLUESCSI_SLEEP_DIAGNOSTIC
    Boolean         logSpecValid;
    unsigned char   _pad2;
    FSSpec          logSpec;
#endif
} ExtState;

/* ── Pascal-string helpers (for alert messages) ─────────────────────────── */

static void PStrSet(Str255 dst, const char *src)
{
    unsigned char n = 0;
    while (src[n] && n < 255) {
        dst[n + 1] = (unsigned char)src[n];
        n++;
    }
    dst[0] = n;
}

static void PStrAppendC(Str255 dst, const char *src)
{
    unsigned char len = dst[0];
    while (*src && len < 255) {
        dst[len + 1] = (unsigned char)*src++;
        len++;
    }
    dst[0] = len;
}

static void PStrAppendShort(Str255 dst, short val)
{
    char           buf[7];
    short          i = 7;
    Boolean        neg = (val < 0);
    unsigned short u;
    unsigned char  len;

    u = neg ? (unsigned short)(0u - (unsigned short)val) : (unsigned short)val;
    if (u == 0) buf[--i] = '0';
    else        while (u) { buf[--i] = (char)('0' + u % 10); u /= 10; }
    if (neg) buf[--i] = '-';

    len = dst[0];
    while (i < 7 && len < 255) {
        dst[len + 1] = (unsigned char)buf[i++];
        len++;
    }
    dst[0] = len;
}

/* ── Notification Manager error alert ────────────────────────────────────── */

/*
 * NM response for the error alert.  Fires at task level after the
 * user dismisses the alert dialog.  Clears errPending so subsequent
 * errors can re-alert.
 */
static pascal void ErrorAlertResp(NMRecPtr nmReqPtr)
{
    ExtState *state = (ExtState *)nmReqPtr->nmRefCon;
    state->errPending = 0;
    NMRemove(nmReqPtr);
}

/*
 * Build a Pascal string from the given prefix, optional err code,
 * optional stat byte, optional suffix; queue an NM alert.  If an
 * error alert is already queued, this call is silently ignored
 * (we don't stack multiple alerts).
 *
 * `appendErr` true ⇒ append " (err=%d stat=%d)" using err and stat.
 */
static void PostErrorAlert(ExtState *state,
                           const char *prefix,
                           Boolean appendErr, OSErr err, short stat,
                           const char *suffix)
{
    if (state->errPending) return;

    PStrSet(state->errString, prefix);
    if (appendErr) {
        PStrAppendC(state->errString, " (err=");
        PStrAppendShort(state->errString, (short)err);
        PStrAppendC(state->errString, " stat=");
        PStrAppendShort(state->errString, stat);
        PStrAppendC(state->errString, ")");
    }
    if (suffix) PStrAppendC(state->errString, suffix);

    state->errNmRec.qType    = nmType;
    state->errNmRec.nmMark   = 0;
    state->errNmRec.nmIcon   = NULL;
    state->errNmRec.nmSound  = NULL;
    state->errNmRec.nmStr    = state->errString;
    state->errNmRec.nmResp   = (NMUPP)state->copiedErrorAlertResp;
    state->errNmRec.nmRefCon = (long)state;

    state->errPending = 1;
    NMInstall(&state->errNmRec);
}

/* ── A5-free logging helpers (diagnostic build only) ────────────────────── */

#if BLUESCSI_SLEEP_DIAGNOSTIC

static short LogOpen(ExtState *state)
{
    short ref;
    long  eof;
    OSErr err;

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

    FSpCreate(&state->logSpec, 'ttxt', 'TEXT', 0 /* smSystemScript */);

    err = FSpOpenDF(&state->logSpec, fsRdWrPerm, &ref);
    if (err != noErr) return 0;

    GetEOF(ref, &eof);
    SetFPos(ref, fsFromStart, eof);
    return ref;
}

static void LogClose(short ref, short vRefNum)
{
    FSClose(ref);
    FlushVol(NULL, vRefNum);
}

static void LogStr(short ref, const char *s)
{
    long        n = 0;
    const char *p = s;
    while (*p++) n++;
    if (n > 0) FSWrite(ref, &n, (Ptr)s);
}

static void LogShort(short ref, short val)
{
    char           buf[7];
    short          i = 7;
    Boolean        neg = (val < 0);
    unsigned short u;
    long           n;

    u = neg ? (unsigned short)(0u - (unsigned short)val) : (unsigned short)val;
    if (u == 0) buf[--i] = '0';
    else        while (u) { buf[--i] = (char)('0' + u % 10); u /= 10; }
    if (neg) buf[--i] = '-';

    n = 7 - i;
    FSWrite(ref, &n, buf + i);
}

#endif /* BLUESCSI_SLEEP_DIAGNOSTIC */

/* ── SCSI Manager helpers (old/sync API; inline _SCSIDispatch traps) ────── */

/*
 * Issue a SCSI command with no data transfer.  Task level only.
 * SCSIGet serialises with any in-flight .ENET driver I/O.
 */
static OSErr ScsiCmdNoData(short id, const unsigned char *cdb, short cdbLen,
                           short *outStat)
{
    OSErr err, getErr;
    short stat = 0, msg = 0;

    *outStat = 0;

    getErr = SCSIGet();
    if (getErr != noErr) return getErr;

    err = SCSISelect(id);
    if (err == noErr) err = SCSICmd((Ptr)cdb, cdbLen);

    {
        OSErr compErr = SCSIComplete(&stat, &msg, 60 /* ticks ≈ 1 s */);
        *outStat = stat;
        if (err == noErr) err = compErr;
    }
    return err;
}

/*
 * Issue SCSI INQUIRY (0x12) and read the first 36 bytes into outBuf.
 * 2-instruction TIB: scInc to transfer 36 bytes, then scStop.
 */
static OSErr ScsiInquiry(short id, unsigned char outBuf[36], short *outStat)
{
    OSErr         err, getErr;
    short         stat = 0, msg = 0;
    SCSIInstr     tib[2];
    unsigned char cdb[6];

    *outStat = 0;
    memset(outBuf, 0, 36);

    cdb[0] = 0x12; cdb[1] = 0; cdb[2] = 0; cdb[3] = 0; cdb[4] = 36; cdb[5] = 0;

    tib[0].scOpcode = scInc;
    tib[0].scParam1 = (long)outBuf;
    tib[0].scParam2 = 36;
    tib[1].scOpcode = scStop;
    tib[1].scParam1 = 0;
    tib[1].scParam2 = 0;

    getErr = SCSIGet();
    if (getErr != noErr) return getErr;

    err = SCSISelect(id);
    if (err == noErr) err = SCSICmd((Ptr)cdb, 6);
    if (err == noErr) err = SCSIRead((Ptr)tib);

    {
        OSErr compErr = SCSIComplete(&stat, &msg, 60);
        *outStat = stat;
        if (err == noErr) err = compErr;
    }
    return err;
}

/*
 * Issue Dayna SCSI/Link 0x0E "toggle interface".  BlueSCSI firmware
 * (lib/SCSI2SD/src/firmware/network.c) checks cdb[5] & 0x80:
 *
 *   cdb[5] = 0x80  → scsiNetworkEnabled = true; inbound queue cleared
 *   cdb[5] = 0x00  → scsiNetworkEnabled = false
 *
 * No WiFi reset, no deeper buffer reinit — just the boolean flag and
 * the inbound queue, which is sufficient to recover the wedge that
 * survives across PowerBook sleep.
 */
static OSErr ScsiToggleInterface(short id, Boolean enable, short *outStat)
{
    unsigned char cdb[6];
    cdb[0] = 0x0E;
    cdb[1] = 0;
    cdb[2] = 0;
    cdb[3] = 0;
    cdb[4] = 0;
    cdb[5] = enable ? 0x80 : 0x00;
    return ScsiCmdNoData(id, cdb, 6, outStat);
}

/* Case-insensitive substring match against a space-padded ASCII field. */
static Boolean InqContains(const unsigned char *field, short fieldLen,
                           const char *needle)
{
    short i, j, n = 0;
    while (needle[n]) n++;
    if (n == 0 || n > fieldLen) return false;
    for (i = 0; i <= fieldLen - n; i++) {
        for (j = 0; j < n; j++) {
            unsigned char a = field[i + j];
            unsigned char b = (unsigned char)needle[j];
            if (a >= 'a' && a <= 'z') a -= 32;
            if (b >= 'a' && b <= 'z') b -= 32;
            if (a != b) break;
        }
        if (j == n) return true;
    }
    return false;
}

/*
 * Walk SCSI IDs 0–6 looking for the BlueSCSI DaynaPORT emulation.
 * Returns the first matching ID, or -1 if none found.  In diagnostic
 * builds, logs each inquiry response when logRef is non-zero.
 *
 * Heuristic: match "DAYNA" or "SCSI/LINK" in vendor or product.
 * Real BlueSCSI hardware reports vendor="Dayna" product="SCSI/Link"
 * with peripheral type 0x03 (processor device).  HDD emulations
 * legitimately put "BlueSCSI" in their product field — we
 * deliberately don't match on that string, lest we toggle the wrong
 * target.
 */
static short DiscoverBlueScsiID(
#if BLUESCSI_SLEEP_DIAGNOSTIC
    short logRef
#else
    void
#endif
)
{
    short         id, found = -1;
    unsigned char inq[36];
    OSErr         err;
    short         stat;

    for (id = 0; id <= 6; id++) {
        err = ScsiInquiry(id, inq, &stat);

#if BLUESCSI_SLEEP_DIAGNOSTIC
        if (logRef) {
            LogStr(logRef, "  id="); LogShort(logRef, id);
            LogStr(logRef, " err="); LogShort(logRef, err);
            LogStr(logRef, " stat=");LogShort(logRef, stat);
        }
#endif

        if (err == noErr && stat == 0) {
            unsigned char *vendor = &inq[8];
            unsigned char *prod   = &inq[16];
            Boolean match = InqContains(vendor, 8,  "DAYNA")   ||
                            InqContains(prod,   16, "SCSI/LINK")||
                            InqContains(prod,   16, "DAYNA");

#if BLUESCSI_SLEEP_DIAGNOSTIC
            if (logRef) {
                long n8 = 8, n16 = 16;
                LogStr(logRef, " v=\""); FSWrite(logRef, &n8,  (Ptr)vendor);
                LogStr(logRef, "\" p=\"");FSWrite(logRef, &n16, (Ptr)prod);
                LogStr(logRef, "\"");
            }
#endif
            if (match && found < 0) {
                found = id;
#if BLUESCSI_SLEEP_DIAGNOSTIC
                if (logRef) LogStr(logRef, " <- match");
#endif
            }
        }
#if BLUESCSI_SLEEP_DIAGNOSTIC
        if (logRef) LogStr(logRef, "\r");
#endif
    }
    return found;
}

/* ── task-level callback (Notification Manager nmResp, no A5) ──────────── */

static pascal void ReinitViaNotification(NMRecPtr nmReqPtr)
{
    ExtState *state = (ExtState *)nmReqPtr->nmRefCon;
    OSErr     scsiErr;
    short     scsiStat;
#if BLUESCSI_SLEEP_DIAGNOSTIC
    short     logRef = 0;
#endif

    /* Clear pending flag before doing work so a rapid sleep/wake cycle
     * during reinit can queue another notification. */
    state->nmPending = 0;
    state->wakeCount++;

    /* First wake: discover BlueSCSI ID.  In diagnostic builds, also
     * log the bus walk so the user can verify the right target was
     * selected.  Diagnostic builds additionally log a "=== Wake N ==="
     * marker on every wake. */
#if BLUESCSI_SLEEP_DIAGNOSTIC
    logRef = LogOpen(state);
    if (logRef) {
        if (state->wakeCount == 1) {
            LogStr(logRef, "=== BlueSCSI Sleep INIT (diagnostic) active ===\r");
        }
        LogStr(logRef, "=== Wake ");
        LogShort(logRef, state->wakeCount);
        LogStr(logRef, " ===\r");
    }
    if (state->scsiID < 0) {
        if (logRef) LogStr(logRef, "Walking SCSI bus:\r");
        state->scsiID = DiscoverBlueScsiID(logRef);
        if (logRef) {
            if (state->scsiID >= 0) {
                LogStr(logRef, "Selected SCSI ID="); LogShort(logRef, state->scsiID);
                LogStr(logRef, "\r");
            } else {
                LogStr(logRef, "BlueSCSI DaynaPORT not detected.\r");
            }
        }
    }
#else
    if (state->scsiID < 0) {
        state->scsiID = DiscoverBlueScsiID();
    }
#endif

    /* Discovery alert is sticky (once per session). */
    if (state->scsiID < 0 && !state->discoveryAlertShown) {
        state->discoveryAlertShown = true;
        PostErrorAlert(state,
            "BlueSCSI Sleep INIT: DaynaPORT not detected on the SCSI bus. "
            "INIT will retry on each wake.",
            false, 0, 0, NULL);
    }

    /* Wake-time reinit: SCSI 0x0E enable.  Bypasses the .ENET driver
     * and OT — speaks straight to the BlueSCSI firmware. */
    if (state->scsiID >= 0) {
        scsiErr = ScsiToggleInterface(state->scsiID, true, &scsiStat);
#if BLUESCSI_SLEEP_DIAGNOSTIC
        if (logRef) {
            LogStr(logRef, "SCSI 0x0E enable id="); LogShort(logRef, state->scsiID);
            LogStr(logRef, " err=");                LogShort(logRef, scsiErr);
            LogStr(logRef, " stat=");               LogShort(logRef, scsiStat);
            LogStr(logRef, "\r");
        }
#endif
        if (scsiErr != noErr || scsiStat != 0) {
            PostErrorAlert(state,
                "BlueSCSI Sleep INIT: SCSI 0x0E enable failed.",
                true, scsiErr, scsiStat,
                "  Network may not recover until restart.");
        }
    }

#if BLUESCSI_SLEEP_DIAGNOSTIC
    if (logRef) LogClose(logRef, state->logSpec.vRefNum);
#endif

    NMRemove(nmReqPtr);
}

/* ── Sleep Queue callback (interrupt level) ──────────────────────────────── */

long SleepQCallbackImpl(long message, SleepQRecPtr qRecPtr)
{
    ExtState *state = (ExtState *)qRecPtr;  /* sleepRec at offset 0 */

    if (message == sleepWakeUp && !state->nmPending) {
        state->nmPending = 1;
        NMInstall(&state->nmRec);   /* documented interrupt-safe */
    }
    return 0;
}

asm(
    ".text\n"
    ".align 2\n"
    "SleepQGlue:\n"
    "    move.l %a0, -(%sp)\n"        /* push qRecPtr as 2nd arg */
    "    move.l %d0, -(%sp)\n"        /* push message as 1st arg */
    "    jsr SleepQCallbackImpl\n"
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
 * Format (see libretro/relocate.c): uleb128 stream terminated by a 0
 * byte; each record encodes (offset_increment << 2) | kind.
 * kind 0=code, 1=data, 2=bss, 3=jump-table.
 *
 * BSS (kind 2) is shared with the original allocation, so we leave
 * those longwords alone.  Jump-table (kind 3) does not apply to
 * flat-mac code resources.  We skip the relative-relocation pass
 * entirely: PC-relative offsets within the block are preserved by
 * a verbatim copy.
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
    void          *copiedGlue, *copiedNmResp, *copiedErrResp;

    RETRO68_RELOCATE();
    Retro68CallConstructors();

    /* Copy text+data into a System-heap block and fix up absolute
     * relocations so callbacks dispatched after _start() exits land
     * in the copy — the original 'INIT' resource doesn't survive
     * boot. */
    origBase     = &_stext;
    textDataSize = (long)(&_edata - &_stext);
    newCode = (unsigned char *)NewPtrSysClear(textDataSize);
    if (newCode == NULL) goto done;
    BlockMoveData((Ptr)origBase, (Ptr)newCode, textDataSize);
    delta = (long)newCode - (long)origBase;
    RelocateCopyAbs(newCode, &_edata, delta);

    copiedGlue    = (void *)((char *)&SleepQGlue            + delta);
    copiedNmResp  = (void *)((char *)&ReinitViaNotification + delta);
    copiedErrResp = (void *)((char *)&ErrorAlertResp        + delta);

    state = (ExtState *)NewPtrSysClear(sizeof(ExtState));
    if (state == NULL) goto done;
    state->scsiID              = -1;     /* lazily discovered on first wake */
    state->copiedErrorAlertResp = copiedErrResp;

    /* Silent wake NM record — no mark, no icon, no sound, no alert
     * string.  nmResp runs at task level after each sleepWakeUp. */
    state->nmRec.qType    = nmType;
    state->nmRec.nmMark   = 0;
    state->nmRec.nmIcon   = NULL;
    state->nmRec.nmSound  = NULL;
    state->nmRec.nmStr    = NULL;
    state->nmRec.nmResp   = (NMUPP)copiedNmResp;
    state->nmRec.nmRefCon = (long)state;

    /* Install Sleep Queue entry. */
    state->sleepRec.sleepQType = sleepQType;            /* must be 16 */
    state->sleepRec.sleepQProc = (SleepQUPP)copiedGlue;
    SleepQInstall(&state->sleepRec);

done:
    Retro68FreeGlobals();
}
