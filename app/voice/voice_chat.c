#include "voice_chat.h"
#include "xiaozhi_ws.h"
#include "mic_inmp441.h"
#include "audio_control.h"
#include "opus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"
#include "freertos/idf_additions.h"   /* xTaskCreateWithCaps: stack trong PSRAM */
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

/******************************* DEFINITIONS *******************************/
#define THIS_MODULE_NAME   "voice_chat"

#define VOICE_TASK_STACK   (8192)     /* voice_task chay ca OTA HTTPS/TLS + cJSON */
/* Opus encoder/decoder cap phat buffer lam viec bang alloca (SILK/CELT dung
 * VARDECL/ALLOC) -> can RAT nhieu stack: 16KB KHONG du, da bi
 * "Stack canary watchpoint triggered (mic_task)" ngay frame dau tien.
 * Ca 2 task nay dung stack PSRAM (PSRAM ~8MB) nen cap rong rai; xem log
 * "stack hwm=... bytes" de biet muc su dung thuc te roi thu nho sau. */
#define MIC_TASK_STACK     (65536)
#define RX_TASK_STACK      (32768)
#define RX_RB_BYTES        (8 * 1024)
#define VOICE_TASK_PRIOR   (4)
#define MIC_TASK_PRIOR     (4)
#define RX_TASK_PRIOR      (5)

#define VOICE_OPUS_MAX     (1000)     /* max opus packet cho 60ms @16k */
#define VOICE_TEXT_MAX     (128)

/******************************* VARIABLES *******************************/
/* Opus */
static OpusEncoder *s_encoder;
static OpusDecoder *s_decoder;

/* Trạng thái chia sẻ với UI */
static voice_state_t   s_state = VOICE_STATE_IDLE;
static char            s_last_text[VOICE_TEXT_MAX];
static SemaphoreHandle_t s_mtx;

/* Cờ điều khiển (giữa UI / voice_task / callbacks) */
static volatile bool s_enabled;      /* người dùng đã bật */
static volatile bool s_ws_up;        /* đã gọi xz_connect */
static volatile bool s_mic_muted;    /* tạm khoá mic khi AI đang nói */
static volatile bool s_need_listen_start;
static volatile bool s_stream_open;
static bool          s_available;

/* Buffer audio (static để không tốn stack) */
static int16_t s_pcm[VOICE_FRAME_SAMPLES];
static int16_t s_decoded[VOICE_FRAME_SAMPLES];
static uint8_t s_opus[VOICE_OPUS_MAX];

/* Hàng đợi Opus nhận từ server: ws_task -> rx_task (không decode trong ws_task).
 * Mỗi bản ghi = [uint16 len little-endian][opus payload]. */
static RingbufHandle_t s_rx_rb;
static uint8_t         s_rx_rec[VOICE_OPUS_MAX + 2];

/******************************* FUNCTIONS PROTOTYPE *******************************/
static void voice_task(void *arg);
static void mic_task(void *arg);
static void rx_task(void *arg);
static void on_xz_audio(const uint8_t *opus, size_t len);
static void on_xz_msg(const char *type, const char *text, const char *state);
static void voice_set_state(voice_state_t st);
static void voice_set_text(const char *text);

/******************************* FUNCTIONS IMPLEMENTATION *******************************/
static void voice_set_state(voice_state_t st)
{
    if (s_mtx == NULL) {
        return;   /* chưa init */
    }
    if (xSemaphoreTake(s_mtx, portMAX_DELAY) == pdTRUE) {
        s_state = st;
        xSemaphoreGive(s_mtx);
    }
    ESP_LOGI(THIS_MODULE_NAME, "state -> %d", (int)st);
}

static void voice_set_text(const char *text)
{
    if (text == NULL || s_mtx == NULL) {
        return;
    }
    if (xSemaphoreTake(s_mtx, portMAX_DELAY) != pdTRUE) {
        return;
    }
    snprintf(s_last_text, sizeof(s_last_text), "%s", text);
    xSemaphoreGive(s_mtx);
}

voice_state_t voice_chat_get_state(void)
{
    voice_state_t st = VOICE_STATE_IDLE;

    if (s_mtx == NULL) {
        return VOICE_STATE_IDLE;   /* chưa init */
    }
    if (xSemaphoreTake(s_mtx, portMAX_DELAY) == pdTRUE) {
        st = s_state;
        xSemaphoreGive(s_mtx);
    }
    return st;
}

