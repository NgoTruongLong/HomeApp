/*
 * xiaozhi_ws.h - Client WebSocket tới Xiaozhi AI.
 *
 * - Tự lấy url/token: đọc NVS (namespace "websocket"); nếu chưa có thì gọi OTA
 *   activation https://api.tenclass.net/xiaozhi/ota/ rồi lưu vào NVS.
 * - Gửi "hello" khi kết nối, "listen start/stop" để điều khiển phiên.
 * - Audio: khung nhị phân 4 byte header [type|reserved|size BE] + Opus payload.
 */
#ifndef XIAOZHI_WS_H
#define XIAOZHI_WS_H

#include "debug.h"
#include <stddef.h>
#include <stdbool.h>

/******************************* DATA TYPES *******************************/
/* Callback khi nhận 1 frame Opus từ server (giọng TTS trả về). */
typedef void (*xz_audio_cb_t)(const uint8_t *opus, size_t len);
/* Callback cho message JSON: type = "hello"/"stt"/"llm"/"tts"/"goodbye"; */
typedef void (*xz_msg_cb_t)(const char *type, const char *text, const char *state);

/******************************* FUNCTIONS PROTOTYPE *******************************/
APP_RESULT xz_init(void);
/* Kết nối WS (tự activation qua OTA nếu NVS chưa có url/token). */
APP_RESULT xz_connect(void);
void       xz_disconnect(void);
bool       xz_is_connected(void);
bool       xz_is_ready(void);         /* đã nhận "hello" từ server */
APP_RESULT xz_send_listen_start(void);
APP_RESULT xz_send_listen_stop(void);
APP_RESULT xz_send_audio(const uint8_t *opus, size_t len);
void       xz_set_callbacks(xz_audio_cb_t audio_cb, xz_msg_cb_t msg_cb);
APP_RESULT xz_mcp_send_payload(const char* payload);
#endif /* XIAOZHI_WS_H */
