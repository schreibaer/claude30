/* Serial port: open in Inside Macintosh order (output driver first), set
   8N1 at the chosen speed with all handshaking off, install a 32 KB input
   buffer, and poll from the event loop. */
#include "serial.h"
#include "speedroute.h"
#include <Devices.h>
#include <Gestalt.h>
#include <Memory.h>
#include <Serial.h>
#include <string.h>

#define INPUT_BUF_SIZE 32768L

static short gIn = 0, gOut = 0;
static Ptr gBuf = NULL;
static SpeedRoute gRoute = SPEED_ROUTE_NONE;
static short gOpenSpeed = kSpeed57600;      /* the speed the open port really uses */
static long gWhyRefused = 0;                /* driver error from the last refused 115,200 open */
static long gWhyMachine = 0;                /* and the Gestalt machine type */
static Boolean gChipSwitched = false;   /* the chip's clock is ours until the next SerReset */

/* Serial driver control and status codes (Inside Macintosh: Devices). */
enum {
    csSerReset = 8,     /* serial configuration word */
    csSerSetBuf = 9,    /* input buffer pointer and size */
    csSerHShake = 14,   /* handshake options, extended form */
    csSer115K = 115,    /* SerialDMA: 115,200 baud from the 3.672 MHz RTxC clock, x32 */
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
    default:          return baud57600;   /* 115,200 starts here, then switches */
    }
}

const char *serial_speed_name(short speed)
{
    switch (speed) {
    case kSpeed38400: return "38,400";
    case kSpeed19200: return "19,200";
    case kSpeed115200: return "115,200";
    default:          return "57,600";
    }
}

short serial_open_speed(short chosen)
{
    return gIn ? gOpenSpeed : chosen;
}

void serial_refusal(long *driver_err, long *machine)
{
    *driver_err = gWhyRefused;
    *machine = gWhyMachine;
}

const char *serial_route_name(void)
{
    switch (gRoute) {
    case SPEED_ROUTE_DRIVER: return " (driver)";
    case SPEED_ROUTE_DIRECT: return " (chip clock)";
    default:                 return "";
    }
}

/* ---- the chip's clock, for 115,200 on an SE/30 ----------------------------

   Apple's SE/30 driver can't name a rate above 57,600, so after it has opened
   the port at 57,600 we take the Z8530's receive and transmit clocks from the
   3.672 MHz RTxC pin in x32 mode (3,672,000 / 32 = 114,750 baud, 0.4% under
   115,200), the switch Apple's SerialDMA driver makes for its 115K call. Only
   three write registers are touched, each with a fixed byte; the registers are
   write-only, so each write sets every bit:
     WR4  = 0x84  x32 clock, one stop bit, no parity
     WR11 = 0x00  receive and transmit clocks from RTxC, TRxC an input, no crystal
     WR14 = 0x00  baud rate generator off, DTR/REQ stays DTR, no echo, no loopback
   WR3, WR5 (DTR and RTS), the interrupt registers and the driver's data
   stay as the driver left them. A restart or the next SerReset clears it.
   Inside Macintosh: Devices (serial driver); Zilog Z8530 SCC manual. */

#define kSCCCtlA 2      /* channel A (modem port) control, from SCCWr */
#define kSCCCtlB 0      /* channel B (printer port) control, from SCCWr */

/* Interrupts off for one two-step register write, so the driver's interrupt
   handler can't use the same control port between the register number and
   the value. Mac OS applications run in supervisor mode, so SR is accessible. */
static unsigned short ints_off(void)
{
    unsigned short sr;
    /* ori raises only the interrupt mask; the other SR bits stay as they were. */
    __asm__ volatile ("move.w %%sr,%0\n\tori.w #0x0700,%%sr" : "=d"(sr) : : "memory", "cc");
    return sr;
}

static void ints_restore(unsigned short sr)
{
    __asm__ volatile ("move.w %0,%%sr" : : "d"(sr) : "memory", "cc");
}

/* The SCC needs time between accesses (6 PCLK, about 1.6 microseconds). */
static void scc_wait(void)
{
    volatile short i;
    for (i = 8; i > 0; i--)
        ;
}

static void scc_write(volatile unsigned char *ctl, unsigned char reg, unsigned char value)
{
    unsigned short sr = ints_off();
    *ctl = reg;
    scc_wait();
    *ctl = value;
    scc_wait();
    ints_restore(sr);
}

/* SCCWr, the low-memory pointer to the chip's write addresses. Read through
   a register so the compiler doesn't treat address 0x1DC as an array bound. */
static volatile unsigned char *scc_base(void)
{
    Ptr *where = (Ptr *)0x01DC;
    __asm__ ("" : "+r"(where));
    return (volatile unsigned char *)*where;
}

/* False, with nothing written, if SCCWr doesn't look like a chip address. */
static Boolean set_chip_clock(short port)
{
    volatile unsigned char *base = scc_base();
    volatile unsigned char *ctl;
    unsigned short sr;
    if (!base || ((unsigned long)base & 1))
        return false;
    ctl = base + (port == kPortPrinter ? kSCCCtlB : kSCCCtlA);
    sr = ints_off();
    *ctl = 0;           /* RR0 selected: the pointer is known */
    scc_wait();
    ints_restore(sr);
    scc_write(ctl, 4, 0x84);
    scc_write(ctl, 11, 0x00);
    scc_write(ctl, 14, 0x00);
    return true;
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
    gRoute = SPEED_ROUTE_NONE;
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

    if (speed == kSpeed115200) {
        long machine = 0;
        OSErr fast = control(gOut, csSer115K, &config, 0);
        if (fast == noErr)
            fast = control(gIn, csSer115K, &config, 0);
        if (Gestalt(gestaltMachineType, &machine) != noErr)
            machine = 0;
        gRoute = speed_route(1, fast, machine);
        if (gRoute == SPEED_ROUTE_DIRECT && !set_chip_clock(port))
            gRoute = SPEED_ROUTE_REFUSE;       /* SCCWr isn't an address we trust */
        else if (gRoute == SPEED_ROUTE_DIRECT)
            gChipSwitched = true;
        if (gRoute == SPEED_ROUTE_REFUSE) {
            gWhyRefused = fast;
            gWhyMachine = machine;
            gRoute = SPEED_ROUTE_NONE;
            setbuf.buf = NULL;
            setbuf.len = 0;
            control(gIn, csSerSetBuf, &setbuf, sizeof(setbuf));
            CloseDriver(gIn);
            CloseDriver(gOut);
            gIn = gOut = 0;
            return kSpeedUnavailableErr;
        }
    }
    gOpenSpeed = speed;

    serial_errors();    /* clear anything left from before */
    return noErr;
}

void serial_close(void)
{
    struct { Ptr buf; short len; } setbuf;
    if (!gIn)
        return;
    if (gChipSwitched) {
        /* Hand the chip back: SerReset makes the driver write its own clock settings. */
        short config = baud57600 + data8 + noParity + stop10;
        control(gOut, csSerReset, &config, sizeof(config));
        control(gIn, csSerReset, &config, sizeof(config));
        gChipSwitched = false;
    }
    gRoute = SPEED_ROUTE_NONE;
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
