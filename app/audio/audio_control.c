
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "audio_control.h"
#include "max98357a.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "task_define.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"

/******************************* DEFINITIONS *******************************/
#define THIS_MODULE_NAME "audio_control"

#define AUDIO_CHUNK_FRAMES  (1024)                /* PCM frames decoded per block */

/* Am luong Q15: 32767 = max (0 dB), 16384 = -6 dB, 8192 = -12 dB, 4096 = -18 dB */
#define AUDIO_VOLUME_Q15    (8192)

/* Số lần đọc lại khi SD trả lỗi (timeout) trước khi tạm dừng phát. */
#define AUDIO_IO_RETRY_MAX  (5)

/* Ring buffer cho luồng PCM phát trực tiếp (giọng AI trả về). */
#define AUDIO_STREAM_RB_BYTES  (16 * 1024)

/******************************* FUNCTIONS PROTOTYPE *******************************/
void audio_task(void *arg);
/******************************* DATA TYPES *******************************/
/* Minimal RIFF/WAVE header info needed for 16-bit PCM playback. */
typedef struct {
    uint32_t sample_rate;     /* e.g. 44100 */
    uint16_t num_channels;    /* 1 = mono, 2 = stereo (downmixed) */
    uint16_t bits_per_sample; /* must be 16 */
    uint32_t data_offset;     /* byte offset of the 'data' chunk payload */
    uint32_t data_size;       /* payload size in bytes */
} wav_info_t;

/* "Động cơ" phát WAV - chỉ audio_task được sửa các trường này. */
typedef struct {
    FILE       *fp;               /* file đang mở (NULL nếu không có bài) */
    wav_info_t  info;
    uint32_t    bytes_per_frame;  /* channels * bits/8 */
    uint32_t    total_frames;     /* tổng số frame trong data */
    uint32_t    pos_frames;       /* số frame đã ghi ra I2S */
    bool        has_song;         /* đã nạp & parse OK, fp mở */
    bool        paused;           /* có bài nhưng đang tạm dừng (gồm cả phát hết) */
    uint8_t     io_retry;         /* số lần thử lại khi đọc SD lỗi */
    bool        stream_active;    /* true: phát PCM trực tiếp (bỏ qua file) */
    char        path[AUDIO_SONG_PATH_MAX];
} audio_engine_t;

/* Lệnh từ UI -> audio_task (single-buffer, task đọc rồi xoá). */
typedef struct {
    bool      new_song;
    char      new_song_path[AUDIO_SONG_PATH_MAX];
    bool      pause;
    bool      resume;
    bool      stop;
    bool      seek;
    uint32_t  seek_ms;
    bool      stream_open;
    bool      stream_close;
    uint32_t  stream_rate;
} audio_cmd_t;

/******************************* VARIABLES *******************************/
static audio_t           audio_control;
static audio_engine_t    audio_eng;
static audio_cmd_t       audio_cmd;
static SemaphoreHandle_t audio_cmd_mux;
static SemaphoreHandle_t audio_status_mux;
static audio_status_t    audio_status;
static RingbufHandle_t   s_stream_rb;

