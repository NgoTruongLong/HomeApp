/**
 * @file env_sensor.c
 * @brief Implementation of the self-contained AHT20 + BMP280 I2C driver.
 *
 * AHT20 protocol: 7-bit address 0x38; command + status/data registers.
 * BMP280 protocol: 7-bit address 0x77 (SDO=GND); register map with
 * calibration data at 0x88..0x9F and raw data at 0xF7..0xFC.
 */

#include "env_sensor.h"

#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"
static const char *TAG = "env_sensor";

/* Device I2C clock — must match the shared I2C bus speed (SENSOR_I2C_BUS_SPEED_HZ). */

/* ------------------------------ AHT20 ------------------------------ */
#define AHT20_ADDR           0x38
#define AHT20_CMD_SOFTRESET  0xBA
#define AHT20_CMD_INIT       0xBE
#define AHT20_CMD_TRIGGER    0xAC
#define AHT20_STATUS_BUSY    (1 << 7)
#define AHT20_STATUS_CALIB   (1 << 3)

/* ------------------------------ BMP280 ----------------------------- */
#define BMP280_ADDR           0x77
#define BMP280_REG_CHIP_ID    0xD0
#define BMP280_REG_RESET      0xE0
#define BMP280_REG_CTRL_MEAS  0xF4
#define BMP280_REG_CONFIG     0xF5
#define BMP280_REG_PRESS_MSB  0xF7
#define BMP280_REG_CALIB      0x88

/* ------------------------------ AHT20 ------------------------------ */

/**
 * @brief Bring the AHT20 out of its power-on state and ensure the
 *        calibration (OTP) coefficient bit is set.
 */
static esp_err_t aht20_init(i2c_master_dev_handle_t dev)
{
    vTaskDelay(pdMS_TO_TICKS(40));                      /* power-on settle */

    const uint8_t reset = AHT20_CMD_SOFTRESET;
    ESP_RETURN_ON_ERROR(i2c_master_transmit(dev, &reset, 1, 100), TAG, "softreset failed");
    vTaskDelay(pdMS_TO_TICKS(40));                      /* reset pulse (min 20 ms) */

    uint8_t status = 0;
    ESP_RETURN_ON_ERROR(i2c_master_receive(dev, &status, 1, 100), TAG, "read status failed");

    if (!(status & AHT20_STATUS_CALIB)) {
        const uint8_t init[] = {AHT20_CMD_INIT, 0x08, 0x00};
        ESP_RETURN_ON_ERROR(i2c_master_transmit(dev, init, sizeof(init), 100), TAG, "init cmd failed");
        vTaskDelay(pdMS_TO_TICKS(10));

        ESP_RETURN_ON_ERROR(i2c_master_receive(dev, &status, 1, 100), TAG, "read status failed");
        ESP_RETURN_ON_FALSE(status & AHT20_STATUS_CALIB, ESP_ERR_INVALID_STATE, TAG,
                            "calibration bit not set (status=0x%02X)", status);
    }
    return ESP_OK;
}

/**
 * @brief Trigger a measurement and decode temperature + humidity.
 *
 * Humidity: 20-bit value, RH% = raw / 2^20 * 100.
 * Temperature: 20-bit value, T°C = raw / 2^20 * 200 - 50.
 */
static esp_err_t aht20_read(i2c_master_dev_handle_t dev, float *temp, float *hum)
{
    const uint8_t trigger[] = {AHT20_CMD_TRIGGER, 0x33, 0x00};
    ESP_RETURN_ON_ERROR(i2c_master_transmit(dev, trigger, sizeof(trigger), 100), TAG, "trigger failed");

    uint8_t d[6];
    for (int attempt = 0; attempt < 5; attempt++) {
        vTaskDelay(pdMS_TO_TICKS(attempt == 0 ? 80 : 10));   /* typically ready in ~80 ms */
        ESP_RETURN_ON_ERROR(i2c_master_receive(dev, d, sizeof(d), 100), TAG, "read data failed");
        if (!(d[0] & AHT20_STATUS_BUSY)) {
            break;
        }
        if (attempt == 4) {
            ESP_LOGE(TAG, "AHT20 still busy after 5 attempts");
            return ESP_ERR_TIMEOUT;
        }
    }

    const uint32_t raw_hum  = ((uint32_t)d[1] << 12) | ((uint32_t)d[2] << 4) | (d[3] >> 4);
    const uint32_t raw_temp = ((uint32_t)(d[3] & 0x0F) << 16) | ((uint32_t)d[4] << 8) | d[5];

    *hum  = (float)raw_hum  / (float)(1 << 20) * 100.0f;
    *temp = (float)raw_temp / (float)(1 << 20) * 200.0f - 50.0f;
    return ESP_OK;
}

