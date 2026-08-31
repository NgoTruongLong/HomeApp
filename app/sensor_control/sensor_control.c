#include "debug.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sensor_control.h"
#include "freertos/idf_additions.h"
#include "env_sensor.h"
#include "i2c_bus.h"
#include "soc/gpio_num.h"
#include "task_define.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "pms7003.h"
#include "scd40.h"
#include <stdbool.h>


/******************************* DEFINITIONS *******************************/
#define THIS_MODULE_NAME "sensor_control"
#define PERIODIC_TIMER_INTERVAL_US  (20000000)  /* 20 seconds */

#define SENSOR_PMS7003_SET          (GPIO_NUM_NC)
#define SENSOR_PMS7003_RESET        (GPIO_NUM_NC)
#define SENSOR_PMS7003_UART_PORT    (UART_NUM_1)
#define SENSOR_PMS7003_TX_PIN       (GPIO_NUM_17)   /* ESP32-S3 UART1 TX  -> PMS7003 RX */
#define SENSOR_PMS7003_RX_PIN       (GPIO_NUM_18)   /* ESP32-S3 UART1 RX  <- PMS7003 TX */

/******************************* FUNCTIONS PROTOTYPE *******************************/
static void sensor_timer_callback(void *arg);
/******************************* DATA TYPES *******************************/

typedef struct {
    // aht20 + bmp280
    env_sensor_t sensor;
    env_sensor_reading_t reading;
    // pms7003
    pms7003_data_t pms_data;
    pms7003_config_t pms_config;
    bool pms_passive_mode;
    // scd40
    scd40_config_t scd40_cfg;
    scd40_data_t scd40_data;
    bool scd40_ready;
    // miscellaneous
    i2c_master_bus_handle_t bus;
    QueueHandle_t queue;
    esp_timer_handle_t periodic_timer;
} sensor_t;
/******************************* VARIABLES *******************************/
static sensor_t sensor_control;
void sensor_task(void *arg);
APP_RESULT sensor_aht20_bmp280_init();
APP_RESULT sensor_pms7003_init();
APP_RESULT sensor_scd40_init();
/******************************* FUNCTIONS IMPLEMENTATION *******************************/
APP_RESULT sensor_init() {
    APP_RESULT ret = APP_OK;

    // Create the shared I2C bus once (AHT20, BMP280 and other sensors share it).
    ret = i2c_bus_init();
    ASSERT_CRITICAL(ret == APP_OK, ret, ret);

    sensor_control.bus = i2c_bus_get_handle();

    // DEBUG: list every device on the bus to verify the SCD40 address (0x62).
    i2c_bus_scan();

    ret = sensor_aht20_bmp280_init();
    ASSERT_CRITICAL(ret == APP_OK, ret, ret);

    ret = sensor_pms7003_init();
    ASSERT_CRITICAL(ret == APP_OK, ret, ret);

    ret = sensor_scd40_init();
    ASSERT_CRITICAL(ret == APP_OK, ret, ret);

    // create message queue for sensor data
    sensor_control.queue = xQueueCreate(10, sizeof(task_msg_t));
    ASSERT_CRITICAL(sensor_control.queue, APP_ERROR, APP_ERROR);

    ret = xTaskCreate(sensor_task, "sensor_task", SENSOR_TASK_STACK_SIZE, NULL, SENSOR_TASK_PRIOR, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(THIS_MODULE_NAME, "Failed to create sensor task");
        return APP_ERROR;
    }
    vTaskDelay(pdMS_TO_TICKS(5000));  // let the PMS sensor finish its reset sequence before starting the periodic timer
    // create periodic timer to read sensor data
    const esp_timer_create_args_t timer_args = {
        .callback = &sensor_timer_callback,
        .arg = NULL,
        .name = "read_sensor_timer"
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &sensor_control.periodic_timer));
    esp_timer_start_periodic(sensor_control.periodic_timer, PERIODIC_TIMER_INTERVAL_US);

    return APP_OK;
   
}

APP_RESULT sensor_scd40_init() {
    APP_RESULT ret = scd40_init(sensor_control.bus, &sensor_control.scd40_cfg);
    if (ret != APP_OK) {
        // Non-fatal: a missing/failed CO2 sensor must not block the other sensors.
        sensor_control.scd40_ready = false;
        ESP_LOGW(THIS_MODULE_NAME, "SCD40 init failed (0x%x), CO2 sensor disabled", ret);
        return APP_OK;
    }
    sensor_control.scd40_ready = true;
    return APP_OK;
}

APP_RESULT sensor_aht20_bmp280_init() {
    APP_RESULT ret = APP_OK;
    ret = env_sensor_init(sensor_control.bus, &sensor_control.sensor);
    ASSERT_CRITICAL(ret == APP_OK, ret, ret);
    return ret;
}

APP_RESULT sensor_pms7003_init() {
    sensor_control.pms_config = (pms7003_config_t){
        .uart_port      = SENSOR_PMS7003_UART_PORT,
        .tx_pin         = SENSOR_PMS7003_TX_PIN,
        .rx_pin         = SENSOR_PMS7003_RX_PIN,
        .set_pin        = SENSOR_PMS7003_SET,
        .reset_pin      = SENSOR_PMS7003_RESET,
        .rx_buffer_size = 1024 * 2,
    };

    APP_RESULT ret = pms7003_init(&sensor_control.pms_config);
    ASSERT_CRITICAL(ret == ESP_OK, ret, APP_ERROR);

    return APP_OK;
}

