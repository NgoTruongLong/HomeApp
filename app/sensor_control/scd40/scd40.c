#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include "scd40.h"
#include "debug.h"
#include "i2c_bus.h"
/******************************* DEFINITIONS *******************************/
#define THIS_MODULE_NAME                        "scd40"
#define SCD40_I2C_ADDRESS                       (0x62)
/* SCD40 commands are 16-bit words sent MSB first (big-endian). */
static const uint8_t SCD40_CMD_START_PERIODIC_MEASUREMENT[] = { 0x21, 0xB1 };
static const uint8_t SCD40_CMD_READ_MEASUREMENT[]           = { 0xEC, 0x05 };
static const uint8_t SCD40_CMD_STOP_PERIODIC_MEASUREMENT[]  = { 0x3F, 0x86 };
// static const uint8_t SCD40_CMD_SOFT_RESET[]                 = { 0x36, 0x46 };

/* The SCD40 needs ~1 s after power-up before it ACKs I2C commands. */
#define SCD40_POWER_UP_DELAY_MS                 (1000)
/* Retry starting periodic measurement — the sensor may NACK while not ready. */
#define SCD40_START_RETRY_COUNT                 (10)
#define SCD40_START_RETRY_DELAY_MS              (200)

/* Run the SCD40 at 100 kHz: the I2C probe succeeded at 100 kHz but the 400 kHz
 * command transfer failed, pointing to marginal bus timing / weak pull-ups on
 * this module. AHT20/BMP280 stay at 400 kHz on the shared bus. */
#define SCD40_SCL_SPEED_HZ                      (100000)
/* Tolerate the SCD40 clock-stretching SCL while processing commands. */
#define SCD40_SCL_WAIT_US                       (1500000)
/* SCD40 self-test / fault code: 0x0000 = OK, 0x8006 = malfunction detected. */
#define SCD40_FAULT_CODE                        (0x8006)

/******************************* FUNCTIONS PROTOTYPE *******************************/

/******************************* DATA TYPES *******************************/

/******************************* VARIABLES *******************************/

/******************************* FUNCTIONS IMPLEMENTATION *******************************/

/**
 * @brief DEBUG — identify what device is really answering at 0x62.
 * Tries several SCD40 commands + a raw read and dumps the raw bytes so we can
 * tell whether it is a genuine/stuck SCD40 or a different chip.
 */
static void scd40_diag(scd40_config_t *cfg)
{
    uint8_t buf[16];

    /* 1) Raw read (no write) — some devices answer with status/data. */
    static const uint8_t CMD_RAW_READ[] = { 0x00 };
    memset(buf, 0, sizeof(buf));
    esp_err_t r = i2c_master_transmit_receive(cfg->scd40_device, CMD_RAW_READ, sizeof(CMD_RAW_READ), buf, 9, 200);
    ESP_LOGW(THIS_MODULE_NAME, "DIAG raw-read: %s -> %02x %02x %02x %02x %02x %02x %02x %02x %02x",
             esp_err_to_name(r), buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7], buf[8]);

    /* 2) SCD40 "get data ready status" 0xE4B8 -> 3 bytes. */
    static const uint8_t CMD_DATA_READY[] = { 0xE4, 0xB8 };
    memset(buf, 0, sizeof(buf));
    r = i2c_master_transmit_receive(cfg->scd40_device, CMD_DATA_READY, sizeof(CMD_DATA_READY), buf, 3, 200);
    ESP_LOGW(THIS_MODULE_NAME, "DIAG data-ready(0xE4B8): %s -> %02x %02x %02x",
             esp_err_to_name(r), buf[0], buf[1], buf[2]);

    /* The responses carry a valid Sensirion CRC and read 0x8006 = fault code
     * (documented in the SCD40 datasheet). A sensor in this state rejects new
     * write commands (NACK) and only reports its fault — hardware issue. */
    if (r == ESP_OK && ((buf[0] << 8) | buf[1]) == SCD40_FAULT_CODE) {
        ESP_LOGE(THIS_MODULE_NAME,
                 ">>> SCD40 reports malfunction code 0x8006 (hardware fault). "
                 "Power-cycle the module (unplug VCC); if it persists, replace it.");
    }

    /* 3) SCD40 "get serial number" 0x3682 -> 9 bytes. */
    static const uint8_t CMD_SERIAL[] = { 0x36, 0x82 };
    memset(buf, 0, sizeof(buf));
    r = i2c_master_transmit_receive(cfg->scd40_device, CMD_SERIAL, sizeof(CMD_SERIAL), buf, 9, 200);
    ESP_LOGW(THIS_MODULE_NAME, "DIAG serial(0x3682): %s -> %02x %02x %02x %02x %02x %02x %02x %02x %02x",
             esp_err_to_name(r), buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7], buf[8]);
}

