
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
#include "micro_sdcard_control.h"

/******************************* DEFINITIONS *******************************/
#define THIS_MODULE_NAME "audio_control"

#define SD_MUSIC_FILE       "/sdcard/music.wav"  /* single WAV file played in a loop */
#define AUDIO_CHUNK_FRAMES  (1024)                /* PCM frames decoded per block */

/* Am luong Q15: 32767 = max (0 dB), 16384 = -6 dB, 8192 = -12 dB, 4096 = -18 dB */
#define AUDIO_VOLUME_Q15    (8192)

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

/******************************* VARIABLES *******************************/
static audio_t audio_control;
/******************************* FUNCTIONS IMPLEMENTATION *******************************/
APP_RESULT audio_init() {
    APP_RESULT ret = APP_OK;
    memset(&audio_control, 0, sizeof(audio_t));

    ret = max98357a_init(&audio_control.i2s_speaker_handle);
    ASSERT_CRITICAL(ret == APP_OK, ret, ret);

    ret = xTaskCreate(audio_task, "audio_task", AUDIO_TASK_STACK_SIZE, NULL, AUDIO_TASK_PRIOR, NULL);
    ASSERT_CRITICAL(ret == pdPASS, ret, ret);

    return APP_OK;
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
    wav_info_t info = {0};
    int32_t *out = NULL;
    uint8_t *raw = NULL;
    const size_t raw_sz = AUDIO_CHUNK_FRAMES * 4;   /* 2ch x 16-bit worst case */
    int i = 0;

    out = (int32_t *)heap_caps_malloc(AUDIO_CHUNK_FRAMES * sizeof(int32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    raw = (uint8_t *)heap_caps_malloc(raw_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (out == NULL || raw == NULL) {
        ESP_LOGE(THIS_MODULE_NAME, "Failed to allocate audio buffers (%d / %d bytes)",
                 (int)(AUDIO_CHUNK_FRAMES * sizeof(int32_t)), (int)raw_sz);
        goto cleanup;
    }

    ESP_LOGI(THIS_MODULE_NAME, "Audio task started, waiting for %s ...", SD_MUSIC_FILE);
    sdcard_t *sdcard = micro_sdcard_get_control();

    printf("Found %d songs on SD card:\n", sdcard->playlist.count);
    for (i = 0; i < sdcard->playlist.count; i++) {
        printf(" - %s\n", sdcard->playlist.songs[i]);
    }

    while (1) {
        for (i = 0; i < sdcard->playlist.count; i++) {
            FILE *fp = fopen(sdcard->playlist.songs[i], "rb");
            if (fp == NULL) {
                ESP_LOGW(THIS_MODULE_NAME, "Cannot open %s (SD mounted? file copied?) - retry in 3 s",
                        sdcard->playlist.songs[i]);
                vTaskDelay(pdMS_TO_TICKS(3000));
                continue;
            }

            if (wav_parse_header(fp, &info) != ESP_OK) {
                ESP_LOGE(THIS_MODULE_NAME, "%s is not a supported 16-bit PCM WAV file", sdcard->playlist.songs[i]);
                fclose(fp);
                vTaskDelay(pdMS_TO_TICKS(3000));
                continue;
            }

            ESP_LOGI(THIS_MODULE_NAME, "Playing %s: %lu Hz, %u ch, %u-bit, %lu bytes",
                    sdcard->playlist.songs[i], (unsigned long)info.sample_rate,
                    info.num_channels, info.bits_per_sample, (unsigned long)info.data_size);

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
                continue;
            }

            /* Stream -> convert -> write to I2S. When the file ends, restart it. */
            for (;;) {
                size_t n = wav_decode_frames(fp, &info, raw, out, AUDIO_CHUNK_FRAMES);
                if (n == 0) {                       /* end of file: loop from the top */
                    // fseek(fp, (long)info.data_offset, SEEK_SET);
                    break;
                }

                size_t written = 0;
                err = i2s_channel_write(audio_control.i2s_speaker_handle, out,
                                        n * sizeof(int32_t), &written, portMAX_DELAY);
                if (err != ESP_OK) {
                    ESP_LOGW(THIS_MODULE_NAME, "i2s_channel_write failed: %s", esp_err_to_name(err));
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2000));  /* wait 1s before restarting the playlist */
    }
    

cleanup:
    if (raw != NULL) free(raw);
    if (out != NULL) free(out);
    vTaskDelete(NULL);
}