/* Serial port: open in Inside Macintosh order (output driver first), set
   8N1 at the chosen speed with all handshaking off, install a 32 KB input
   buffer, and poll from the event loop. */
#include "serial.h"
#include <Devices.h>
#include <Memory.h>
#include <Serial.h>
#include <string.h>

#define INPUT_BUF_SIZE 32768L

static short gIn = 0, gOut = 0;
static Ptr gBuf = NULL;

/* Serial driver control and status codes (Inside Macintosh: Devices). */
enum {
    csSerReset = 8,     /* serial configuration word */
    csSerSetBuf = 9,    /* input buffer pointer and size */
    csSerHShake = 14,   /* handshake options, extended form */
    csSerGetBuf = 2,    /* status: bytes waiting in the input buffer */
    csSerStatus = 8     /* status: SerStaRec */
};

static OSErr control(short ref, short code, const void *param, short plen)
{
    CntrlParam pb;
    memset(&pb, 0, sizeof(pb));
    pb.ioCRefNum = ref;
    pb.csCode = code;
    memcpy(pb.csParam, param, plen);
    return PBControlSync((ParmBlkPtr)&pb);
}

static OSErr status(short ref, short code, void *out, short olen)
{
    CntrlParam pb;
    OSErr err;
    memset(&pb, 0, sizeof(pb));
    pb.ioCRefNum = ref;
    pb.csCode = code;
    err = PBStatusSync((ParmBlkPtr)&pb);
    memcpy(out, pb.csParam, olen);
    return err;
}

static void kill_io(short ref)
{
    ParamBlockRec pb;
    memset(&pb, 0, sizeof(pb));
    pb.ioParam.ioRefNum = ref;
    PBKillIOSync(&pb);
}

static short speed_bits(short speed)
{
    switch (speed) {
    case kSpeed38400: return baud38400;
    case kSpeed19200: return baud19200;
    default:          return baud57600;
    }
}

const char *serial_speed_name(short speed)
{
    switch (speed) {
    case kSpeed38400: return "38,400";
    case kSpeed19200: return "19,200";
    default:          return "57,600";
    }
}

Boolean serial_is_open(void)
{
    return gIn != 0;
}

OSErr serial_open(short port, short speed)
{
    OSErr err;
    short config;
    struct { Ptr buf; short len; } setbuf;
    SerShk shk;
    ConstStr255Param outName = port == kPortPrinter ? "\p.BOut" : "\p.AOut";
    ConstStr255Param inName = port == kPortPrinter ? "\p.BIn" : "\p.AIn";

    if (gIn)
        return noErr;
    if (!gBuf) {
        gBuf = NewPtr(INPUT_BUF_SIZE);
        if (!gBuf)
            return memFullErr;
    }
    err = OpenDriver(outName, &gOut);
    if (err)
        return err;
    err = OpenDriver(inName, &gIn);
    if (err) {
        CloseDriver(gOut);
        gOut = 0;
        gIn = 0;
        return err;
    }

    config = speed_bits(speed) + data8 + noParity + stop10;
    control(gOut, csSerReset, &config, sizeof(config));
    control(gIn, csSerReset, &config, sizeof(config));

    /* No XON/XOFF, no CTS: flow control is the bridge's W / !ACK window. */
    memset(&shk, 0, sizeof(shk));
    control(gOut, csSerHShake, &shk, sizeof(shk));
    control(gIn, csSerHShake, &shk, sizeof(shk));

    setbuf.buf = gBuf;
    setbuf.len = (short)(INPUT_BUF_SIZE - 1);  /* the size is a signed short */
    control(gIn, csSerSetBuf, &setbuf, sizeof(setbuf));

    serial_errors();    /* clear anything left from before */
    return noErr;
}

void serial_close(void)
{
    struct { Ptr buf; short len; } setbuf;
    if (!gIn)
        return;
    /* Restore the driver's own buffer before ours can go away. */
    setbuf.buf = NULL;
    setbuf.len = 0;
    control(gIn, csSerSetBuf, &setbuf, sizeof(setbuf));
    kill_io(gIn);
    kill_io(gOut);
    CloseDriver(gIn);
    CloseDriver(gOut);
    gIn = gOut = 0;
}

long serial_read(char *buf, long max)
{
    long waiting = 0;
    ParamBlockRec pb;
    if (!gIn)
        return 0;
    if (status(gIn, csSerGetBuf, &waiting, sizeof(waiting)) != noErr || waiting <= 0)
        return 0;
    if (waiting > max)
        waiting = max;
    memset(&pb, 0, sizeof(pb));
    pb.ioParam.ioRefNum = gIn;
    pb.ioParam.ioBuffer = buf;
    pb.ioParam.ioReqCount = waiting;
    pb.ioParam.ioPosMode = fsAtMark;
    if (PBReadSync(&pb) != noErr)
        return 0;
    return pb.ioParam.ioActCount;
}

OSErr serial_write(const char *data, long len)
{
    ParamBlockRec pb;
    if (!gOut)
        return notOpenErr;
    if (len <= 0)
        return noErr;
    memset(&pb, 0, sizeof(pb));
    pb.ioParam.ioRefNum = gOut;
    pb.ioParam.ioBuffer = (Ptr)data;
    pb.ioParam.ioReqCount = len;
    pb.ioParam.ioPosMode = fsAtMark;
    return PBWriteSync(&pb);
}

short serial_errors(void)
{
    SerStaRec st;
    if (!gIn)
        return 0;
    memset(&st, 0, sizeof(st));
    if (status(gIn, csSerStatus, &st, sizeof(st)) != noErr)
        return 0;
    return st.cumErrs & kSerialLossBits;
}
