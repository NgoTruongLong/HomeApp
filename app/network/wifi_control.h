#ifndef WIFI_CONTROL_H
#define WIFI_CONTROL_H

#include "debug.h"
#include "stdbool.h"
/******************************* DEFINITIONS *******************************/

/******************************* FUNCTIONS PROTOTYPE *******************************/
APP_RESULT wifi_control_init();
APP_RESULT wifi_control_wait_connected(uint32_t timeout_ms);
bool wifi_control_is_connected();
/******************************* DATA TYPES *******************************/

/******************************* VARIABLES *******************************/

#endif /* WIFI_CONTROL_H */