void sensor_task(void *arg) {
    APP_RESULT ret = APP_OK;
    task_msg_t msg;
    static bool scd40_period_trigger = false;
    while (1) {
        ret = xQueueReceive(sensor_control.queue, &msg, portMAX_DELAY);
        switch (msg.u8type) {
            case TASK_MSG_TYPE_SENSOR_READ:
                ESP_LOGI(THIS_MODULE_NAME, "Read sensor data");
                ret = env_sensor_read(&sensor_control.sensor, &sensor_control.reading);
                ASSERT(ret == APP_OK, ret);
                printf("%.2f°C  %.2f%%RH  %.2fhPa\n", sensor_control.reading.temperature, sensor_control.reading.humidity, sensor_control.reading.pressure);
                break;

            case TASK_MSG_TYPE_PMS_READ:
                // First PMS cycle: switch the sensor to passive mode (once).
                // In passive mode the sensor only sends data when polled, so no
                // wake/sleep cycle is needed between reads. (Wake requires ~32s
                // reset+stabilize, which doesn't fit the 20s periodic timer.)
                if (!sensor_control.pms_passive_mode) {
                    ESP_LOGI(THIS_MODULE_NAME, "switching PMS7003 to passive mode...");
                    ret = pms7003_set_mode(PMS7003_MODE_PASSIVE);
                    ASSERT(ret == APP_OK, ret);
                    sensor_control.pms_passive_mode = true;
                    break;  // actual reading starts from the next cycle
                }

                ESP_LOGI(THIS_MODULE_NAME, "Read PMS data");
                // The sensor measures first and only then sends its 32-byte frame,
                // so wait up to 2.5s instead of one short 100ms read.
                ret = pms7003_read_passive(&sensor_control.pms_data, 2500);
                if (ret != ESP_OK) {
                    ESP_LOGW(THIS_MODULE_NAME, "PMS read failed (0x%x)", ret);
                    break;
                }

                printf("PMS7003: PM1.0=%d ug/m3, PM2.5=%d ug/m3, PM10=%d ug/m3\n",
                       sensor_control.pms_data.pm1_0_atm,
                       sensor_control.pms_data.pm2_5_atm,
                       sensor_control.pms_data.pm10_atm);
                break;

            case TASK_MSG_TYPE_SCD40_READ:
                if (!scd40_period_trigger) {
                    ESP_LOGI(THIS_MODULE_NAME, "Trigger SCD40 periodic measurement");
                    ret = scd40_start_periodic_measurement(&sensor_control.scd40_cfg);
                    ASSERT(ret == APP_OK, ret);
                    scd40_period_trigger = true;
                    break;
                } else {
                    ESP_LOGI(THIS_MODULE_NAME, "Read SCD40 data");
                    ret = scd40_read_measurement(&sensor_control.scd40_cfg, &sensor_control.scd40_data);
                    if (ret != APP_OK) {
                        ESP_LOGE(THIS_MODULE_NAME, "Failed to read SCD40 data");
                    }
                }
                break;
            default:
                ESP_LOGW(THIS_MODULE_NAME, "Unknown message type: %d", msg.u8type);
                break;
        }
    }
};

APP_RESULT sensor_fire_event(task_msg_t *msg) {
    APP_RESULT ret = APP_OK;
    ret = xQueueSend(sensor_control.queue, msg, 100);
    if (ret != pdPASS) {
        ESP_LOGE(THIS_MODULE_NAME, "Failed to send message to sensor task");
        return APP_ERROR;
    }
    return APP_OK;
}

static void sensor_timer_callback(void *arg)
{
    // NOTE: this runs in the esp_timer task - it must NEVER block
    // (no vTaskDelay / blocking calls here). The queue is processed in order,
    // and sensor_task already delays internally for sensor stabilization.
    APP_RESULT ret = APP_OK;
    // send message to sensor task to read sensor data
    task_msg_t msg = {
        .u8type = TASK_MSG_TYPE_SENSOR_READ,
        .u8subtype = 0,
        .length = 0
    };
    ret = sensor_fire_event(&msg);
    if (ret != APP_OK) {
        ESP_LOGE(THIS_MODULE_NAME, "Failed to fire sensor event");
    }

    task_msg_t pms_msg = {
        .u8type = TASK_MSG_TYPE_PMS_READ,
        .u8subtype = 0,
        .length = 0
    };
    ret = sensor_fire_event(&pms_msg);
    if (ret != APP_OK) {
        ESP_LOGE(THIS_MODULE_NAME, "Failed to fire sensor event");
    }

    task_msg_t scd40_msg = {
        .u8type = TASK_MSG_TYPE_SCD40_READ,
        .u8subtype = 0,
        .length = 0
    };
    ret = sensor_fire_event(&scd40_msg);
    if (ret != APP_OK) {
        ESP_LOGE(THIS_MODULE_NAME, "Failed to fire sensor event");
    }
}


