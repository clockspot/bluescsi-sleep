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
 * Architecture
 * ────────────
 *   • _start() installs a Sleep Queue entry whose callback (interrupt
 *     level) posts a Notification Manager request on sleepWakeUp.
 *   • The NM callback (task level) issues SCSI 0x0E enable via the
 *     synchronous SCSI Manager directly, bypassing the .ENET driver
 *     and OT entirely.
 *   • SCSI ID is auto-discovered on first wake by walking IDs 0–6
 *     with INQUIRY (0x12), matching "Dayna" or "SCSI/Link" in the
 *     vendor or product field.  Cached for subsequent wakes.
 *
 * Code lifetime: copy-and-relocate
 * ────────────────────────────────
 * Under Retro68's --mac-flat INIT layout, the 'INIT' code resource
 * does not survive _start() returning even with DetachResource +
 * HLock + HNoPurge — the OS reuses the memory after boot, leaving
 * Sleep Queue callbacks pointing at garbage.  _start() therefore
 * copies text+data to a System-heap allocation, walks Retro68's
 * absolute relocation records to fix kind-0/code and kind-1/data
 * references inside the copy, and installs the callbacks with the
 * copied addresses.
 *
 * A5-free design
 * ──────────────
 * After _start() returns, Retro68's A5 world is freed.  All
 * callbacks therefore access state exclusively through:
 *   • An ExtState block in the System heap (NewPtrSysClear).
 *   • SleepQRec is the first field, so the sleep callback receives
 *     qRecPtr == &state — no A5 needed.
 *   • nmRec.nmRefCon holds the ExtState pointer for the nmResp
 *     callback.
 *
 * Logging
 * ───────
 * "BlueSCSI Sleep Log" in the System Folder is appended on first
 * wake (one-time bus walk + selected ID) and on error (SCSI failure
 * or BlueSCSI not detected).  Silent on successful subsequent wakes,
 * so the log doesn't grow over hundreds of sleep/wake cycles.
 *
 * All log I/O uses File Manager traps directly (FSpOpenDF, FSWrite,
 * etc.) rather than stdio, because stdio depends on A5-relative
 * globals that are not available after _start() returns.
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
    unsigned char   nmPending;      /* 1 = NMRec is already in the NM queue */
    Boolean         logSpecValid;
    short           wakeCount;
    short           scsiID;         /* BlueSCSI SCSI ID; -1 = not yet discovered */
    short           _pad;
    FSSpec          logSpec;
    NMRec           nmRec;
} ExtState;

/* ── A5-free logging helpers (used for discovery + errors only) ──────────── */

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
    char           buf[7];   /* worst case: "-32768" = 6 chars */
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

/* ── SCSI Manager helpers (old/sync API; inline _SCSIDispatch traps) ────── */

/*
 * Issue a SCSI command with no data transfer.  Returns the SCSI Manager
 * error from the worst-failing call in the GET/SELECT/CMD/COMPLETE
 * sequence; *outStat receives the device-side status byte (0 = GOOD).
 *
 * Task level only.  SCSIGet serialises with any in-flight .ENET driver
 * I/O, so no explicit KillIO is needed.
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

    /* SCSIComplete must be called to release the bus, even after error. */
    {
        OSErr compErr = SCSIComplete(&stat, &msg, 60 /* ticks ≈ 1 s */);
        *outStat = stat;
        if (err == noErr) err = compErr;
    }
    return err;
}

/*
 * Issue SCSI INQUIRY (0x12) and read the first 36 bytes into outBuf.
 * Uses a 2-instruction TIB: scInc to transfer 36 bytes, then scStop.
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
 * Issue Dayna SCSI/Link 0x0E "toggle interface".  BlueSCSI's firmware
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
 * Returns the first matching ID, or -1 if none found.  If logRef is
 * non-zero, logs every inquiry response (so the user can see what's
 * on the bus); pass 0 for silent operation.
 *
 * Heuristic: match "DAYNA" or "SCSI/LINK" in vendor or product.
 * Real BlueSCSI hardware reports vendor="Dayna" product="SCSI/Link"
 * with peripheral type 0x03 (processor device).  HDD emulations on
 * the same bus legitimately put "BlueSCSI" in their product field —
 * we deliberately don't match on that string, lest we toggle the
 * wrong target.
 */
