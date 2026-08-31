#ifndef DEBUG_H
#define DEBUG_H

#include "stdio.h"
#include "stdint.h"
#include "esp_log.h"

/******************************* DEFINITIONS *******************************/
#define APP_OK           0
#define APP_ERROR       -1

#define ASSERT_CRITICAL(condition, value, ret)   do {if (!(condition)){printf("ASSERT AT FILE: %s, FUNCTION: %s, LINE: %d, VALUE: %d\r\n", __FILE__, __func__, __LINE__, value);return ret;}} while (0);
#define ASSERT(condition, value)                 do {if (!(condition)){printf("ASSERT AT FILE: %s, FUNCTION: %s, LINE: %d, VALUE: %d\r\n", __FILE__, __func__, __LINE__, value);}} while (0);
#define ASSERT_CRITICAL_VOID(condition, value)   do {if (!(condition)){printf("ASSERT AT FILE: %s, FUNCTION: %s, LINE: %d, VALUE: %d\r\n", __FILE__, __func__, __LINE__, value);return;}} while (0);

/******************************* FUNCTIONS PROTOTYPE *******************************/

/******************************* DATA TYPES *******************************/
typedef int APP_RESULT;
/******************************* VARIABLES *******************************/

/******************************* FUNCTIONS IMPLEMENTATION *******************************/
#endif /* DEBUG_H */