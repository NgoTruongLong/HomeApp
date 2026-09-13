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
#include "voice_chat.h"

void app_main(void)
{
    APP_RESULT ret = APP_OK;

    ret = pump_init();
    ASSERT(ret == APP_OK, ret);

    ret = sensor_init();
    ASSERT(ret == APP_OK, ret);

    /* WiFi: khởi tạo STA, nếu có mạng đã lưu thì tự kết nối (không block). */
    ret = wifi_control_init();
    ASSERT(ret == APP_OK, ret);

    /* SNTP non-blocking: task nền sẽ đồng bộ giờ khi có WiFi.
       Nếu chưa có mạng, người dùng chọn & kết nối trên màn hình (Phase 3). */
    ret = time_control_init();
    ASSERT(ret == APP_OK, ret);

    /* UI khởi động ngay để thao tác kể cả khi chưa có mạng. */
    ret = screen_init();
    ASSERT(ret == APP_OK, ret);

    /* SD card must be mounted before the audio task streams music from it. */
    ret = micro_sdcard_init();
    ASSERT(ret == APP_OK, ret);

#if 1
    ret = audio_init();
    ASSERT(ret == APP_OK, ret);
#endif

    /* Voice chat voi Xiaozhi AI (INMP441 + Opus + WebSocket). Can audio + WiFi. */
    ret = voice_chat_init();
    ASSERT(ret == APP_OK, ret);

}
