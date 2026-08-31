#ifndef SENSOR_CONTROL
#define SENSOR_CONTROL

#include "debug.h"
#include "commons.h"
#include "driver/i2c_master.h"
/******************************* DEFINITIONS *******************************/

/******************************* FUNCTIONS PROTOTYPE *******************************/
APP_RESULT sensor_init();
APP_RESULT sensor_fire_event(task_msg_t *msg);
/******************************* DATA TYPES *******************************/

/******************************* VARIABLES *******************************/

#endif /* SENSOR_CONTROL */