static short DiscoverBlueScsiID(short logRef)
{
    short         id, found = -1;
    unsigned char inq[36];
    OSErr         err;
    short         stat;

    for (id = 0; id <= 6; id++) {
        err = ScsiInquiry(id, inq, &stat);

        if (logRef) {
            LogStr(logRef, "  id="); LogShort(logRef, id);
            LogStr(logRef, " err="); LogShort(logRef, err);
            LogStr(logRef, " stat=");LogShort(logRef, stat);
        }

        if (err == noErr && stat == 0) {
            unsigned char *vendor = &inq[8];
            unsigned char *prod   = &inq[16];
            Boolean match = InqContains(vendor, 8,  "DAYNA")   ||
                            InqContains(prod,   16, "SCSI/LINK")||
                            InqContains(prod,   16, "DAYNA");

            if (logRef) {
                long n8 = 8, n16 = 16;
                LogStr(logRef, " v=\""); FSWrite(logRef, &n8,  (Ptr)vendor);
                LogStr(logRef, "\" p=\"");FSWrite(logRef, &n16, (Ptr)prod);
                LogStr(logRef, "\"");
            }
            if (match && found < 0) {
                found = id;
                if (logRef) LogStr(logRef, " <- match");
            }
        }
        if (logRef) LogStr(logRef, "\r");
    }
    return found;
}

/* ── task-level callback (Notification Manager nmResp, no A5) ──────────── */

static pascal void ReinitViaNotification(NMRecPtr nmReqPtr)
{
    ExtState *state = (ExtState *)nmReqPtr->nmRefCon;
    short     logRef = 0;
    OSErr     scsiErr;
    short     scsiStat;

    /* Clear the pending flag before doing any work so a rapid sleep/wake
     * cycle during reinit can queue another notification. */
    state->nmPending = 0;
    state->wakeCount++;

    /* First wake: discover the BlueSCSI ID and log the bus walk so the
     * user can confirm the right target was selected.  Subsequent wakes
     * skip this entirely. */
    if (state->scsiID < 0) {
        logRef = LogOpen(state);
        if (logRef) {
            LogStr(logRef, "=== BlueSCSI Sleep INIT active ===\r");
            LogStr(logRef, "Walking SCSI bus:\r");
        }
        state->scsiID = DiscoverBlueScsiID(logRef);
        if (logRef) {
            if (state->scsiID >= 0) {
                LogStr(logRef, "Selected SCSI ID=");
                LogShort(logRef, state->scsiID);
                LogStr(logRef, "\r");
            } else {
                LogStr(logRef, "BlueSCSI DaynaPORT not detected on SCSI bus.\r");
                LogStr(logRef, "INIT will retry discovery on next wake.\r");
            }
        }
    }

    /* Wake-time reinit: SCSI 0x0E enable.  Bypasses the .ENET driver
     * and OT — speaks straight to the BlueSCSI firmware. */
    if (state->scsiID >= 0) {
        scsiErr = ScsiToggleInterface(state->scsiID, true, &scsiStat);
        if (scsiErr != noErr || scsiStat != 0) {
            if (logRef == 0) logRef = LogOpen(state);
            if (logRef) {
                LogStr(logRef, "Wake ");      LogShort(logRef, state->wakeCount);
                LogStr(logRef, ": SCSI 0x0E enable id="); LogShort(logRef, state->scsiID);
                LogStr(logRef, " err=");      LogShort(logRef, scsiErr);
                LogStr(logRef, " stat=");     LogShort(logRef, scsiStat);
                LogStr(logRef, "\r");
            }
        }
    }

    if (logRef) LogClose(logRef, state->logSpec.vRefNum);
    NMRemove(nmReqPtr);
}

/* ── Sleep Queue callback (interrupt level) ──────────────────────────────── */

long SleepQCallbackImpl(long message, SleepQRecPtr qRecPtr)
{
    /* SleepQRec is the first field of ExtState, so qRecPtr == &state. */
    ExtState *state = (ExtState *)qRecPtr;

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
    state->scsiID = -1;   /* lazily discovered on first wake */

    /* Silent NM record — no mark, no icon, no sound, no alert string.
     * nmResp runs at task level after each sleepWakeUp. */
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
