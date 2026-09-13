#ifndef AUDIO_CONTROL
#define AUDIO_CONTROL

#include "debug.h"
#include "commons.h"
#include "driver/i2s_std.h"

/******************************* DEFINITIONS *******************************/
/* Dài tối đa đường dẫn bài hát trên SD (khớp SONG_NAME_MAX_LEN của micro_sdcard). */
#define AUDIO_SONG_PATH_MAX    (128)

/******************************* DATA TYPES *******************************/
/* Trạng thái phát nhạc (đọc bằng audio_get_status để vẽ UI). */
typedef enum {
    AUDIO_STATE_IDLE = 0,   /* không có bài nào được nạp */
    AUDIO_STATE_PLAYING,    /* đang phát */
    AUDIO_STATE_PAUSED,     /* đã nạp bài nhưng tạm dừng / đã phát hết */
    AUDIO_STATE_STREAMING,  /* đang phát PCM trực tiếp (giọng AI) */
} audio_state_t;

typedef struct {
    audio_state_t state;                        /* trạng thái hiện tại */
    char          song_path[AUDIO_SONG_PATH_MAX]; /* đường dẫn bài đang phát ("" khi IDLE) */
    uint32_t      elapsed_ms;                   /* thời gian đã phát (ms) */
    uint32_t      duration_ms;                  /* tổng thời lượng (ms, 0 nếu chưa biết) */
} audio_status_t;

typedef struct {
    i2s_chan_handle_t i2s_speaker_handle;
} audio_t;

/******************************* FUNCTIONS PROTOTYPE *******************************/
APP_RESULT audio_init(void);
APP_RESULT audio_request_new_song(const char *song_path);
APP_RESULT audio_pause(void);
APP_RESULT audio_resume(void);
APP_RESULT audio_stop(void);
APP_RESULT audio_seek(uint32_t seek_ms);
APP_RESULT audio_get_status(audio_status_t *status);

/* --- Phát PCM trực tiếp (dùng cho giọng trả về của AI) --- */
/* Mở luồng phát: dừng nhạc đang phát, đổi I2S sang sample_rate, bật chế độ stream. */
APP_RESULT audio_stream_open(uint32_t sample_rate);
/* Đẩy PCM 16-bit mono vào hàng đợi phát (không block lâu). */
APP_RESULT audio_stream_write(const int16_t *pcm, size_t samples);
/* Đóng luồng phát, về trạng thái IDLE. */
APP_RESULT audio_stream_close(void);
/******************************* VARIABLES *******************************/

/******************************* FUNCTIONS IMPLEMENTATION *******************************/

#endif /* AUDIO_CONTROL */