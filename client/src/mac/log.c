#include "log.h"
#include "resources.h"
#include <Files.h>
#include <OSUtils.h>
#include <Processes.h>
#include <string.h>


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

void log_new_session(LogFile *lf, const char *project, short digit)
{
    unsigned long secs;
    DateTimeRec dt;
    short i, n = (short)strlen(project);
    log_close(lf);
    GetDateTime(&secs);
    SecondsToDate(secs, &dt);
    /* HFS names cannot contain colons: "project12345 11-24 10.31.05 #2". */
    if (n > 12)
        n = 12;
    memcpy(lf->name + 1, project, n);
    for (i = 1; i <= n; i++)
        if (lf->name[i] == ':')
            lf->name[i] = '-';
    lf->name[0] = (unsigned char)n;
    append_str(lf->name, " ");
    append_num(lf->name, dt.month, 2);
    append_str(lf->name, "-");
    append_num(lf->name, dt.day, 2);
    append_str(lf->name, " ");
    append_num(lf->name, dt.hour, 2);
    append_str(lf->name, ".");
    append_num(lf->name, dt.minute, 2);
    append_str(lf->name, ".");
    append_num(lf->name, dt.second, 2);
    append_str(lf->name, " #");
    append_num(lf->name, digit, 1);
    lf->have_name = true;
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

static OSErr open_log(LogFile *lf)
{
    short vRef;
    long dirID, logDir;
    FSSpec spec;
    CInfoPBRec pb;
    OSErr err;
    Str63 folder;

    if (!lf->have_name)
        return fnfErr;
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

    err = FSMakeFSSpec(vRef, logDir, lf->name, &spec);
    if (err == fnfErr)
        err = FSpCreate(&spec, 'ttxt', 'TEXT', smSystemScript);
    if (err)
        return err;
    err = FSpOpenDF(&spec, fsRdWrPerm, &lf->ref);
    if (err) {
        lf->ref = 0;
        return err;
    }
    return SetFPos(lf->ref, fsFromLEOF, 0);
}

OSErr log_append(LogFile *lf, const char *text, long len)
{
    OSErr err;
    long n = len;
    if (len <= 0)
        return noErr;
    if (!lf->ref) {
        err = open_log(lf);
        if (err)
            return err;
    }
    err = FSWrite(lf->ref, &n, text);
    if (err == noErr && n != len)
        err = ioErr;
    return err;
}

void log_close(LogFile *lf)
{
    if (lf->ref) {
        FSClose(lf->ref);
        lf->ref = 0;
    }
}