/* Helper prototypes (định nghĩa phía dưới). */
static void  audio_status_publish(void);
static void  audio_engine_close(void);
static bool  audio_engine_load(const char *path);
static void  audio_engine_seek_ms(uint32_t ms);
static esp_err_t wav_parse_header(FILE *fp, wav_info_t *info);
/******************************* FUNCTIONS IMPLEMENTATION *******************************/
APP_RESULT audio_init() {
    APP_RESULT ret = APP_OK;
    memset(&audio_control, 0, sizeof(audio_t));
    /* Log heap trước khi cấp DMA cho I2S để dễ kiểm tra nếu vẫn thiếu bộ nhớ. */
    ESP_LOGI(THIS_MODULE_NAME,
             "heap before I2S: free=%u  DMA=%u  min_free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));

    audio_cmd_mux    = xSemaphoreCreateMutex();
    audio_status_mux = xSemaphoreCreateMutex();
    ASSERT_CRITICAL(audio_cmd_mux != NULL && audio_status_mux != NULL, ret, APP_ERROR);

    /* Ring buffer PCM dat trong PSRAM: FreeRTOS luon cap phat tu RAM noi bo
     * (portFREERTOS_HEAP_CAPS = MALLOC_CAP_INTERNAL|8BIT), ma RAM noi bo rat chat. */
    s_stream_rb = xRingbufferCreateWithCaps(AUDIO_STREAM_RB_BYTES, RINGBUF_TYPE_BYTEBUF,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ASSERT_CRITICAL(s_stream_rb != NULL, ret, APP_ERROR);

    ret = max98357a_init(&audio_control.i2s_speaker_handle);
    ASSERT_CRITICAL(ret == APP_OK, ret, ret);

    ret = xTaskCreate(audio_task, "audio_task", AUDIO_TASK_STACK_SIZE, NULL, AUDIO_TASK_PRIOR, NULL);
    ASSERT_CRITICAL(ret == pdPASS, ret, ret);

    return APP_OK;
}

/* Đổi số frame phát thành thời gian (ms). */
static uint32_t audio_frames_to_ms(uint32_t frames, uint32_t sample_rate)
{
    if (sample_rate == 0) {
        return 0;
    }
    return (uint32_t)(((uint64_t)frames * 1000ULL) / sample_rate);
}

/* Đổi sample rate của I2S TX (chỉ đổi được khi channel đang disable). */
static void audio_i2s_set_rate(uint32_t sample_rate)
{
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    esp_err_t err = i2s_channel_disable(audio_control.i2s_speaker_handle);
    if (err == ESP_OK) {
        err = i2s_channel_reconfig_std_clock(audio_control.i2s_speaker_handle, &clk_cfg);
    }
    if (err == ESP_OK) {
        err = i2s_channel_enable(audio_control.i2s_speaker_handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(THIS_MODULE_NAME, "i2s set rate %lu Hz failed: %s",
                 (unsigned long)sample_rate, esp_err_to_name(err));
    }
}

/* Xả sạch ring buffer PCM (dùng khi mở/đóng luồng). */
static void audio_stream_rb_drain(void)
{
    size_t len = 0;
    void *p;

    if (s_stream_rb == NULL) {
        return;
    }
    while ((p = xRingbufferReceiveUpTo(s_stream_rb, &len, 0,
                                       AUDIO_CHUNK_FRAMES * 2)) != NULL) {
        vRingbufferReturnItem(s_stream_rb, p);
    }
}

/* Xuất bản trạng thái hiện tại cho UI (đọc bằng audio_get_status). */
static void audio_status_publish(void)
{
    audio_state_t state   = AUDIO_STATE_IDLE;
    uint32_t      elapsed = 0, duration = 0;

    if (audio_eng.stream_active) {
        state = AUDIO_STATE_STREAMING;
    } else if (audio_eng.has_song) {
        state    = audio_eng.paused ? AUDIO_STATE_PAUSED : AUDIO_STATE_PLAYING;
        elapsed  = audio_frames_to_ms(audio_eng.pos_frames, audio_eng.info.sample_rate);
        duration = audio_frames_to_ms(audio_eng.total_frames, audio_eng.info.sample_rate);
    }

    if (audio_status_mux != NULL &&
        xSemaphoreTake(audio_status_mux, portMAX_DELAY) == pdTRUE) {
        audio_status.state       = state;
        audio_status.elapsed_ms  = elapsed;
        audio_status.duration_ms = duration;
        if (state != AUDIO_STATE_IDLE && audio_eng.path[0]) {
            snprintf(audio_status.song_path, sizeof(audio_status.song_path), "%s", audio_eng.path);
        } else {
            audio_status.song_path[0] = '\0';
        }
        xSemaphoreGive(audio_status_mux);
    }
}

/* Đóng file hiện tại & reset "động cơ" về IDLE. */
static void audio_engine_close(void)
{
    if (audio_eng.fp != NULL) {
        fclose(audio_eng.fp);
        audio_eng.fp = NULL;
    }
    memset(&audio_eng.info, 0, sizeof(audio_eng.info));
    audio_eng.bytes_per_frame = 0;
    audio_eng.total_frames    = 0;
    audio_eng.pos_frames      = 0;
    audio_eng.has_song        = false;
    audio_eng.paused          = false;
    audio_eng.io_retry        = 0;
    audio_eng.stream_active   = false;
    audio_eng.path[0]         = '\0';
}

/* Nhảy tới vị trí (ms) trong bài đang phát. */
static void audio_engine_seek_ms(uint32_t ms)
{
    uint32_t rate, target;

    if (!audio_eng.has_song || audio_eng.fp == NULL) {
        return;
    }
    rate = audio_eng.info.sample_rate;
    if (rate == 0 || audio_eng.total_frames == 0) {
        return;
    }
    target = (uint32_t)(((uint64_t)ms * rate) / 1000ULL);
    if (target > audio_eng.total_frames) {
        target = audio_eng.total_frames;
    }
    if (fseek(audio_eng.fp,
              (long)audio_eng.info.data_offset + (long)target * (long)audio_eng.bytes_per_frame,
              SEEK_SET) != 0) {
        return;
    }
    audio_eng.pos_frames = target;
}

/* Mở file WAV, parse header, chỉnh lại clock I2S theo sample rate. */
static bool audio_engine_load(const char *path)
{
    wav_info_t info = {0};
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        ESP_LOGW(THIS_MODULE_NAME, "Cannot open %s (SD mounted? file exists?)", path);
        return false;
    }
    if (wav_parse_header(fp, &info) != ESP_OK) {
        ESP_LOGE(THIS_MODULE_NAME, "%s is not a supported 16-bit PCM WAV file", path);
        fclose(fp);
        return false;
    }
    /* The I2S clock can only be changed while the channel is disabled:
     * disable -> set the file's sample rate -> re-enable. */
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(info.sample_rate);
    esp_err_t err = i2s_channel_disable(audio_control.i2s_speaker_handle);
    if (err == ESP_OK) {
        err = i2s_channel_reconfig_std_clock(audio_control.i2s_speaker_handle, &clk_cfg);
    }
    if (err == ESP_OK) {
        err = i2s_channel_enable(audio_control.i2s_speaker_handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(THIS_MODULE_NAME, "i2s reconfig clock to %lu Hz failed: %s",
                 (unsigned long)info.sample_rate, esp_err_to_name(err));
    }
    if (fseek(fp, (long)info.data_offset, SEEK_SET) != 0) {
        ESP_LOGE(THIS_MODULE_NAME, "fseek to audio data failed");
        fclose(fp);
        return false;
    }

    audio_engine_close();
    audio_eng.fp              = fp;
    audio_eng.info            = info;
    audio_eng.bytes_per_frame = (uint32_t)info.num_channels * (info.bits_per_sample / 8);
    audio_eng.total_frames    = info.data_size / audio_eng.bytes_per_frame;
    audio_eng.pos_frames      = 0;
    audio_eng.paused          = false;
    audio_eng.io_retry        = 0;
    audio_eng.has_song        = true;
    snprintf(audio_eng.path, sizeof(audio_eng.path), "%s", path);

    ESP_LOGI(THIS_MODULE_NAME, "Playing %s: %lu Hz, %u ch, %u-bit, %lu bytes",
             path, (unsigned long)info.sample_rate, info.num_channels,
             info.bits_per_sample, (unsigned long)info.data_size);
    return true;
}

/* Parse the RIFF/WAVE header of a 16-bit PCM file and leave the FILE*
 * positioned at the 'data' chunk payload. */
static esp_err_t wav_parse_header(FILE *fp, wav_info_t *info)
{
    uint8_t hdr[12];
    if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr) ||
        memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    for (;;) {
        uint8_t ch[8];
        if (fread(ch, 1, sizeof(ch), fp) != sizeof(ch)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        uint32_t size = (uint32_t)ch[4] | ((uint32_t)ch[5] << 8) |
                        ((uint32_t)ch[6] << 16) | ((uint32_t)ch[7] << 24);

        if (memcmp(ch, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (size < 16 || fread(fmt, 1, sizeof(fmt), fp) != sizeof(fmt)) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            uint16_t audio_format = fmt[0] | (fmt[1] << 8);
            uint16_t channels     = fmt[2] | (fmt[3] << 8);
            uint32_t rate         = (uint32_t)fmt[4] | ((uint32_t)fmt[5] << 8) |
                                    ((uint32_t)fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
            uint16_t bits         = fmt[14] | (fmt[15] << 8);

            if (audio_format != 1 || bits != 16) {
                ESP_LOGE(THIS_MODULE_NAME, "Unsupported WAV: format=%u bits=%u (need PCM 16-bit)",
                         audio_format, bits);
                return ESP_ERR_NOT_SUPPORTED;
            }
            info->num_channels    = channels;
            info->bits_per_sample = bits;
            info->sample_rate     = rate;

            if (size > 16 && fseek(fp, (long)(size - 16), SEEK_CUR) != 0) {
                return ESP_ERR_INVALID_RESPONSE;
            }
        } else if (memcmp(ch, "data", 4) == 0) {
            info->data_offset = (uint32_t)ftell(fp);
            info->data_size   = size;
            return ESP_OK;
        } else {
            /* skip unknown chunk (e.g. LIST, fact, ...) */
            if (fseek(fp, (long)size, SEEK_CUR) != 0) {
                return ESP_ERR_INVALID_RESPONSE;
            }
        }
    }
}

/* Read up to max_frames PCM frames from the 'data' payload and convert them
 * into the mono 32-bit-slot buffer used by the MAX98357A (16-bit left-justified).
 * Stereo sources are averaged into a single mono channel. */
static size_t wav_decode_frames(FILE *fp, const wav_info_t *info,
                                uint8_t *raw, int32_t *out, size_t max_frames)
{
    const size_t bytes_per_frame = (size_t)info->num_channels * (info->bits_per_sample / 8);
    const long   data_end        = (long)(info->data_offset + info->data_size);
    long pos = ftell(fp);

    if (pos >= data_end) {
        return 0;   /* reached end of audio data */
    }

    size_t frames = (size_t)(data_end - pos) / bytes_per_frame;
    if (frames > max_frames) {
        frames = max_frames;
    }

    size_t got = fread(raw, 1, frames * bytes_per_frame, fp);
    size_t n   = got / bytes_per_frame;

    if (info->num_channels == 1) {
        const int16_t *p = (const int16_t *)raw;
        for (size_t i = 0; i < n; i++) {
            out[i] = (int32_t)(((int32_t)p[i] * AUDIO_VOLUME_Q15) >> 15) << 16;
        }
    } else {   /* stereo -> mono average */
        const int16_t *p = (const int16_t *)raw;
        for (size_t i = 0; i < n; i++) {
            int32_t s = ((int32_t)p[2 * i] + (int32_t)p[2 * i + 1]) / 2;
            out[i] = (int32_t)((s * AUDIO_VOLUME_Q15) >> 15) << 16;
        }
    }
    return n;
}

void audio_task(void *arg)
{
    int32_t *out = NULL;
    uint8_t *raw = NULL;
    const size_t raw_sz = AUDIO_CHUNK_FRAMES * 4;   /* 2ch x 16-bit worst case */

    out = (int32_t *)heap_caps_malloc(AUDIO_CHUNK_FRAMES * sizeof(int32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    raw = (uint8_t *)heap_caps_malloc(raw_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (out == NULL || raw == NULL) {
        ESP_LOGE(THIS_MODULE_NAME, "Failed to allocate audio buffers (%d / %d bytes)",
                 (int)(AUDIO_CHUNK_FRAMES * sizeof(int32_t)), (int)raw_sz);
        goto cleanup;
    }

    ESP_LOGI(THIS_MODULE_NAME, "Audio task started, waiting for song requests...");

    while (1) {
        /* --- 1. Nhận lệnh từ UI (đọc rồi xoá toàn bộ cờ) --- */
        audio_cmd_t cmd;
        memset(&cmd, 0, sizeof(cmd));

        if (audio_cmd_mux != NULL && xSemaphoreTake(audio_cmd_mux, portMAX_DELAY) == pdTRUE) {
            cmd = audio_cmd;
            audio_cmd.new_song = false;
            audio_cmd.pause    = false;
            audio_cmd.resume   = false;
            audio_cmd.stop     = false;
            audio_cmd.seek     = false;
            audio_cmd.stream_open  = false;
            audio_cmd.stream_close = false;
            xSemaphoreGive(audio_cmd_mux);
        }

        if (cmd.stop) {
            audio_engine_close();
            audio_status_publish();
        }
        if (cmd.new_song) {
            audio_engine_close();
            audio_engine_load(cmd.new_song_path);
            audio_status_publish();
        }
        if (cmd.pause) {
            if (audio_eng.has_song && !audio_eng.paused) {
                audio_eng.paused = true;
                audio_status_publish();
            }
        }
        if (cmd.resume) {
            if (audio_eng.has_song && audio_eng.paused) {
                /* Phát hết bài (pos == total): bấm Play sẽ chơi lại từ đầu. */
                if (audio_eng.total_frames > 0 &&
                    audio_eng.pos_frames >= audio_eng.total_frames) {
                    audio_engine_seek_ms(0);
                }
                audio_eng.paused = false;
                audio_status_publish();
            }
        }
        if (cmd.seek) {
            if (audio_eng.has_song) {
                audio_engine_seek_ms(cmd.seek_ms);
                audio_status_publish();
            }
        }
        if (cmd.stream_open) {
            audio_engine_close();
            audio_i2s_set_rate(cmd.stream_rate ? cmd.stream_rate : 16000);
            audio_stream_rb_drain();
            audio_eng.stream_active = true;
            audio_status_publish();
        }
        if (cmd.stream_close) {
            audio_eng.stream_active = false;
            audio_stream_rb_drain();
            audio_status_publish();
        }

        /* --- 2. Ưu tiên phát PCM trực tiếp (giọng AI trả về) --- */
        if (audio_eng.stream_active) {
            size_t len = 0;
            void *p = xRingbufferReceiveUpTo(s_stream_rb, &len, pdMS_TO_TICKS(20),
                                             AUDIO_CHUNK_FRAMES * 2);
            if (p != NULL) {
                size_t frames = len / 2;
                const int16_t *s = (const int16_t *)p;
                for (size_t i = 0; i < frames; i++) {
                    out[i] = (int32_t)s[i] << 16;
                }
                vRingbufferReturnItem(s_stream_rb, p);
                if (frames > 0) {
                    size_t written = 0;
                    esp_err_t err = i2s_channel_write(audio_control.i2s_speaker_handle, out,
                                                      frames * sizeof(int32_t), &written,
                                                      portMAX_DELAY);
                    if (err != ESP_OK) {
                        ESP_LOGW(THIS_MODULE_NAME, "i2s write (stream) failed: %s",
                                 esp_err_to_name(err));
                    }
                }
            }
        } else if (audio_eng.has_song && !audio_eng.paused && audio_eng.fp != NULL) {
            size_t n = wav_decode_frames(audio_eng.fp, &audio_eng.info,
                                         raw, out, AUDIO_CHUNK_FRAMES);
            if (n == 0) {
                /* Phân biệt lỗi đọc SD (timeout...) với hết bài (EOF). */
                if (ferror(audio_eng.fp) && audio_eng.io_retry < AUDIO_IO_RETRY_MAX) {
                    audio_eng.io_retry++;
                    clearerr(audio_eng.fp);
                    ESP_LOGW(THIS_MODULE_NAME, "SD read error on %s, retry %u/%u",
                             audio_eng.path, audio_eng.io_retry, AUDIO_IO_RETRY_MAX);
                    vTaskDelay(pdMS_TO_TICKS(50));
                    continue;
                }
                if (ferror(audio_eng.fp)) {
                    clearerr(audio_eng.fp);
                    ESP_LOGE(THIS_MODULE_NAME, "SD read failed on %s after %u retries",
                             audio_eng.path, AUDIO_IO_RETRY_MAX);
                }
                /* Hết bài: dừng ở cuối (paused) để UI hiện nút Play nghe lại. */
                audio_eng.pos_frames = audio_eng.total_frames;
                audio_eng.paused     = true;
                ESP_LOGI(THIS_MODULE_NAME, "Finished %s", audio_eng.path);
                audio_status_publish();
            } else {
                size_t written = 0;
                esp_err_t err = i2s_channel_write(audio_control.i2s_speaker_handle, out,
                                                  n * sizeof(int32_t), &written, portMAX_DELAY);
                if (err != ESP_OK) {
                    ESP_LOGW(THIS_MODULE_NAME, "i2s_channel_write failed: %s", esp_err_to_name(err));
                    vTaskDelay(pdMS_TO_TICKS(10));
                } else {
                    audio_eng.io_retry = 0;
                    audio_eng.pos_frames += n;
                    audio_status_publish();
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    

cleanup:
    if (raw != NULL) free(raw);
    if (out != NULL) free(out);
    vTaskDelete(NULL);
}

APP_RESULT audio_request_new_song(const char *song_path) {
    ASSERT_CRITICAL(song_path != NULL && song_path[0] != '\0', APP_ERROR, APP_ERROR);
    if (audio_cmd_mux != NULL && xSemaphoreTake(audio_cmd_mux, portMAX_DELAY) == pdTRUE) {
        snprintf(audio_cmd.new_song_path, sizeof(audio_cmd.new_song_path), "%s", song_path);
        audio_cmd.new_song = true;
        xSemaphoreGive(audio_cmd_mux);
    } else {
        /* audio chưa init: lưu thẳng để task xử lý khi chạy. */
        snprintf(audio_cmd.new_song_path, sizeof(audio_cmd.new_song_path), "%s", song_path);
        audio_cmd.new_song = true;
    }
    ESP_LOGI(THIS_MODULE_NAME, "Request new song: %s", song_path);
    return APP_OK;
}

APP_RESULT audio_pause(void) {
    if (audio_cmd_mux != NULL && xSemaphoreTake(audio_cmd_mux, portMAX_DELAY) == pdTRUE) {
        audio_cmd.pause = true;
        xSemaphoreGive(audio_cmd_mux);
    } else {
        audio_cmd.pause = true;
    }
    return APP_OK;
}

APP_RESULT audio_resume(void) {
    if (audio_cmd_mux != NULL && xSemaphoreTake(audio_cmd_mux, portMAX_DELAY) == pdTRUE) {
        audio_cmd.resume = true;
        xSemaphoreGive(audio_cmd_mux);
    } else {
        audio_cmd.resume = true;
    }
    return APP_OK;
}

APP_RESULT audio_stop(void) {
    if (audio_cmd_mux != NULL && xSemaphoreTake(audio_cmd_mux, portMAX_DELAY) == pdTRUE) {
        audio_cmd.stop = true;
        xSemaphoreGive(audio_cmd_mux);
    } else {
        audio_cmd.stop = true;
    }
    return APP_OK;
}

APP_RESULT audio_seek(uint32_t seek_ms) {
    if (audio_cmd_mux != NULL && xSemaphoreTake(audio_cmd_mux, portMAX_DELAY) == pdTRUE) {
        audio_cmd.seek_ms = seek_ms;
        audio_cmd.seek    = true;
        xSemaphoreGive(audio_cmd_mux);
    } else {
        audio_cmd.seek_ms = seek_ms;
        audio_cmd.seek    = true;
    }
    return APP_OK;
}

APP_RESULT audio_get_status(audio_status_t *status) {
    ASSERT_CRITICAL(status != NULL, APP_ERROR, APP_ERROR);
    if (audio_status_mux != NULL && xSemaphoreTake(audio_status_mux, portMAX_DELAY) == pdTRUE) {
        *status = audio_status;
        xSemaphoreGive(audio_status_mux);
    } else {
        memset(status, 0, sizeof(*status));   /* audio chưa init -> IDLE */
    }
    return APP_OK;
}

APP_RESULT audio_stream_open(uint32_t sample_rate) {
    if (audio_cmd_mux != NULL && xSemaphoreTake(audio_cmd_mux, portMAX_DELAY) == pdTRUE) {
        audio_cmd.stream_rate = sample_rate;
        audio_cmd.stream_open = true;
        xSemaphoreGive(audio_cmd_mux);
    } else {
        audio_cmd.stream_rate = sample_rate;
        audio_cmd.stream_open = true;
    }
    return APP_OK;
}

APP_RESULT audio_stream_write(const int16_t *pcm, size_t samples) {
    if (pcm == NULL || samples == 0 || s_stream_rb == NULL) {
        return APP_ERROR;
    }
    /* Chờ tối đa 200ms nếu buffer đầy để tránh block vô hạn task WebSocket. */
    if (xRingbufferSend(s_stream_rb, pcm, samples * sizeof(int16_t),
                        pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(THIS_MODULE_NAME, "stream buffer full, drop %u samples", (unsigned)samples);
        return APP_ERROR;
    }
    return APP_OK;
}

APP_RESULT audio_stream_close(void) {
    if (audio_cmd_mux != NULL && xSemaphoreTake(audio_cmd_mux, portMAX_DELAY) == pdTRUE) {
        audio_cmd.stream_close = true;
        xSemaphoreGive(audio_cmd_mux);
    } else {
        audio_cmd.stream_close = true;
    }
    return APP_OK;
}