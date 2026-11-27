#include "speedroute.h"

SpeedRoute speed_route(int fast, long driver_err, long machine)
{
    if (!fast)
        return SPEED_ROUTE_NONE;
    if (driver_err == 0)
        return SPEED_ROUTE_DRIVER;
    /* Only "the driver doesn't know this code", on the one Mac whose chip
       setup is known, may lead to writing chip registers. Any other error
       may mean a broken port. */
    if (driver_err == SPEEDROUTE_CONTROL_ERR && machine == SPEEDROUTE_MACHINE_SE30)
        return SPEED_ROUTE_DIRECT;
    return SPEED_ROUTE_REFUSE;
}