APP_RESULT scd40_init(i2c_master_bus_handle_t bus_handle, scd40_config_t *cfg) {
    APP_RESULT ret = APP_OK;
    cfg->scd40_cfg = (i2c_device_config_t){
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = SCD40_I2C_ADDRESS,
        .scl_speed_hz    = SENSOR_I2C_BUS_SPEED_HZ,
        .scl_wait_us     = SCD40_SCL_WAIT_US,   // tolerate SCD40 clock stretching
    };

    ret = i2c_master_bus_add_device(bus_handle, &cfg->scd40_cfg, &cfg->scd40_device);
    ASSERT_CRITICAL(ret == APP_OK, ret, ret);
    return ret;
}

APP_RESULT scd40_start_periodic_measurement(scd40_config_t *cfg) {
    APP_RESULT ret = APP_OK;
    ret = i2c_master_transmit(cfg->scd40_device, SCD40_CMD_START_PERIODIC_MEASUREMENT, sizeof(SCD40_CMD_START_PERIODIC_MEASUREMENT), 2000);
    ASSERT(ret == APP_OK, ret);
    return ret;
}

APP_RESULT scd40_stop_periodic_measurement(scd40_config_t *cfg) {
    APP_RESULT ret = APP_OK;
    ret = i2c_master_transmit(cfg->scd40_device, SCD40_CMD_STOP_PERIODIC_MEASUREMENT, sizeof(SCD40_CMD_STOP_PERIODIC_MEASUREMENT), 2000);
    ASSERT(ret == APP_OK, ret);
    return ret;
}

/* Sensirion CRC-8: polynomial 0x31, init 0xFF (one per 2-byte value). */
static uint8_t scd40_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

APP_RESULT scd40_read_measurement(scd40_config_t *cfg, scd40_data_t *data) {
    APP_RESULT ret = APP_OK;
    uint8_t read_buffer[9] = {0};
    ret = i2c_master_transmit_receive(cfg->scd40_device, SCD40_CMD_READ_MEASUREMENT, sizeof(SCD40_CMD_READ_MEASUREMENT), read_buffer, sizeof(read_buffer), 2000);
    ASSERT(ret == APP_OK, ret);

    // Each 2-byte value is followed by a CRC-8 byte — reject corrupted frames.
    if (scd40_crc8(&read_buffer[0], 2) != read_buffer[2] ||
        scd40_crc8(&read_buffer[3], 2) != read_buffer[5] ||
        scd40_crc8(&read_buffer[6], 2) != read_buffer[8]) {
        ESP_LOGW(THIS_MODULE_NAME, "SCD40 data CRC mismatch");
        return APP_ERROR;
    }

    data->co2 = (read_buffer[0] << 8) | read_buffer[1];
    data->temperature = (read_buffer[3] << 8) | read_buffer[4];   /* °C × 100 */
    data->humidity = (read_buffer[6] << 8) | read_buffer[7];      /* %RH × 100 */

    printf("SCD40 Read Measurement: CO2=%d ppm, Temperature=%.2f°C, Humidity=%.2f%%\n",
           data->co2, data->temperature / 100.0f, data->humidity / 100.0f);

    return ret;
}

