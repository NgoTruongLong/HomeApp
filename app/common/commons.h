#ifndef COMMON_H
#define COMMON_H

#include "stdint.h"

#define TASK_MSG_LENGTH_MAX  (128)

typedef struct {
    uint8_t u8type;
    uint8_t u8subtype;
    uint8_t data[TASK_MSG_LENGTH_MAX];
    uint8_t length;
} task_msg_t;


// SENSOR MSG TYPE
#define TASK_MSG_TYPE_SENSOR_READ   (0x01)
#define TASK_MSG_TYPE_PMS_READ      (0x02)
#define TASK_MSG_TYPE_SCD40_READ    (0x03)

#endif /* COMMON_H */