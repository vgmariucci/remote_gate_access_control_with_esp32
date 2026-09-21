#include <stdlib.h>

#include "unity.h"

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = UNITY_END();

    /* The FreeRTOS linux port keeps the scheduler alive after app_main
     * returns, so the process would hang forever. Exit explicitly, and
     * carry the result in the exit code so CI can actually tell a pass
     * from a failure. */
    exit(failures == 0 ? 0 : 1);
}