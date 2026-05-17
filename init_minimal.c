/* Diagnostic step 9: copy-and-relocate + real NM dance, no driver work.
 *
 * Sleep callback (interrupt level): on sleepWakeUp, NMInstall a notification.
 * NM response callback (task level): increment a wake counter, write a line
 * to the log, NMRemove.
 *
 * If sleep+wake produces a "Wake N" entry in BlueSCSI Sleep Log, we've
 * validated the copy-and-relocate skeleton end-to-end. The OT probe lives
 * in the BlueSCSISleep app (main.c) so it can be re-run on demand without
 * blocking the wake handler. */
#include <MacTypes.h>
#include <Memory.h>
#include <Power.h>
#include <Devices.h>
#include <Files.h>
#include <Folders.h>
#include <Notification.h>
#include <Resources.h>
#include "Retro68Runtime.h"

extern unsigned char _stext, _edata;

typedef struct {
    SleepQRec       sleepRec;
    short           driverRefNum;
    unsigned char   nmPending;
    unsigned char   _pad;
    short           wakeCount;
    Boolean         logSpecValid;
    unsigned char   _pad2;
    FSSpec          logSpec;
    NMRec           nmRec;
} ExtState;

/* ── log helpers (task level only) ──────────────────────────────────────── */

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
    FSpCreate(&state->logSpec, 'ttxt', 'TEXT', 0);
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
    unsigned short u = neg ? (unsigned short)(0u - (unsigned short)val)
                            : (unsigned short)val;
    long           n;
    if (u == 0) buf[--i] = '0';
    else        while (u) { buf[--i] = (char)('0' + u % 10); u /= 10; }
    if (neg) buf[--i] = '-';
    n = 7 - i;
    FSWrite(ref, &n, buf + i);
}

static void LogHex32(short ref, unsigned long v)
{
    static const char hex[] = "0123456789ABCDEF";
    char  buf[8];
    long  n = 8;
    short i;
    for (i = 7; i >= 0; i--) { buf[i] = hex[v & 0xF]; v >>= 4; }
    FSWrite(ref, &n, buf);
}

/* ── task-level callback (Notification Manager) ─────────────────────────── */

static pascal void ReinitViaNotification(NMRecPtr nmReqPtr)
{
    ExtState *state = (ExtState *)nmReqPtr->nmRefCon;
    short     logRef;

    state->nmPending = 0;
    state->wakeCount++;

    logRef = LogOpen(state);
    if (logRef) {
        LogStr(logRef, "=== Wake ");
        LogShort(logRef, state->wakeCount);
        LogStr(logRef, " (step 9, no driver work) ===\r");
        LogClose(logRef, state->logSpec.vRefNum);
    }

    NMRemove(nmReqPtr);
}

/* ── interrupt-level callback (Sleep Queue) ─────────────────────────────── */

long SleepQCallbackImpl(long message, SleepQRecPtr qRecPtr)
{
    ExtState *state = (ExtState *)qRecPtr;   /* sleepRec is at offset 0 */
    if (message == sleepWakeUp && !state->nmPending) {
        state->nmPending = 1;
        NMInstall(&state->nmRec);
    }
    return 0;
}

asm(
    ".text\n"
    ".align 2\n"
    "SleepQGlue:\n"
    "    move.l %a0, -(%sp)\n"
    "    move.l %d0, -(%sp)\n"
    "    jsr SleepQCallbackImpl\n"
    "    addq.l #8, %sp\n"
    "    rts\n"
);
extern void SleepQGlue(void);

/* ── relocate-the-copy helper ────────────────────────────────────────────── */

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

/* ── INIT entry ─────────────────────────────────────────────────────────── */

void _start(void)
{
    Handle         selfH;
    ExtState      *state;
    unsigned char *origBase, *newCode;
    long           textDataSize, delta;
    void          *copiedGlue, *copiedNmResp;
    short          ref;
    long           eof;

    RETRO68_RELOCATE();
    Retro68CallConstructors();

    selfH = Get1Resource('INIT', 128);
    if (selfH == NULL) goto done;

    origBase     = &_stext;
    textDataSize = (long)(&_edata - &_stext);
    newCode = (unsigned char *)NewPtrSysClear(textDataSize);
    if (newCode == NULL) goto done;
    BlockMoveData((Ptr)origBase, (Ptr)newCode, textDataSize);
    delta = (long)newCode - (long)origBase;
    RelocateCopyAbs(newCode, &_edata, delta);

    state = (ExtState *)NewPtrSysClear(sizeof(ExtState));
    if (state == NULL) goto done;

    copiedGlue   = (void *)((char *)&SleepQGlue            + delta);
    copiedNmResp = (void *)((char *)&ReinitViaNotification + delta);

    /* Notification Manager record: silent notification, dispatched at task
     * level after each sleepWakeUp. */
    state->nmRec.qType    = nmType;
    state->nmRec.nmMark   = 0;
    state->nmRec.nmIcon   = NULL;
    state->nmRec.nmSound  = NULL;
    state->nmRec.nmStr    = NULL;
    state->nmRec.nmResp   = (NMUPP)copiedNmResp;
    state->nmRec.nmRefCon = (long)state;

    /* Sleep Queue record. */
    state->sleepRec.sleepQType = sleepQType;
    state->sleepRec.sleepQProc = (SleepQUPP)copiedGlue;
    SleepQInstall(&state->sleepRec);

    /* Boot-time log entry. */
    {
        short tmp = LogOpen(state);
        if (tmp) {
            LogStr(tmp, "=== Step 9 _start ===\r");
            LogStr(tmp, "newCode=");      LogHex32(tmp, (unsigned long)newCode);
            LogStr(tmp, " size=");        LogHex32(tmp, (unsigned long)textDataSize);
            LogStr(tmp, " glue=");        LogHex32(tmp, (unsigned long)copiedGlue);
            LogStr(tmp, " nmResp=");      LogHex32(tmp, (unsigned long)copiedNmResp);
            LogStr(tmp, "\r");
            LogClose(tmp, state->logSpec.vRefNum);
        }
        (void)ref; (void)eof;
    }

done:
    Retro68FreeGlobals();
}
