#include "mic_inmp441.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define THIS_MODULE_NAME "mic_inmp441"

/* Buffer thô 32-bit (INMP441 trả 24-bit MSB-aligned trong slot 32-bit).
 * Nho (512 frame = 2KB) de tiet kiem RAM noi bo; mic_inmp441_read() doc lap
 * tung chunk nho nay cho den khi du so frame caller yeu cau. */
#define MIC_RAW_FRAMES   (512)

static i2s_chan_handle_t s_rx;
static int32_t           s_raw[MIC_RAW_FRAMES];

APP_RESULT mic_inmp441_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_I2S_PORT, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx);
    ASSERT_CRITICAL(err == ESP_OK, err, APP_ERROR);

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_I2S_BCLK_PIN,
            .ws   = MIC_I2S_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din  = MIC_I2S_DIN_PIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    /* INMP441 L/R nối GND -> dữ liệu nằm ở kênh LEFT. */
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    err = i2s_channel_init_std_mode(s_rx, &std_cfg);
    ASSERT_CRITICAL(err == ESP_OK, err, APP_ERROR);
    err = i2s_channel_enable(s_rx);
    ASSERT_CRITICAL(err == ESP_OK, err, APP_ERROR);

    ESP_LOGI(THIS_MODULE_NAME, "INMP441 ready (I2S%d) BCLK=%d WS=%d DIN=%d @%d Hz",
             MIC_I2S_PORT, MIC_I2S_BCLK_PIN, MIC_I2S_WS_PIN, MIC_I2S_DIN_PIN,
             MIC_SAMPLE_RATE);
    return APP_OK;
}

size_t mic_inmp441_read(int16_t *out, size_t max_frames, uint32_t timeout_ms)
{
    size_t got = 0;

    if (s_rx == NULL || out == NULL || max_frames == 0) {
        return 0;
    }
    /* Doc theo tung chunk nho: buffer tinh KHONG phu thuoc kich thuoc frame ma
     * caller yeu cau (vd 60ms @24kHz = 1440 frame > MIC_RAW_FRAMES), va luon
     * tra ve DU so frame -> tranh truong hop mic im lang khong ro nguyen nhan. */
    while (got < max_frames) {
        size_t want = max_frames - got;
        size_t bytes_read = 0;
        size_t frames;

        if (want > MIC_RAW_FRAMES) {
            want = MIC_RAW_FRAMES;
        }
        if (i2s_channel_read(s_rx, s_raw, want * sizeof(int32_t), &bytes_read,
                             pdMS_TO_TICKS(timeout_ms)) != ESP_OK) {
            break;
        }
        frames = bytes_read / sizeof(int32_t);
        for (size_t i = 0; i < frames; i++) {
            out[got + i] = (int16_t)(s_raw[i] >> 16);
        }
        got += frames;
        if (frames < want) {
            break;          /* lan doc nay khong con du du lieu */
        }
    }
    return got;
}
