/*
 * voice_chat.h - Trò chuyện giọng nói với Xiaozhi AI.
 *
 * Luồng: bật -> kết nối WS (activation OTA nếu cần) -> server hello ->
 *         gửi listen start -> mic INMP441 -> Opus encode -> gửi WS
 *         WS -> Opus decode -> audio_stream_write -> loa MAX98357A.
 */
#ifndef VOICE_CHAT_H
#define VOICE_CHAT_H

#include "debug.h"
#include <stdbool.h>

/******************************* DEFINITIONS *******************************/
#define VOICE_SAMPLE_RATE   (24000)   /* server xiaozhi tra ve 24000 trong ban hello */
#define VOICE_FRAME_MS      (60)
#define VOICE_FRAME_SAMPLES (VOICE_SAMPLE_RATE / 1000 * VOICE_FRAME_MS)   /* 1440 */

/******************************* DATA TYPES *******************************/
typedef enum {
    VOICE_STATE_IDLE = 0,   /* chưa bật */
    VOICE_STATE_CONNECTING, /* đang kết nối / kích hoạt */
    VOICE_STATE_LISTENING,  /* đang nghe (mic gửi lên server) */
    VOICE_STATE_THINKING,   /* server đang xử lý */
    VOICE_STATE_SPEAKING,   /* đang phát giọng AI */
    VOICE_STATE_ERROR,      /* lỗi */
} voice_state_t;

/******************************* FUNCTIONS PROTOTYPE *******************************/
APP_RESULT    voice_chat_init(void);
/* UI: bật/tắt phiên trò chuyện (toggle). */
APP_RESULT    voice_chat_toggle(void);
bool          voice_chat_is_active(void);
voice_state_t voice_chat_get_state(void);
/* Chuỗi trạng thái hiển thị trên UI (ASCII). */
const char   *voice_chat_state_text(void);
/* Text mới nhất (STT người dùng hoặc câu AI đang nói). */
const char   *voice_chat_get_last_text(void);

#endif /* VOICE_CHAT_H */
