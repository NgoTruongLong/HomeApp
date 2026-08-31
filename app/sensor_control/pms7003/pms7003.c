/**
 * @file pms7003.c
 * @brief Implementation of the self-contained PMS7003 UART driver.
 */

#include "pms7003.h"

#include <string.h>

#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pms7003";

#define PMS7003_FRAME_LEN       32
#define PMS7003_HEADER_1        0x42
#define PMS7003_HEADER_2        0x4D
#define PMS7003_CMD_LEN         7
#define PMS7003_ACK_LEN         8
#define PMS7003_BAUDRATE        9600

/* Passive-read command and mode-switch commands (header + command + data + checksum). */
static const uint8_t CMD_PASSIVE_READ[] = { 0x42, 0x4D, 0xE2, 0x00, 0x00, 0x01, 0x71 };
static const uint8_t CMD_SET_PASSIVE[]  = { 0x42, 0x4D, 0xE1, 0x00, 0x00, 0x01, 0x70 };
static const uint8_t CMD_SET_ACTIVE[]   = { 0x42, 0x4D, 0xE1, 0x00, 0x01, 0x01, 0x71 };

static pms7003_config_t s_config;

/**
 * @brief Write a full command frame to the sensor and wait for TX to finish.
 */
static esp_err_t pms7003_send_command(const uint8_t *cmd, size_t len)
{
    const int written = uart_write_bytes(s_config.uart_port, cmd, len);
    ESP_RETURN_ON_FALSE(written == (int)len, ESP_FAIL, TAG,
                        "uart write failed (%d/%d bytes)", written, (int)len);
    ESP_RETURN_ON_ERROR(uart_wait_tx_done(s_config.uart_port, pdMS_TO_TICKS(100)),
                        TAG, "uart TX timeout");
    return ESP_OK;
}

esp_err_t pms7003_init(const pms7003_config_t *config)
{
    esp_err_t err;

    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_FALSE(config->rx_buffer_size > 0, ESP_ERR_INVALID_ARG, TAG,
                        "rx_buffer_size must be > 0");

    memcpy(&s_config, config, sizeof(s_config));

    const uart_config_t uart_config = {
        .baud_rate  = PMS7003_BAUDRATE,
        .data_bits  = UART_DATA_8_BITS,
        .stop_bits  = UART_STOP_BITS_1,
        .parity     = UART_PARITY_DISABLE,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };

    err = uart_driver_install(config->uart_port, config->rx_buffer_size, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: 0x%x", err);
        return err;
    }

    err = uart_param_config(config->uart_port, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: 0x%x", err);
        goto fail;
    }

    err = uart_set_pin(config->uart_port, config->tx_pin, config->rx_pin,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: 0x%x", err);
        goto fail;
    }

    return ESP_OK;

fail:
    uart_driver_delete(config->uart_port);
    return err;
}

esp_err_t pms7003_deinit(void)
{
    return uart_driver_delete(s_config.uart_port);
}

void pms7003_flush(void)
{
    uart_flush_input(s_config.uart_port);
}

esp_err_t pms7003_set_mode(pms7003_mode_t mode)
{
    const uint8_t *cmd = (mode == PMS7003_MODE_PASSIVE) ? CMD_SET_PASSIVE : CMD_SET_ACTIVE;

    /* Drop any data frames the sensor may still be pushing from the previous mode. */
    pms7003_flush();

    ESP_RETURN_ON_ERROR(pms7003_send_command(cmd, PMS7003_CMD_LEN), TAG,
                        "send mode command failed");

    /* The sensor answers a mode command with an 8-byte acknowledgement. Drain it so
     * the next frame read is not confused by it. Missing ack is not fatal. */
    uint8_t ack[PMS7003_ACK_LEN];
    int len = 0;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
    while (len < PMS7003_ACK_LEN && xTaskGetTickCount() < deadline) {
        const int n = uart_read_bytes(s_config.uart_port, &ack[len],
                                      PMS7003_ACK_LEN - len, pdMS_TO_TICKS(100));
        if (n > 0) {
            len += n;
        }
    }
    if (len < PMS7003_ACK_LEN) {
        ESP_LOGW(TAG, "no ack for mode command (%d/%d bytes)", len, PMS7003_ACK_LEN);
    }

    return ESP_OK;
}

/**
 * @brief Wait for a full 32-byte frame, synchronizing on the header and
 *        verifying the checksum. Misaligned/corrupted bytes are skipped.
 */
