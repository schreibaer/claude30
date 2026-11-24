/* Serial port access through the Serial Driver.
   Toolbox: Device Manager (OpenDriver, CloseDriver, PBControlSync,
   PBStatusSync, PBReadSync, PBWriteSync, KillIO), Memory Manager. */
#ifndef SERIAL_H
#define SERIAL_H

#include <MacTypes.h>

enum { kPortModem = 0, kPortPrinter = 1 };
enum { kSpeed57600 = 0, kSpeed38400 = 1, kSpeed19200 = 2 };

/* Serial driver error bits that mean received bytes were lost or damaged. */
enum { kSerialLossBits = 0x01 | 0x10 | 0x20 | 0x40 };

OSErr serial_open(short port, short speed);
void serial_close(void);
Boolean serial_is_open(void);
/* Reads whatever is waiting, up to max bytes, without blocking. */
long serial_read(char *buf, long max);
OSErr serial_write(const char *data, long len);
/* Returns the driver's cumulative error bits since the last call (0 = clean). */
short serial_errors(void);

const char *serial_speed_name(short speed);

#endif
