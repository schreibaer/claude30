/* Which way the serial port reaches 115,200 baud. Pure logic, so the host
   tests can cover it; the Toolbox and chip writes live in src/mac/serial.c. */
#ifndef SPEEDROUTE_H
#define SPEEDROUTE_H

/* Device Manager error for a control call the driver doesn't know. */
#define SPEEDROUTE_CONTROL_ERR (-17)
/* Gestalt 'mach' value of a Macintosh SE/30. */
#define SPEEDROUTE_MACHINE_SE30 9

typedef enum {
    SPEED_ROUTE_NONE,     /* not 115,200: nothing extra to do */
    SPEED_ROUTE_DRIVER,   /* the driver accepted the 115K call */
    SPEED_ROUTE_DIRECT,   /* an SE/30 whose driver doesn't know it: set the chip's clock */
    SPEED_ROUTE_REFUSE    /* can't be done safely here: stop and tell the person */
} SpeedRoute;

/* fast: the person chose 115,200. driver_err: the result of the driver's
   115K control call (0 = accepted). machine: Gestalt 'mach', 0 if unknown. */
SpeedRoute speed_route(int fast, long driver_err, long machine);

#endif