static esp_err_t pms7003_wait_frame(uint8_t frame[PMS7003_FRAME_LEN], uint32_t timeout_ms)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    uint8_t byte = 0;
    int idx = 0;

    /* Phase 1: synchronize on the 0x42 0x4D header. */
    while (idx < 2) {
        if (uart_read_bytes(s_config.uart_port, &byte, 1, pdMS_TO_TICKS(50)) != 1) {
            if (xTaskGetTickCount() >= deadline) {
                return ESP_ERR_TIMEOUT;
            }
            continue;
        }
        if (idx == 0) {
            if (byte == PMS7003_HEADER_1) {
                frame[idx++] = byte;
            }
        } else { /* idx == 1 */
            if (byte == PMS7003_HEADER_2) {
                frame[idx++] = byte;
            } else if (byte == PMS7003_HEADER_1) {
                frame[0] = byte;    /* possible start of a new frame */
            } else {
                idx = 0;
            }
        }
    }

    /* Phase 2: read the remaining 30 bytes. */
    int need = PMS7003_FRAME_LEN - 2;
    while (need > 0) {
        const int n = uart_read_bytes(s_config.uart_port, &frame[PMS7003_FRAME_LEN - need],
                                      need, pdMS_TO_TICKS(50));
        if (n <= 0) {
            if (xTaskGetTickCount() >= deadline) {
                return ESP_ERR_TIMEOUT;
            }
            continue;
        }
        need -= n;
    }

    /* Phase 3: checksum = sum of bytes [0..29], stored in bytes [30..31]. */
    uint16_t sum = 0;
    for (int i = 0; i < PMS7003_FRAME_LEN - 2; i++) {
        sum += frame[i];
    }
    const uint16_t checksum = ((uint16_t)frame[30] << 8) | frame[31];
    if (sum != checksum) {
        ESP_LOGW(TAG, "checksum mismatch: got 0x%04x, expected 0x%04x", checksum, sum);
        return ESP_ERR_INVALID_CRC;
    }

    return ESP_OK;
}

/**
 * @brief Decode the 32-byte frame into a pms7003_data_t (all fields big-endian).
 */
static void pms7003_parse_frame(const uint8_t frame[PMS7003_FRAME_LEN], pms7003_data_t *data)
{
    data->pm1_0_cf1 = (int16_t)((frame[4]  << 8) | frame[5]);
    data->pm2_5_cf1 = (int16_t)((frame[6]  << 8) | frame[7]);
    data->pm10_cf1  = (int16_t)((frame[8]  << 8) | frame[9]);
    data->pm1_0_atm = (int16_t)((frame[10] << 8) | frame[11]);
    data->pm2_5_atm = (int16_t)((frame[12] << 8) | frame[13]);
    data->pm10_atm  = (int16_t)((frame[14] << 8) | frame[15]);
    data->pc_0_3    = (uint16_t)((frame[16] << 8) | frame[17]);
    data->pc_0_5    = (uint16_t)((frame[18] << 8) | frame[19]);
    data->pc_1_0    = (uint16_t)((frame[20] << 8) | frame[21]);
    data->pc_2_5    = (uint16_t)((frame[22] << 8) | frame[23]);
    data->pc_5_0    = (uint16_t)((frame[24] << 8) | frame[25]);
    data->pc_10     = (uint16_t)((frame[26] << 8) | frame[27]);
}

esp_err_t pms7003_read_active(pms7003_data_t *data, uint32_t timeout_ms)
{
    uint8_t frame[PMS7003_FRAME_LEN];

    ESP_RETURN_ON_FALSE(data, ESP_ERR_INVALID_ARG, TAG, "data is NULL");
    ESP_RETURN_ON_ERROR(pms7003_wait_frame(frame, timeout_ms), TAG,
                        "no valid frame within %lu ms", (unsigned long)timeout_ms);

    pms7003_parse_frame(frame, data);
    return ESP_OK;
}

esp_err_t pms7003_read_passive(pms7003_data_t *data, uint32_t timeout_ms)
{
    uint8_t frame[PMS7003_FRAME_LEN];

    ESP_RETURN_ON_FALSE(data, ESP_ERR_INVALID_ARG, TAG, "data is NULL");
    ESP_RETURN_ON_ERROR(pms7003_send_command(CMD_PASSIVE_READ, PMS7003_CMD_LEN), TAG,
                        "send passive-read command failed");
    ESP_RETURN_ON_ERROR(pms7003_wait_frame(frame, timeout_ms), TAG,
                        "no valid frame within %lu ms", (unsigned long)timeout_ms);

    pms7003_parse_frame(frame, data);
    return ESP_OK;
}
