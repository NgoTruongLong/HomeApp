/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "pump_control.h"
#include "sensor_control.h"
#include "wifi_control.h"
#include "time_control.h"
#include "screen_control.h"
#include "audio_control.h"
#include "micro_sdcard_control.h"

void app_main(void)
{
    APP_RESULT ret = APP_OK;

    ret = pump_init();
    ASSERT(ret == APP_OK, ret);

    ret = sensor_init();
    ASSERT(ret == APP_OK, ret);

    ret = wifi_control_init();
    ASSERT(ret == APP_OK, ret);

    ret = wifi_control_wait_connected(15000);
    ASSERT(ret == APP_OK, ret);

    // vTaskDelay(pdMS_TO_TICKS(10000)); // Wait for a second before starting time sync
    
    ret = time_control_init();
    ASSERT(ret == APP_OK, ret);

    ret = screen_init();
    ASSERT(ret == APP_OK, ret);

    /* SD card must be mounted before the audio task streams music from it. */
    ret = micro_sdcard_init();
    ASSERT(ret == APP_OK, ret);

    ret = audio_init();
    ASSERT(ret == APP_OK, ret);
}
