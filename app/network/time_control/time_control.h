#ifndef TIME_CONTROL_H
#define TIME_CONTROL_H

#include "debug.h"
#include "stdbool.h"
#include <time.h>
#include <sys/time.h>
#include "esp_sntp.h"
/******************************* DEFINITIONS *******************************/

/******************************* FUNCTIONS PROTOTYPE *******************************/
APP_RESULT time_control_init();
bool time_control_is_synced();
void time_control_print_current_time();
APP_RESULT time_control_get_time_str(char *buf, size_t len, const char *fmt);
/******************************* DATA TYPES *******************************/
typedef struct {
    bool is_synced;
    struct tm current_time;
} time_control_t;
/******************************* VARIABLES *******************************/

#endif /* TIME_CONTROL_H */
