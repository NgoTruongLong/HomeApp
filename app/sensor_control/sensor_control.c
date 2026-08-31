#include "debug.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sensor_control.h"
#include "freertos/idf_additions.h"
#include "env_sensor.h"
#include "soc/gpio_num.h"
#include "task_define.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "pms.h"


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
    env_sensor_reading_t reading;
    pms7003_data_t pms_data;
    pms_config_t pms_config;
    i2c_master_bus_handle_t bus;
    env_sensor_t sensor;
    QueueHandle_t queue;
    esp_timer_handle_t periodic_timer;
} sensor_t;
/******************************* VARIABLES *******************************/
static sensor_t sensor_control;
void sensor_task(void *arg);
APP_RESULT sensor_aht20_bmp280_init();
APP_RESULT sensor_pms7003_init();
/******************************* FUNCTIONS IMPLEMENTATION *******************************/
APP_RESULT sensor_init() {
    APP_RESULT ret = APP_OK;

    sensor_aht20_bmp280_init();

    sensor_pms7003_init();

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

APP_RESULT sensor_aht20_bmp280_init() {
    APP_RESULT ret = APP_OK;
    ret = env_sensor_create_bus(&sensor_control.bus);
    ASSERT_CRITICAL(ret == APP_OK, ret, ret);
    ret = env_sensor_init(sensor_control.bus, &sensor_control.sensor);
    ASSERT_CRITICAL(ret == APP_OK, ret, ret);
    return ret;
}

APP_RESULT sensor_pms7003_init() {
    APP_RESULT ret = APP_OK;

    uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .stop_bits = UART_STOP_BITS_1,
        .parity = UART_PARITY_DISABLE,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };

    ret = uart_driver_install(SENSOR_PMS7003_UART_PORT, 1024 * 2, 0, 0, NULL, 0);
    ASSERT_CRITICAL(ret == APP_OK, ret, ret);

    ret = uart_param_config(SENSOR_PMS7003_UART_PORT, &uart_config);
    ASSERT_CRITICAL(ret == APP_OK, ret, ret);

    // IMPORTANT: route UART1 TX/RX to the physical GPIOs wired to the sensor.
    // Without this, the UART is not connected to any pin and nothing is sent/received.
    ret = uart_set_pin(SENSOR_PMS7003_UART_PORT, SENSOR_PMS7003_TX_PIN, SENSOR_PMS7003_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    ASSERT_CRITICAL(ret == APP_OK, ret, ret);

    sensor_control.pms_config.type = PMS_TYPE_7003;
    sensor_control.pms_config.set_gpio = SENSOR_PMS7003_SET;
    sensor_control.pms_config.reset_gpio = SENSOR_PMS7003_RESET;
    sensor_control.pms_config.uart_port = SENSOR_PMS7003_UART_PORT;

    ret = pms_init(&sensor_control.pms_config);
    ASSERT_CRITICAL(ret == APP_OK, ret, ret);

    ASSERT_CRITICAL(ret == APP_OK, ret, ret);
    return ret;
}

static int sensor_pms_read_bytes(uint8_t *buf, size_t buf_len, uint32_t timeout_ms)
{
    // Keep reading until buf_len bytes are collected or the timeout expires.
    // The PMS7003 only sends its frame AFTER measuring, so a single short
    // uart_read_bytes() call usually returns before the data arrives.
    int len = 0;
    TickType_t start = xTaskGetTickCount();
    while (len < (int)buf_len && (xTaskGetTickCount() - start) < pdMS_TO_TICKS(timeout_ms)) {
        int n = uart_read_bytes(SENSOR_PMS7003_UART_PORT, &buf[len], buf_len - len, pdMS_TO_TICKS(200));
        if (n > 0) {
            len += n;
        }
    }
    return len;
}

void sensor_task(void *arg) {
    APP_RESULT ret = APP_OK;
    task_msg_t msg;
    uint8_t data[32];
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
                memset(data, 0, sizeof(data));

                // First PMS cycle: switch the sensor to passive mode (once).
                // In passive mode the sensor only sends data when polled, so no
                // wake/sleep cycle is needed between reads. (Wake requires ~32s
                // reset+stabilize, which doesn't fit the 20s periodic timer.)
                if(pms_get_mode() != PMS_MODE_PASSIVE) {
                    ESP_LOGI(THIS_MODULE_NAME, "switching to passive mode...");
                    // Discard any data frames the sensor sent while still in active mode.
                    uart_flush_input(SENSOR_PMS7003_UART_PORT);
                    ret = pms_set_mode(PMS_MODE_PASSIVE);
                    ASSERT(ret == APP_OK, ret);
                    // Read the 8-byte command acknowledgment.
                    int len = sensor_pms_read_bytes(data, sizeof(data), 1000);
                    ESP_LOGI(THIS_MODULE_NAME, "PMS mode ack (%d bytes): %02x %02x %02x %02x %02x %02x %02x %02x",
                             len, data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]);
                    if (len > 0) {
                        pms_parse_cmd_response(data, len);
                    } else {
                        ESP_LOGW(THIS_MODULE_NAME, "no response to mode command");
                    }
                    break;  // actual reading starts from the next cycle
                }

                // A passive read command only works once the sensor is fully
                // active/stabilized (state == PMS_STATE_ACTIVE).
                if (pms_get_state() != PMS_STATE_ACTIVE) {
                    ESP_LOGW(THIS_MODULE_NAME, "PMS not ready yet (state=%d), skipping this cycle", pms_get_state());
                    break;
                }

                ESP_LOGI(THIS_MODULE_NAME, "Read PMS data");
                ret = pms_send_passive_read_cmd();
                ASSERT(ret == APP_OK, ret);

                // The sensor measures first and only then sends its 32-byte frame,
                // so wait up to 2.5s instead of one short 100ms read.
                int len = sensor_pms_read_bytes(data, sizeof(data), 2500);
                // DEBUG: print what actually arrived on the RX line
                ESP_LOGI(THIS_MODULE_NAME, "PMS raw (%d bytes): %02x %02x %02x %02x %02x %02x %02x %02x",
                         len, data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]);

                if (len > 0) {
                    ret = pms_parse_data(data, len);
                    if (ret != ESP_OK) {
                        ESP_LOGW(THIS_MODULE_NAME, "PMS frame invalid (ret=0x%x)", ret);
                        break;
                    }
                    ESP_LOGI(THIS_MODULE_NAME, "PMS data read successfully");

                    sensor_control.pms_data.pm1 = pms_get_data(PMS_FIELD_PM1_ATM);
                    sensor_control.pms_data.pm2_5 = pms_get_data(PMS_FIELD_PM2_5_ATM);
                    sensor_control.pms_data.pm10 = pms_get_data(PMS_FIELD_PM10_ATM);

                    printf("PMS7003: PM1.0=%d ug/m3, PM2.5=%d ug/m3, PM10=%d ug/m3\n",
                           sensor_control.pms_data.pm1,
                           sensor_control.pms_data.pm2_5,
                           sensor_control.pms_data.pm10);
                } else {
                    ESP_LOGE(THIS_MODULE_NAME, "Failed to read PMS data");
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
}