const char *voice_chat_state_text(void)
{
    switch (voice_chat_get_state()) {
    case VOICE_STATE_CONNECTING: return "Connecting...";
    case VOICE_STATE_LISTENING:  return "Listening...";
    case VOICE_STATE_THINKING:   return "Thinking...";
    case VOICE_STATE_SPEAKING:   return "Speaking...";
    case VOICE_STATE_ERROR:      return "Error (check WiFi / activation)";
    default:                     return "Tap START to talk";
    }
}

const char *voice_chat_get_last_text(void)
{
    if (s_mtx == NULL) {
        return "";   /* chưa init */
    }
    return s_last_text;
}

bool voice_chat_is_active(void)
{
    return (s_mtx != NULL) && s_enabled;
}

/******************************* XIAOZHI CALLBACKS *******************************/
static void on_xz_audio(const uint8_t *opus, size_t len)
{
    if (opus == NULL || len == 0 || len > VOICE_OPUS_MAX || s_rx_rb == NULL) {
        return;
    }
    /* Chay TRONG websocket task -> NOI DUNG DUY NHAT: copy vao ring buffer
     * (PSRAM) roi tra ve ngay. Tuyet doi KHONG opus_decode o day: decode can
     * >8KB stack, se lam tran stack/het RAM cua websocket task. */
    s_rx_rec[0] = (uint8_t)(len & 0xFF);
    s_rx_rec[1] = (uint8_t)((len >> 8) & 0xFF);
    memcpy(s_rx_rec + 2, opus, len);

    if (xRingbufferSend(s_rx_rb, s_rx_rec, len + 2, pdMS_TO_TICKS(20)) != pdTRUE) {
        ESP_LOGW(THIS_MODULE_NAME, "rx ring full, drop %u B", (unsigned)len);
    }
}

static void on_xz_msg(const char *type, const char *text, const char *state)
{
    if (type == NULL) {
        return;
    }
    if (strcmp(type, "hello") == 0) {
        s_need_listen_start = true;      /* để voice_task gửi listen start */
        voice_set_state(VOICE_STATE_LISTENING);
    } else if (strcmp(type, "stt") == 0) {
        if (text) voice_set_text(text);
        voice_set_state(VOICE_STATE_THINKING);
    } else if (strcmp(type, "llm") == 0) {
        if (text) voice_set_text(text);
    } else if (strcmp(type, "tts") == 0) {
        if (state && strcmp(state, "start") == 0) {
            s_mic_muted = true;
            if (text) voice_set_text(text);
            voice_set_state(VOICE_STATE_SPEAKING);
        } else if (state && strcmp(state, "stop") == 0) {
            /* Het 1 luot -> PHAI mo luot nghe MOI. Truoc day chi gui
             * "listen start" 1 lan duy nhat sau "hello", nen server chi STT
             * duoc cau dau tien: cac cau sau mic van gui audio (sent tang deu)
             * nhung server khong o trang thai nghe -> khong co "stt" moi. */
            s_mic_muted = false;
            s_need_listen_start = true;
            voice_set_state(VOICE_STATE_LISTENING);
        } else if (text) {           /* sentence_start */
            voice_set_text(text);
        }
    } else if (strcmp(type, "goodbye") == 0) {
        s_mic_muted = false;
        voice_set_state(VOICE_STATE_LISTENING);
    }
}

