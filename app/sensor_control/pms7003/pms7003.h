/**
 * @file pms7003.h
 * @brief Self-contained driver for the Plantower PMS7003 laser particle sensor.
 *
 * Communication: UART @ 9600 baud, 8 data bits, 1 stop bit, no parity.
 *
 * - Active mode (default): the sensor measures and pushes a 32-byte frame
 *   continuously every ~200-800 ms (fast) / ~2.3 s (stable).
 * - Passive mode: the sensor sends a 32-byte frame only after it receives
 *   the passive-read command.
 *
 * Data frame (32 bytes, big-endian fields):
 * @verbatim
 * +---------+--------------------+----------------------------------------+
 * | Byte(s) | Field              | Description                            |
 * +---------+--------------------+----------------------------------------+
 * | 0       | Start byte 1       | 0x42                                   |
 * | 1       | Start byte 2       | 0x4D                                   |
 * | 2-3     | Frame length       | 0x001C (28)                            |
 * | 4-5     | PM1.0  CF=1        | standard particles, ug/m3              |
 * | 6-7     | PM2.5  CF=1        | standard particles, ug/m3              |
 * | 8-9     | PM10   CF=1        | standard particles, ug/m3              |
 * | 10-11   | PM1.0  ATM         | atmospheric environment, ug/m3         |
 * | 12-13   | PM2.5  ATM         | atmospheric environment, ug/m3         |
 * | 14-15   | PM10   ATM         | atmospheric environment, ug/m3         |
 * | 16-17   | Particles >0.3 um  | count in 0.1 L of air                  |
 * | 18-19   | Particles >0.5 um  | count in 0.1 L of air                  |
 * | 20-21   | Particles >1.0 um  | count in 0.1 L of air                  |
 * | 22-23   | Particles >2.5 um  | count in 0.1 L of air                  |
 * | 24-25   | Particles >5.0 um  | count in 0.1 L of air                  |
 * | 26-27   | Particles >10 um   | count in 0.1 L of air                  |
 * | 28-29   | Reserved           | -                                      |
 * | 30-31   | Checksum           | sum of bytes [0..29]                   |
 * +---------+--------------------+----------------------------------------+
 * @endverbatim
 *
 * @note SET and RESET are optional control pins. If they are not wired, pass
 *       GPIO_NUM_NC for them (they are not used by this driver).
 */

#ifndef PMS7003_H
#define PMS7003_H

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/uart.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Working mode of the sensor. */
typedef enum {
    PMS7003_MODE_PASSIVE = 0,   /**< Reply only when polled. */
    PMS7003_MODE_ACTIVE,        /**< Push data continuously (factory default). */
} pms7003_mode_t;

/** Parsed PMS7003 measurement. */
typedef struct {
    int16_t pm1_0_cf1;  /**< PM1.0  CF=1 (standard particles), ug/m3. */
    int16_t pm2_5_cf1;  /**< PM2.5  CF=1 (standard particles), ug/m3. */
    int16_t pm10_cf1;   /**< PM10   CF=1 (standard particles), ug/m3. */
    int16_t pm1_0_atm;  /**< PM1.0  atmospheric environment, ug/m3. */
    int16_t pm2_5_atm;  /**< PM2.5  atmospheric environment, ug/m3. */
    int16_t pm10_atm;   /**< PM10   atmospheric environment, ug/m3. */
    uint16_t pc_0_3;    /**< Particles >0.3 um in 0.1 L of air. */
    uint16_t pc_0_5;    /**< Particles >0.5 um in 0.1 L of air. */
    uint16_t pc_1_0;    /**< Particles >1.0 um in 0.1 L of air. */
    uint16_t pc_2_5;    /**< Particles >2.5 um in 0.1 L of air. */
    uint16_t pc_5_0;    /**< Particles >5.0 um in 0.1 L of air. */
    uint16_t pc_10;     /**< Particles >10  um in 0.1 L of air. */
} pms7003_data_t;

/** Driver configuration. */
typedef struct {
    uart_port_t uart_port;      /**< UART peripheral to use (e.g. UART_NUM_1). */
    gpio_num_t  tx_pin;         /**< ESP32 TX pin connected to PMS7003 RX. */
    gpio_num_t  rx_pin;         /**< ESP32 RX pin connected to PMS7003 TX. */
    gpio_num_t  set_pin;        /**< Optional SET pin, GPIO_NUM_NC if unused. */
    gpio_num_t  reset_pin;      /**< Optional RESET pin, GPIO_NUM_NC if unused. */
    uint32_t    rx_buffer_size; /**< UART RX ring buffer size in bytes. */
} pms7003_config_t;

/**
 * @brief Install and configure the UART for the PMS7003.
 * @param config Driver configuration (must be valid).
 * @return ESP_OK on success, otherwise an esp_err_t error code.
 */
esp_err_t pms7003_init(const pms7003_config_t *config);

/**
 * @brief Delete the UART driver and release the port.
 * @return ESP_OK on success.
 */
esp_err_t pms7003_deinit(void);

/**
 * @brief Discard all bytes currently buffered in the UART RX ring buffer.
 */
void pms7003_flush(void);

/**
 * @brief Switch the sensor between passive and active mode.
 *
 * Any data frames still coming from the previous mode are flushed first, and
 * the sensor's 8-byte acknowledgement is drained before returning.
 *
 * @param mode PMS7003_MODE_PASSIVE or PMS7003_MODE_ACTIVE.
 * @return ESP_OK on success.
 */
esp_err_t pms7003_set_mode(pms7003_mode_t mode);

/**
 * @brief Wait for the next valid 32-byte frame (active mode).
 *
 * The reader synchronizes on the 0x42 0x4D header and validates the checksum;
 * corrupted or misaligned bytes are skipped automatically.
 *
 * @param data       Output parsed measurement.
 * @param timeout_ms Maximum time to wait for a valid frame.
 * @return ESP_OK on success, ESP_ERR_TIMEOUT / ESP_ERR_INVALID_CRC otherwise.
 */
esp_err_t pms7003_read_active(pms7003_data_t *data, uint32_t timeout_ms);

/**
 * @brief Send the passive-read command and parse the resulting frame.
 *
 * Only valid after the sensor has been switched to passive mode
 * (see pms7003_set_mode).
 *
 * @param data       Output parsed measurement.
 * @param timeout_ms Maximum time to wait for the sensor to measure + reply.
 * @return ESP_OK on success, ESP_ERR_TIMEOUT / ESP_ERR_INVALID_CRC otherwise.
 */
esp_err_t pms7003_read_passive(pms7003_data_t *data, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* PMS7003_H */
