/**
 * @file env_sensor.h
 * @brief Self-contained driver for the AHT20 (temperature + humidity) and
 *        BMP280 (pressure + temperature) sensors.
 *
 * The I2C bus itself is owned by the shared i2c_bus component — this driver
 * only registers its devices on the bus handle it is given.
 *
 * Devices (all on the shared bus, SDA=GPIO21, SCL=GPIO20):
 *   - AHT20 : address 0x38 — temperature (°C) + relative humidity (%RH).
 *   - BMP280: address 0x77 (SDO=GND) — pressure (hPa) + temperature.
 *     Chip IDs 0x58 (BMP280) and 0x60 (BME280, pressure-compatible) are
 *     both accepted.
 *
 * Vdd is assumed to be always powered (not switched through a GPIO).
 */

#ifndef ENV_SENSOR_H
#define ENV_SENSOR_H

#include <stdint.h>

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A single combined environmental reading. */
typedef struct {
    float temperature;  /**< °C  — from AHT20. */
    float humidity;     /**< %RH — from AHT20. */
    float pressure;     /**< hPa — from BMP280. */
} env_sensor_reading_t;

/** Driver handle: device handles + BMP280 trimming coefficients. */
typedef struct {
    i2c_master_dev_handle_t aht20;
    i2c_master_dev_handle_t bmp280;
    /* BMP280 trimming coefficients (from register 0x88..0x9F). */
    uint16_t dig_T1;
    int16_t  dig_T2, dig_T3;
    uint16_t dig_P1;
    int16_t  dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;
} env_sensor_t;

/**
 * @brief Initialize the AHT20 and BMP280 devices on an existing I2C bus.
 *
 * Reads the BMP280 calibration data and configures both sensors. The bus
 * itself is created by the shared i2c_bus component (see i2c_bus.h).
 *
 * @param bus    Shared I2C master bus handle (from i2c_bus_init()).
 * @param handle Caller-allocated handle to populate.
 * @return ESP_OK on success.
 */
esp_err_t env_sensor_init(i2c_master_bus_handle_t bus, env_sensor_t *handle);

/**
 * @brief Trigger a measurement and read temperature, humidity, and pressure.
 *
 * @param handle Initialized handle.
 * @param out    Output reading.
 * @return ESP_OK on success.
 */
esp_err_t env_sensor_read(env_sensor_t *handle, env_sensor_reading_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ENV_SENSOR_H */