/******************************* TASKS *******************************/
static void mic_task(void *arg)
{
    uint32_t read_ok   = 0;
    uint32_t read_err  = 0;
    uint32_t sent_ok   = 0;
    uint32_t sent_fail = 0;
    uint32_t sent_bytes = 0;
    int      peak      = 0;
    bool     logged_hwm = false;

    (void)arg;
    ESP_LOGI(THIS_MODULE_NAME, "mic task started");

    while (1) {
        if (s_enabled && xz_is_ready() && !s_mic_muted) {
            size_t n = mic_inmp441_read(s_pcm, VOICE_FRAME_SAMPLES, 200);

            if (n != VOICE_FRAME_SAMPLES) {
                read_err++;
                ESP_LOGW(THIS_MODULE_NAME, "mic read short: %u/%u frames",
                         (unsigned)n, (unsigned)VOICE_FRAME_SAMPLES);
            } else if (s_encoder != NULL) {
                int len = opus_encode(s_encoder, s_pcm, VOICE_FRAME_SAMPLES,
                                      s_opus, sizeof(s_opus));
                if (len <= 0) {
                    ESP_LOGW(THIS_MODULE_NAME, "opus_encode failed: %d", len);
                } else {
                    read_ok++;
                    /* Do 1 lan: biet stack con du bao nhieu de chinh MIC_TASK_STACK.
                     * ESP-IDF dinh nghia StackType_t = uint8_t (portSTACK_TYPE)
                     * -> hwm tra ve BYTE, KHONG nhan them 4. */
                    if (!logged_hwm) {
                        logged_hwm = true;
                        ESP_LOGI(THIS_MODULE_NAME,
                                 "mic: opus_encode OK (%d B), stack hwm=%u bytes",
                                 len, (unsigned)uxTaskGetStackHighWaterMark(NULL));
                    }
                    /* Peak = muc tin hieu lon nhat trong frame.
                     * peak ~0 lien tuc => mic im lang / chua thu duoc am thanh.
                     * peak vai tram..vai nghin => co tieng noi. */
                    for (size_t i = 0; i < VOICE_FRAME_SAMPLES; i++) {
                        int a = s_pcm[i] < 0 ? -s_pcm[i] : s_pcm[i];
                        if (a > peak) {
                            peak = a;
                        }
                    }
                    if (xz_is_connected() &&
                        xz_send_audio(s_opus, (size_t)len) == APP_OK) {
                        sent_ok++;
                        sent_bytes += (uint32_t)len;
                    } else {
                        sent_fail++;
                    }
                }
            }

            /* Log moi ~2 s (33 frame x 60ms) de theo doi luong mic -> server. */
            if ((read_ok + read_err + sent_fail) % 33 == 0) {
                ESP_LOGI(THIS_MODULE_NAME,
                         "mic: read_ok=%u err=%u sent=%u pkt(%u B) fail=%u peak=%d hwm=%u",
                         (unsigned)read_ok, (unsigned)read_err, (unsigned)sent_ok,
                         (unsigned)sent_bytes, (unsigned)sent_fail,
                         peak, (unsigned)uxTaskGetStackHighWaterMark(NULL));
                peak = 0;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

/* Decode Opus TTS -> PCM -> loa. Task RIENG vi opus_decode can ~16KB stack va
 * de khong chan vong recv cua websocket task. */
static void rx_task(void *arg)
{
    uint32_t pkts = 0;
    uint32_t errs = 0;

    (void)arg;
    ESP_LOGI(THIS_MODULE_NAME, "rx decode task started (stack o PSRAM)");

    while (1) {
        size_t len = 0;
        uint8_t *block = (uint8_t *)xRingbufferReceive(s_rx_rb, &len, portMAX_DELAY);

        if (block == NULL) {
            continue;
        }
        /* Byte buffer co the gop nhieu ban ghi lien nhau -> tach theo header 2B. */
        size_t off = 0;
        while (off + 2 <= len) {
            size_t plen = (size_t)block[off] | ((size_t)block[off + 1] << 8);
            if (off + 2 + plen > len) {
                break;          /* ban ghi chua day du */
            }
            int n = opus_decode(s_decoder, block + off + 2, (opus_int32)plen,
                                s_decoded, VOICE_FRAME_SAMPLES, 0);
            if (n > 0) {
                pkts++;
                audio_stream_write(s_decoded, (size_t)n);
            } else {
                errs++;
                ESP_LOGW(THIS_MODULE_NAME, "opus_decode failed: %d (len=%u)",
                         n, (unsigned)plen);
            }
            if ((pkts + errs) % 50 == 0) {
                ESP_LOGI(THIS_MODULE_NAME, "rx tts: pkts=%u err=%u hwm=%u",
                         (unsigned)pkts, (unsigned)errs,
                         (unsigned)uxTaskGetStackHighWaterMark(NULL));
            }
            off += 2 + plen;
        }
        vRingbufferReturnItem(s_rx_rb, block);
    }
}

static void voice_task(void *arg)
{
    (void)arg;
    ESP_LOGI(THIS_MODULE_NAME, "voice task started");

    while (1) {
        if (s_enabled) {
            if (!s_ws_up) {
                voice_set_state(VOICE_STATE_CONNECTING);
                if (!s_stream_open) {
                    audio_stream_open(VOICE_SAMPLE_RATE);
                    s_stream_open = true;
                }
                if (xz_connect() == APP_OK) {
                    s_ws_up = true;
                } else {
                    voice_set_text("Connect failed (WiFi / activation?)");
                    voice_set_state(VOICE_STATE_ERROR);
                    s_enabled = false;
                    s_mic_muted = false;
                }
            } else if (s_need_listen_start) {
                s_need_listen_start = false;
                xz_send_listen_start();
            }
        } else {
            if (s_ws_up) {
                xz_send_listen_stop();
                xz_disconnect();
                s_ws_up = false;
            }
            if (s_stream_open) {
                audio_stream_close();
                s_stream_open = false;
            }
            s_mic_muted = false;
            if (voice_chat_get_state() != VOICE_STATE_IDLE) {
                voice_set_state(VOICE_STATE_IDLE);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/******************************* PUBLIC API *******************************/
APP_RESULT voice_chat_init(void)
{
    int err = OPUS_OK;

    s_mtx = xSemaphoreCreateMutex();
    ASSERT_CRITICAL(s_mtx != NULL, APP_ERROR, APP_ERROR);

    s_last_text[0] = '\0';
    s_enabled = false;
    s_ws_up = false;
    s_available = false;

    if (mic_inmp441_init() != APP_OK) {
        ESP_LOGE(THIS_MODULE_NAME, "INMP441 init failed");
        return APP_ERROR;
    }

    s_encoder = opus_encoder_create(VOICE_SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &err);
    if (s_encoder == NULL || err != OPUS_OK) {
        ESP_LOGE(THIS_MODULE_NAME, "opus encoder create failed (%d)", err);
        return APP_ERROR;
    }
    opus_encoder_ctl(s_encoder, OPUS_SET_BITRATE(24000));
    /* Complexity 5 ton ~16ms cho 1 frame 60ms -> mic_task chay cham hon thuc te
     * ~24% (log: 33 frame / ~2.5s thay vi 2.0s), audio bi tre dan va lech nhip
     * so voi thoi gian thuc -> VAD cua server de bi sai. Complexity 3 van tot
     * cho VOIP 24kbps ma kip thoi gian thuc. */
    opus_encoder_ctl(s_encoder, OPUS_SET_COMPLEXITY(3));
    opus_encoder_ctl(s_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

    s_decoder = opus_decoder_create(VOICE_SAMPLE_RATE, 1, &err);
    if (s_decoder == NULL || err != OPUS_OK) {
        ESP_LOGE(THIS_MODULE_NAME, "opus decoder create failed (%d)", err);
        return APP_ERROR;
    }

    if (xz_init() != APP_OK) {
        ESP_LOGW(THIS_MODULE_NAME, "xiaozhi init failed (WiFi chua san sang?)");
        /* vẫn tạo task; lúc bật sẽ thử lại */
    }
    xz_set_callbacks(on_xz_audio, on_xz_msg);

    /* Ring buffer Opus nhan ve dat trong PSRAM (RAM noi bo chi con ~31KB). */
    s_rx_rb = xRingbufferCreateWithCaps(RX_RB_BYTES, RINGBUF_TYPE_BYTEBUF,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ASSERT_CRITICAL(s_rx_rb != NULL, APP_ERROR, APP_ERROR);

    /* RAM noi bo qua chat (chi con ~25KB, largest block ~7.6KB) nen task
     * websocket cua esp_websocket_client (xTaskCreate -> PHAI o RAM noi bo)
     * khong tao duoc. Day stack mic_task (16KB) sang PSRAM de tra lai 1 khoi
     * 16KB lien mach cho RAM noi bo. rx_task cung o PSRAM. */
    if (xTaskCreateWithCaps(mic_task, "mic_task", MIC_TASK_STACK, NULL,
                            MIC_TASK_PRIOR, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(THIS_MODULE_NAME, "cannot create mic task (PSRAM)");
        return APP_ERROR;
    }
    xTaskCreate(voice_task, "voice_task", VOICE_TASK_STACK, NULL, VOICE_TASK_PRIOR, NULL);
    /* rx_task can ~16KB stack cho opus_decode -> cap tu PSRAM. */
    if (xTaskCreateWithCaps(rx_task, "voice_rx", RX_TASK_STACK, NULL,
                            RX_TASK_PRIOR, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(THIS_MODULE_NAME, "cannot create rx task (PSRAM)");
        return APP_ERROR;
    }

    s_available = true;
    ESP_LOGI(THIS_MODULE_NAME, "voice chat ready");
    return APP_OK;
}

APP_RESULT voice_chat_toggle(void)
{
    if (!s_available) {
        ESP_LOGW(THIS_MODULE_NAME, "voice chat not available");
        return APP_ERROR;
    }
    if (!s_enabled) {
        s_enabled = true;                 /* voice_task sẽ lo phần kết nối */
        s_need_listen_start = false;
        voice_set_state(VOICE_STATE_CONNECTING);
        ESP_LOGI(THIS_MODULE_NAME, "chat ON");
    } else {
        s_enabled = false;
        ESP_LOGI(THIS_MODULE_NAME, "chat OFF");
    }
    return APP_OK;
}
