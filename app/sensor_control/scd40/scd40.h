#ifndef SCD40_H
#define SCD40_H

#include "debug.h"
#include "commons.h"
#include "driver/i2c_master.h"

/******************************* DEFINITIONS *******************************/

/******************************* DATA TYPES *******************************/
typedef struct {
    uint16_t co2;
    uint16_t temperature;
    uint16_t humidity;
} scd40_data_t;

typedef struct {
    i2c_device_config_t scd40_cfg;
    i2c_master_dev_handle_t scd40_device;
} scd40_config_t;

/******************************* FUNCTIONS PROTOTYPE *******************************/
APP_RESULT scd40_init(i2c_master_bus_handle_t bus_handle, scd40_config_t *cfg);
APP_RESULT scd40_start_periodic_measurement(scd40_config_t *cfg);
APP_RESULT scd40_stop_periodic_measurement(scd40_config_t *cfg);
APP_RESULT scd40_read_measurement(scd40_config_t *cfg, scd40_data_t *data);

/******************************* VARIABLES *******************************/

/******************************* FUNCTIONS IMPLEMENTATION *******************************/
#endif /* SCD40_H */