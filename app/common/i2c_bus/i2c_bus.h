/**
 * @file i2c_bus.h
 * @brief Shared singleton I2C master bus for all I2C sensors on the board.
 *
 * All I2C sensors (AHT20, BMP280, and any future ones) share a single I2C
 * bus. This component owns the bus: it is created once and reused.
 *
 * Wiring (I2C bus 0 @ 400 kHz):
 * @verbatim
 * +-------+------+------------------+
 * | Line  | Pin  | Devices          |
 * +-------+------+------------------+
 * | SDA   | GPIO21 | AHT20, BMP280, ... |
 * | SCL   | GPIO20 | AHT20, BMP280, ... |
 * +-------+------+------------------+
 * @endverbatim
 */

#ifndef I2C_BUS_H
#define I2C_BUS_H

#include "esp_err.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Shared I2C bus configuration. */
#define I2C_BUS_PORT        (I2C_NUM_0)
#define I2C_BUS_SDA_PIN     (GPIO_NUM_21)
#define I2C_BUS_SCL_PIN     (GPIO_NUM_20)
#define SENSOR_I2C_BUS_SPEED_HZ    (100000)    /* 400 kHz */

/**
 * @brief Initialize the shared I2C master bus.
 *
 * Idempotent: safe to call from every sensor driver — subsequent calls
 * simply return ESP_OK and keep the existing bus.
 *
 * @return ESP_OK on success.
 */
esp_err_t i2c_bus_init(void);

/**
 * @brief Get the shared I2C bus handle.
 * @return Bus handle, or NULL if i2c_bus_init() has not been called yet.
 */
i2c_master_bus_handle_t i2c_bus_get_handle(void);

/**
 * @brief Create (if needed) the shared bus and return its handle.
 *
 * Convenience wrapper for `i2c_bus_init()` + `i2c_bus_get_handle()`.
 *
 * @param bus_out Output bus handle.
 * @return ESP_OK on success.
 */
esp_err_t i2c_bus_create(i2c_master_bus_handle_t *bus_out);

/**
 * @brief Scan the shared I2C bus and log every responding 7-bit address.
 *
 * Useful for debugging sensor wiring and address configuration.
 *
 * @return ESP_OK on success.
 */
esp_err_t i2c_bus_scan(void);

#ifdef __cplusplus
}
#endif

#endif /* I2C_BUS_H */
