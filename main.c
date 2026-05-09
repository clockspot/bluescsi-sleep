/*
 * BlueSCSI DaynaPORT reinit utility
 *
 * Run this after waking from sleep to restore network connectivity.
 * Shows nothing to the user; results are written to "BlueSCSI Sleep Log"
 * in the System Folder (the same file the INIT appends to).
 *
 * Usage: double-click after waking, then check the log in TeachText.
 */

#include <MacTypes.h>
#include <Quickdraw.h>
#include <Devices.h>
#include <Files.h>
#include <Folders.h>
#include <Memory.h>
#include <OSUtils.h>
#include <string.h>

/* ── low-memory Unit Table ───────────────────────────────────────────────── */

#define LMUTableBase()      (*(DCtlHandle **)0x011C)
#define LMUnitNtryCnt()     (*(short *)0x01D2)

/* ── logging ─────────────────────────────────────────────────────────────── */

static FSSpec  gLogSpec;
static Boolean gLogSpecValid = false;

static short LogOpen(void)
{
    short ref, vRef;
    long  eof, dirID;
    OSErr err;

    if (!gLogSpecValid) {
        err = FindFolder(kOnSystemDisk, kSystemFolderType,
                         kDontCreateFolder, &vRef, &dirID);
        if (err != noErr) return 0;
        err = FSMakeFSSpec(vRef, dirID,
                           (const unsigned char *)"\pBlueSCSI Sleep Log",
                           &gLogSpec);
        if (err != noErr && err != fnfErr) return 0;
        gLogSpecValid = true;
    }

    FSpCreate(&gLogSpec, 'ttxt', 'TEXT', 0);
    err = FSpOpenDF(&gLogSpec, fsRdWrPerm, &ref);
    if (err != noErr) return 0;

    GetEOF(ref, &eof);
    SetFPos(ref, fsFromStart, eof);
    return ref;
}

static void LogClose(short ref)
{
    FSClose(ref);
    FlushVol(NULL, gLogSpec.vRefNum);
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
    short          i   = 7;
    Boolean        neg = (val < 0);
    unsigned short u   = neg ? (unsigned short)(0u - (unsigned short)val)
                              : (unsigned short)val;
    long           n;

    if (u == 0) {
        buf[--i] = '0';
    } else {
        while (u) { buf[--i] = (char)('0' + u % 10); u /= 10; }
    }
    if (neg) buf[--i] = '-';
    n = 7 - i;
    FSWrite(ref, &n, buf + i);
}

/* ── driver finding ──────────────────────────────────────────────────────── */

static Boolean PStrEqual(const unsigned char *a, const unsigned char *b)
{
    unsigned char len = a[0], i;
    if (b[0] != len) return false;
    for (i = 1; i <= len; i++) {
        unsigned char ca = a[i], cb = b[i];
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return false;
    }
    return true;
}

static short FindAndLogDriver(short logRef)
{
    unsigned char n0[6] = {5, '.', 'E', 'N', 'E', 'T'};
    unsigned char n1[7] = {6, '.', 'E', 'N', 'E', 'T', '0'};
    unsigned char n2[7] = {6, '.', 'E', 'N', 'E', 'T', '1'};
    unsigned char n3[7] = {6, '.', 'E', 'N', 'E', 'T', '2'};
    unsigned char n4[7] = {6, '.', 'E', 'N', 'E', 'T', '3'};

    DCtlHandle *tbl = LMUTableBase();
    short       cnt = LMUnitNtryCnt();
    short       i;

    /* Try each name; return on first match (same priority order as INIT). */
#define TRY(name, label) \
    for (i = 0; i < cnt; i++) { \
        DCtlHandle    h   = tbl[i]; \
        DCtlPtr       dce; \
        DRVRHeaderPtr hdr; \
        Handle        drvrH; \
        if (h == NULL) continue; \
        dce = *h; \
        if (dce == NULL) continue; \
        if (!(dce->dCtlFlags & 0x0020)) continue; \
        if (dce->dCtlFlags & 0x0040) { \
            drvrH = (Handle)dce->dCtlDriver; \
            if (drvrH == NULL || *drvrH == NULL) continue; \
            hdr = (DRVRHeaderPtr)*drvrH; \
        } else { \
            hdr = (DRVRHeaderPtr)dce->dCtlDriver; \
        } \
        if (hdr == NULL) continue; \
        if (PStrEqual((const unsigned char *)&hdr->drvrName[0], name)) { \
            short ref = ~i; \
            if (logRef) { LogStr(logRef, "Driver " label " ref "); \
                          LogShort(logRef, ref); LogStr(logRef, "\r"); } \
            return ref; \
        } \
    }

    TRY(n0, ".ENET")
    TRY(n1, ".ENET0")
    TRY(n2, ".ENET1")
    TRY(n3, ".ENET2")
    TRY(n4, ".ENET3")
#undef TRY

    if (logRef) LogStr(logRef, "Driver not found\r");
    return 0;
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    short      logRef = 0, ref;
    OSErr      killErr = 0, ctrl0Err = 0, ctrl1Err = 0;
    CntrlParam cpb;

    InitGraf(&qd.thePort);
    MaxApplZone();

    logRef = LogOpen();
    if (logRef) LogStr(logRef, "=== BlueSCSI Reinit App ===\r");

    ref = FindAndLogDriver(logRef);

    if (ref != 0) {
        killErr = KillIO(ref);

        memset(&cpb, 0, sizeof(cpb));
        cpb.ioCRefNum = ref;
        cpb.csCode    = 0;
        ctrl0Err = PBControlSync((ParmBlkPtr)&cpb);

        memset(&cpb, 0, sizeof(cpb));
        cpb.ioCRefNum = ref;
        cpb.csCode    = 1;
        ctrl1Err = PBControlSync((ParmBlkPtr)&cpb);

        if (logRef) {
            LogStr(logRef, "KillIO=");   LogShort(logRef, killErr);
            LogStr(logRef, " ctrl0=");   LogShort(logRef, ctrl0Err);
            LogStr(logRef, " ctrl1=");   LogShort(logRef, ctrl1Err);
            LogStr(logRef, "\r");
        }
    }

    if (logRef) LogClose(logRef);
    return 0;
}
