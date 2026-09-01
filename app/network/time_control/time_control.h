#ifndef TIME_CONTROL_H
#define TIME_CONTROL_H

#include "debug.h"
#include "stdbool.h"
/******************************* DEFINITIONS *******************************/

/******************************* FUNCTIONS PROTOTYPE *******************************/
APP_RESULT time_control_init();
bool time_control_is_synced();
void time_control_print_current_time();
APP_RESULT time_control_get_time_str(char *buf, size_t len, const char *fmt);
/******************************* DATA TYPES *******************************/

/******************************* VARIABLES *******************************/

#endif /* TIME_CONTROL_H */
