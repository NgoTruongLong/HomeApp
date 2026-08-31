#ifndef PUMP_CONTROL
#define PUMP_CONTROL

#include "debug.h"
/******************************* DEFINITIONS *******************************/
typedef enum {
    PUMP_ON,
    PUMP_OFF,
} PUMP_STATE;

#define PUMP_DUTY_MIN   0
#define PUMP_DUTY_MAX   100
/******************************* FUNCTIONS PROTOTYPE *******************************/
APP_RESULT pump_init();
APP_RESULT pump_set_state(PUMP_STATE state);
APP_RESULT pump_set_duty(uint8_t duty_percent);
uint8_t   pump_get_duty();
PUMP_STATE pump_get_state();
/******************************* DATA TYPES *******************************/

/******************************* VARIABLES *******************************/

#endif  /* PUMP_CONTROL */