/* ------------------------------ BMP280 ----------------------------- */

static esp_err_t bmp280_read_regs(i2c_master_dev_handle_t dev, uint8_t reg,
                                  uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, buf, len, 100);
}

static esp_err_t bmp280_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    const uint8_t buf[] = {reg, val};
    return i2c_master_transmit(dev, buf, sizeof(buf), 100);
}

/** Read the 24-byte calibration block (registers 0x88..0x9F), little-endian. */
static esp_err_t bmp280_read_trim(i2c_master_dev_handle_t dev, env_sensor_t *h)
{
    uint8_t cal[24];
    ESP_RETURN_ON_ERROR(bmp280_read_regs(dev, BMP280_REG_CALIB, cal, sizeof(cal)), TAG, "read calib failed");

    h->dig_T1 = (uint16_t)(cal[0]  | (cal[1]  << 8));
    h->dig_T2 = (int16_t) (cal[2]  | (cal[3]  << 8));
    h->dig_T3 = (int16_t) (cal[4]  | (cal[5]  << 8));
    h->dig_P1 = (uint16_t)(cal[6]  | (cal[7]  << 8));
    h->dig_P2 = (int16_t) (cal[8]  | (cal[9]  << 8));
    h->dig_P3 = (int16_t) (cal[10] | (cal[11] << 8));
    h->dig_P4 = (int16_t) (cal[12] | (cal[13] << 8));
    h->dig_P5 = (int16_t) (cal[14] | (cal[15] << 8));
    h->dig_P6 = (int16_t) (cal[16] | (cal[17] << 8));
    h->dig_P7 = (int16_t) (cal[18] | (cal[19] << 8));
    h->dig_P8 = (int16_t) (cal[20] | (cal[21] << 8));
    h->dig_P9 = (int16_t) (cal[22] | (cal[23] << 8));
    return ESP_OK;
}

/**
 * @brief Verify the chip, reset it, load calibration, and enable measurements.
 */
static esp_err_t bmp280_init(i2c_master_dev_handle_t dev, env_sensor_t *h)
{
    uint8_t id = 0;
    ESP_RETURN_ON_ERROR(bmp280_read_regs(dev, BMP280_REG_CHIP_ID, &id, 1), TAG, "read chip id failed");
    /* 0x58 = BMP280, 0x60 = BME280 (pressure-compatible). */
    ESP_RETURN_ON_FALSE(id == 0x58 || id == 0x60, ESP_ERR_INVALID_RESPONSE, TAG,
                        "unexpected chip ID 0x%02X", id);

    ESP_RETURN_ON_ERROR(bmp280_write_reg(dev, BMP280_REG_RESET, 0xB6), TAG, "softreset failed");
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_RETURN_ON_ERROR(bmp280_read_trim(dev, h), TAG, "read trimming failed");

    /* ctrl_meas: osrs_t = x2, osrs_p = x16, normal mode (0x57). */
    ESP_RETURN_ON_ERROR(bmp280_write_reg(dev, BMP280_REG_CTRL_MEAS, 0x57), TAG, "ctrl_meas write failed");
    /* config: t_sb = 0.5 ms, IIR filter coefficient = 16 (0x10). */
    ESP_RETURN_ON_ERROR(bmp280_write_reg(dev, BMP280_REG_CONFIG, 0x10), TAG, "config write failed");

    vTaskDelay(pdMS_TO_TICKS(50));                      /* first measurement ready */
    return ESP_OK;
}

