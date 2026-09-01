#ifndef COMMON_H
#define COMMON_H

#include "stdint.h"
#include "stdbool.h"

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

/**
 * @brief Latest sensor readings shared between sensor_control and other
 *        modules (e.g. screen_control). Filled by sensor_control's task.
 */
typedef struct {
    float temperature;      /**< °C  from AHT20. */
    float humidity;         /**< %RH from AHT20. */
    float pressure;         /**< hPa from BMP280. */
    int16_t pm1_0_atm;      /**< µg/m³ from PMS7003. */
    int16_t pm2_5_atm;      /**< µg/m³ from PMS7003. */
    int16_t pm10_atm;       /**< µg/m³ from PMS7003. */
    uint16_t co2;           /**< ppm from SCD40. */
    bool env_valid;         /**< true if AHT20+BMP280 read OK. */
    bool pms_valid;         /**< true if PMS7003 read OK. */
    bool scd40_valid;       /**< true if SCD40 read OK. */
} sensor_data_t;

#endif /* COMMON_H */