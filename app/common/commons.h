#ifndef COMMON_H
#define COMMON_H

#include "stdint.h"

#define TASK_MSG_LENGTH_MAX  (128)

typedef struct {
    int16_t pm1;
    int16_t pm2_5;
    int16_t pm10;
} pms7003_data_t;

typedef struct {
    uint8_t u8type;
    uint8_t u8subtype;
    uint8_t data[TASK_MSG_LENGTH_MAX];
    uint8_t length;
} task_msg_t;


// SENSOR MSG TYPE
#define TASK_MSG_TYPE_SENSOR_READ   (0x01)
#define TASK_MSG_TYPE_PMS_READ      (0x02)

#endif /* COMMON_H */