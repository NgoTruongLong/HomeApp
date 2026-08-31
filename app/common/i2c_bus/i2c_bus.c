/**
 * @file i2c_bus.c
 * @brief Implementation of the shared singleton I2C master bus.
 */

#include "i2c_bus.h"

#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "i2c_bus";

static i2c_master_bus_handle_t s_bus = NULL;

esp_err_t i2c_bus_init(void)
{
    if (s_bus != NULL) {
        return ESP_OK;                  /* already initialized */
    }

    const i2c_master_bus_config_t cfg = {
        .i2c_port            = I2C_BUS_PORT,
        .sda_io_num          = I2C_BUS_SDA_PIN,
        .scl_io_num          = I2C_BUS_SCL_PIN,
        .clk_source          = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt   = 7,
        .flags.enable_internal_pullup = true,
    };

    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &s_bus), TAG, "i2c_new_master_bus failed");
    return ESP_OK;
}

i2c_master_bus_handle_t i2c_bus_get_handle(void)
{
    return s_bus;
}

esp_err_t i2c_bus_create(i2c_master_bus_handle_t *bus_out)
{
    ESP_RETURN_ON_FALSE(bus_out, ESP_ERR_INVALID_ARG, TAG, "bus_out is NULL");
    ESP_RETURN_ON_ERROR(i2c_bus_init(), TAG, "i2c_bus_init failed");
    *bus_out = s_bus;
    return ESP_OK;
}

esp_err_t i2c_bus_scan(void)
{
    ESP_RETURN_ON_FALSE(s_bus != NULL, ESP_ERR_INVALID_STATE, TAG, "bus not initialized");

    ESP_LOGI(TAG, "Scanning I2C bus (0x08-0x77)...");
    for (uint16_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(s_bus, addr, 20) == ESP_OK) {
            ESP_LOGI(TAG, "Device found at 0x%02X", addr);
        }
    }
    ESP_LOGI(TAG, "I2C bus scan finished");
    return ESP_OK;
}
