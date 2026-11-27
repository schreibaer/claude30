/* Serial port access through the Serial Driver.
   Toolbox: Device Manager (OpenDriver, CloseDriver, PBControlSync,
   PBStatusSync, PBReadSync, PBWriteSync, KillIO), Memory Manager, Gestalt.
   At 115,200 on an SE/30 it also writes three Z8530 write registers through
   the SCCWr low-memory pointer (see set_chip_clock in serial.c). */
#ifndef SERIAL_H
#define SERIAL_H

#include <MacTypes.h>

enum { kPortModem = 0, kPortPrinter = 1 };
/* Saved in the preferences: append new speeds, never renumber. */
enum { kSpeed57600 = 0, kSpeed38400 = 1, kSpeed19200 = 2, kSpeed115200 = 3 };

/* serial_open result when 115,200 can't be set on this Mac. */
#define kSpeedUnavailableErr (-30000)

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
/* How the open port reached its speed, for the status line: "" for the
   driver's own speeds, " (driver)" or " (chip clock)" at 115,200. */
const char *serial_route_name(void);
/* The speed the open port really uses (the chosen one while it is closed). */
short serial_open_speed(short chosen);
/* After kSpeedUnavailableErr: the driver's error and the Gestalt machine type. */
void serial_refusal(long *driver_err, long *machine);

#endif