/** Temperature compensation (Bosch BMP280 datasheet §4.2.3). */
static int32_t bmp280_compensate_t(const env_sensor_t *h, int32_t adc_T)
{
    const int32_t v1 = ((((adc_T >> 3) - ((int32_t)h->dig_T1 << 1))) * (int32_t)h->dig_T2) >> 11;
    const int32_t v2 = (((((adc_T >> 4) - (int32_t)h->dig_T1) *
                          ((adc_T >> 4) - (int32_t)h->dig_T1)) >> 12) * (int32_t)h->dig_T3) >> 14;
    return v1 + v2;                                     /* t_fine */
}

/**
 * @brief Read raw data and compute the compensated pressure (hPa).
 */
static esp_err_t bmp280_read(i2c_master_dev_handle_t dev, const env_sensor_t *h, float *pressure)
{
    uint8_t raw[6];
    ESP_RETURN_ON_ERROR(bmp280_read_regs(dev, BMP280_REG_PRESS_MSB, raw, sizeof(raw)), TAG, "read raw failed");

    const int32_t adc_P = ((int32_t)raw[0] << 12) | ((int32_t)raw[1] << 4) | (raw[2] >> 4);
    const int32_t adc_T = ((int32_t)raw[3] << 12) | ((int32_t)raw[4] << 4) | (raw[5] >> 4);

    const int32_t t_fine = bmp280_compensate_t(h, adc_T);

    /* Pressure compensation — 64-bit integer path (datasheet §4.2.3). */
    int64_t var1 = (int64_t)t_fine - 128000;
    int64_t var2 = var1 * var1 * (int64_t)h->dig_P6;
    var2 = var2 + ((var1 * (int64_t)h->dig_P5) << 17);
    var2 = var2 + (((int64_t)h->dig_P4) << 35);
    var1 = ((var1 * var1 * (int64_t)h->dig_P3) >> 8) + ((var1 * (int64_t)h->dig_P2) << 12);
    var1 = ((((int64_t)1) << 47) + var1) * ((int64_t)h->dig_P1) >> 33;
    if (var1 == 0) {
        ESP_LOGE(TAG, "pressure compensation divide-by-zero");
        return ESP_ERR_INVALID_STATE;
    }
    int64_t p = 1048576 - adc_P;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = (((int64_t)h->dig_P9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (((int64_t)h->dig_P8) * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (((int64_t)h->dig_P7) << 4);

    /* p is in Pa*256 (Q24.8); convert to hPa. */
    *pressure = (float)(uint32_t)p / 256.0f / 100.0f;
    return ESP_OK;
}

/* ---------------------------- Public API --------------------------- */

esp_err_t env_sensor_init(i2c_master_bus_handle_t bus, env_sensor_t *handle)
{
    const i2c_device_config_t aht_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = AHT20_ADDR,
        .scl_speed_hz    = SENSOR_I2C_BUS_SPEED_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &aht_cfg, &handle->aht20), TAG, "add AHT20 failed");

    const i2c_device_config_t bmp_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BMP280_ADDR,
        .scl_speed_hz    = SENSOR_I2C_BUS_SPEED_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &bmp_cfg, &handle->bmp280);
    if (err != ESP_OK) {
        i2c_master_bus_rm_device(handle->aht20);
        return err;
    }

    err = aht20_init(handle->aht20);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AHT20 init failed: %s", esp_err_to_name(err));
        goto cleanup;
    }
    err = bmp280_init(handle->bmp280, handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BMP280 init failed: %s", esp_err_to_name(err));
        goto cleanup;
    }
    return ESP_OK;

cleanup:
    i2c_master_bus_rm_device(handle->aht20);
    i2c_master_bus_rm_device(handle->bmp280);
    return err;
}

esp_err_t env_sensor_read(env_sensor_t *handle, env_sensor_reading_t *out)
{
    ESP_RETURN_ON_FALSE(handle && out, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    /* Temperature + humidity from AHT20, then pressure from BMP280. */
    ESP_RETURN_ON_ERROR(aht20_read(handle->aht20, &out->temperature, &out->humidity),
                        TAG, "AHT20 read failed");
    return bmp280_read(handle->bmp280, handle, &out->pressure);
}
