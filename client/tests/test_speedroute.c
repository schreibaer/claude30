/* The 115,200 baud routing decision: Apple's driver call first, the serial
   chip's clock only on an SE/30, otherwise refuse. */
#include "check.h"
#include "speedroute.h"

#define CONTROL_ERR (-17)
#define NOT_OPEN_ERR (-28)
#define MACH_II 6
#define MACH_SE30 9
#define MACH_IICI 11

static void test_slower_speeds_never_route(void)
{
    /* Only 115,200 asks for anything; the others keep today's path. */
    CHECK_EQ_LONG(speed_route(0, 0, MACH_SE30), SPEED_ROUTE_NONE);
    CHECK_EQ_LONG(speed_route(0, CONTROL_ERR, MACH_SE30), SPEED_ROUTE_NONE);
    CHECK_EQ_LONG(speed_route(0, CONTROL_ERR, MACH_IICI), SPEED_ROUTE_NONE);
}

static void test_driver_accepts(void)
{
    CHECK_EQ_LONG(speed_route(1, 0, MACH_SE30), SPEED_ROUTE_DRIVER);
    CHECK_EQ_LONG(speed_route(1, 0, MACH_IICI), SPEED_ROUTE_DRIVER);
    CHECK_EQ_LONG(speed_route(1, 0, 0), SPEED_ROUTE_DRIVER);
}

static void test_se30_unknown_code_uses_the_chip_clock(void)
{
    CHECK_EQ_LONG(speed_route(1, CONTROL_ERR, MACH_SE30), SPEED_ROUTE_DIRECT);
}

static void test_other_macs_refuse(void)
{
    CHECK_EQ_LONG(speed_route(1, CONTROL_ERR, MACH_IICI), SPEED_ROUTE_REFUSE);
    CHECK_EQ_LONG(speed_route(1, CONTROL_ERR, MACH_II), SPEED_ROUTE_REFUSE);
    CHECK_EQ_LONG(speed_route(1, CONTROL_ERR, 0), SPEED_ROUTE_REFUSE);   /* Gestalt failed */
}

static void test_other_errors_refuse_even_on_an_se30(void)
{
    /* Only "this driver doesn't know the code" may lead to register writes;
       any other error may mean a broken port. */
    CHECK_EQ_LONG(speed_route(1, NOT_OPEN_ERR, MACH_SE30), SPEED_ROUTE_REFUSE);
    CHECK_EQ_LONG(speed_route(1, -1, MACH_SE30), SPEED_ROUTE_REFUSE);
    CHECK_EQ_LONG(speed_route(1, 1, MACH_SE30), SPEED_ROUTE_REFUSE);
}

int main(void)
{
    test_slower_speeds_never_route();
    test_driver_accepts();
    test_se30_unknown_code_uses_the_chip_clock();
    test_other_macs_refuse();
    test_other_errors_refuse_even_on_an_se30();
    CHECK_DONE();
}
