
#include <string.h>
#include <math.h>

#include "audio_control.h"
#include "max98357a.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "task_define.h"

/******************************* DEFINITIONS *******************************/
#define THIS_MODULE_NAME "audio_control"

#define AUDIO_TONE_AMP  (0.30f)   /* volume 0..1, keep modest for the speaker */
#define AUDIO_PI        (3.14159265358979f)

/******************************* FUNCTIONS PROTOTYPE *******************************/
void audio_task(void *arg);
/******************************* DATA TYPES *******************************/
/* One note of the looping melody: {frequency Hz, duration ms}. freq = 0 => rest. */
typedef struct {
    uint16_t freq_hz;
    uint16_t dur_ms;
} melody_note_t;

/******************************* VARIABLES *******************************/
static audio_t audio_control;

/* Short looping melody (Twinkle Twinkle Little Star) for the speaker test. */
static const melody_note_t audio_melody[] = {
    {262, 240}, {262, 240}, {392, 240}, {392, 240}, {440, 240}, {440, 240}, {392, 480},
    {  0,  80},   /* brief rest */
    {349, 240}, {349, 240}, {330, 240}, {330, 240}, {294, 240}, {294, 240}, {262, 480},
    {  0, 400},   /* pause before looping again */
};
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

void audio_task(void *arg)
{
    const uint32_t sample_rate = SPEAKER_SAMPLE_RATE;   /* 16000 Hz */
    const uint32_t samples     = I2S_BUFFER_SIZE / 4;   /* 32-bit per sample */
    const uint32_t fade_n      = sample_rate / 100;     /* ~10 ms fade-in/out per note */

    int32_t *buf = (int32_t *)heap_caps_malloc(I2S_BUFFER_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGE(THIS_MODULE_NAME, "Failed to allocate audio buffer (%d bytes)", I2S_BUFFER_SIZE);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(THIS_MODULE_NAME, "Playing looping melody @ %lu Hz ...", (unsigned long)sample_rate);

    const size_t melody_len = sizeof(audio_melody) / sizeof(audio_melody[0]);

    while (1) {
        for (size_t k = 0; k < melody_len; k++) {
            uint32_t freq  = audio_melody[k].freq_hz;
            uint32_t total = (uint32_t)audio_melody[k].dur_ms * sample_rate / 1000u;
            uint32_t n     = 0;

            while (n < total) {
                uint32_t cnt = (total - n < samples) ? (total - n) : samples;

                /* Sine wave with a short fade envelope to avoid clicks at note edges. */
                for (uint32_t i = 0; i < cnt; i++, n++) {
                    float phase = 2.0f * AUDIO_PI * (float)freq * (float)n / (float)sample_rate;
                    float env   = 1.0f;
                    if (n < fade_n) {
                        env = (float)n / (float)fade_n;
                    } else if (total - n < fade_n) {
                        env = (float)(total - n) / (float)fade_n;
                    }
                    int16_t s = (int16_t)(AUDIO_TONE_AMP * 32767.0f * env * sinf(phase));
                    buf[i]    = ((int32_t)s) << 16;
                }

                size_t written = 0;
                esp_err_t err = i2s_channel_write(audio_control.i2s_speaker_handle, buf,
                                                  cnt * 4, &written, portMAX_DELAY);
                if (err != ESP_OK) {
                    ESP_LOGW(THIS_MODULE_NAME, "i2s_channel_write failed: %s", esp_err_to_name(err));
                }
            }
        }
    }
}