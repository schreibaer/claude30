#include "log.h"
#include "resources.h"
#include <Files.h>
#include <OSUtils.h>
#include <Processes.h>
#include <string.h>

static short gLogRef = 0;
static Str63 gLogName;
static Boolean gHaveName = false;

static void append_num(unsigned char *s, short v, short digits)
{
    char buf[8];
    short i;
    for (i = digits - 1; i >= 0; i--) {
        buf[i] = (char)('0' + v % 10);
        v /= 10;
    }
    memcpy(s + s[0] + 1, buf, digits);
    s[0] += digits;
}

static void append_str(unsigned char *s, const char *t)
{
    short n = (short)strlen(t);
    memcpy(s + s[0] + 1, t, n);
    s[0] += n;
}

void log_new_session(void)
{
    unsigned long secs;
    DateTimeRec dt;
    log_close();
    GetDateTime(&secs);
    SecondsToDate(secs, &dt);
    /* HFS names cannot contain colons: "Session 2026-11-24 10.31.05". */
    gLogName[0] = 0;
    append_str(gLogName, "Session ");
    append_num(gLogName, dt.year, 4);
    append_str(gLogName, "-");
    append_num(gLogName, dt.month, 2);
    append_str(gLogName, "-");
    append_num(gLogName, dt.day, 2);
    append_str(gLogName, " ");
    append_num(gLogName, dt.hour, 2);
    append_str(gLogName, ".");
    append_num(gLogName, dt.minute, 2);
    append_str(gLogName, ".");
    append_num(gLogName, dt.second, 2);
    gHaveName = true;
}

/* The folder that holds the application. */
static OSErr app_folder(short *vRef, long *dirID)
{
    ProcessSerialNumber psn;
    ProcessInfoRec info;
    FSSpec app;
    OSErr err;

    psn.highLongOfPSN = 0;
    psn.lowLongOfPSN = kCurrentProcess;
    memset(&info, 0, sizeof(info));
    info.processInfoLength = sizeof(info);
    info.processAppSpec = &app;
    err = GetProcessInformation(&psn, &info);
    if (err)
        return err;
    *vRef = app.vRefNum;
    *dirID = app.parID;
    return noErr;
}

static OSErr open_log(void)
{
    short vRef;
    long dirID, logDir;
    FSSpec spec;
    CInfoPBRec pb;
    OSErr err;
    Str63 folder;

    if (!gHaveName)
        log_new_session();
    err = app_folder(&vRef, &dirID);
    if (err)
        return err;

    memcpy(folder, "\pClaude30 Logs", 15);
    err = FSMakeFSSpec(vRef, dirID, folder, &spec);
    if (err == fnfErr)
        err = FSpDirCreate(&spec, smSystemScript, &logDir);
    else if (err == noErr)
        err = dupFNErr;
    if (err == dupFNErr) {
        memset(&pb, 0, sizeof(pb));
        pb.dirInfo.ioNamePtr = folder;
        pb.dirInfo.ioVRefNum = vRef;
        pb.dirInfo.ioDrDirID = dirID;
        err = PBGetCatInfoSync(&pb);
        logDir = pb.dirInfo.ioDrDirID;
    }
    if (err)
        return err;

    err = FSMakeFSSpec(vRef, logDir, gLogName, &spec);
    if (err == fnfErr)
        err = FSpCreate(&spec, 'ttxt', 'TEXT', smSystemScript);
    if (err)
        return err;
    err = FSpOpenDF(&spec, fsRdWrPerm, &gLogRef);
    if (err) {
        gLogRef = 0;
        return err;
    }
    return SetFPos(gLogRef, fsFromLEOF, 0);
}

OSErr log_append(const char *text, long len)
{
    OSErr err;
    long n = len;
    if (len <= 0)
        return noErr;
    if (!gLogRef) {
        err = open_log();
        if (err)
            return err;
    }
    return FSWrite(gLogRef, &n, text);
}

void log_close(void)
{
    if (gLogRef) {
        FSClose(gLogRef);
        gLogRef = 0;
    }
    gHaveName = false;
}
