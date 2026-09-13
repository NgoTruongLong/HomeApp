#ifndef WIFI_CONTROL_H
#define WIFI_CONTROL_H

#include "debug.h"
#include "stdbool.h"
#include <stdint.h>
#include <stddef.h>

/******************************* DEFINITIONS *******************************/
/* Số mạng tối đa lưu trong 1 lần quét. */
#define WIFI_MAX_AP (16)

/******************************* DATA TYPES *******************************/
/* Trạng thái hoạt động của module WiFi (STA). */
typedef enum {
    WIFI_STATE_OFF = 0,        /* chưa khởi tạo / chưa start */
    WIFI_STATE_IDLE,           /* STA sẵn sàng, chưa kết nối */
    WIFI_STATE_CONNECTING,     /* đang kết nối (do boot hoặc người dùng) */
    WIFI_STATE_RECONNECTING,   /* đang tự thử kết nối lại sau khi rớt mạng */
    WIFI_STATE_CONNECTED,      /* đã kết nối + có IP */
} wifi_state_t;

/* Kết quả 1 AP sau khi quét (bản sao nhẹ, không lộ esp_wifi types). */
typedef struct {
    char    ssid[33];
    uint8_t bssid[6];
    int8_t  rssi;
    uint8_t channel;
    uint8_t authmode;          /* 0 == WIFI_AUTH_OPEN (mạng không mật khẩu) */
} wifi_ap_info_t;

/******************************* FUNCTIONS PROTOTYPE *******************************/
/* Khởi tạo STA; nếu có profile (NVS hoặc Kconfig) sẽ tự kết nối không block. */
APP_RESULT wifi_control_init();
APP_RESULT wifi_control_wait_connected(uint32_t timeout_ms);
bool wifi_control_is_connected();
wifi_state_t wifi_control_get_state();
const char *wifi_control_get_connected_ssid();
APP_RESULT wifi_control_get_ip(char *buf, size_t len);

/* Chọn 1 mạng & kết nối (async, không block UI). Lưu profile vào NVS. */
APP_RESULT wifi_control_connect(const char *ssid, const char *password);
/* Ngắt kết nối hiện tại nhưng giữ profile để lần sau boot tự kết nối. */
void wifi_control_disconnect(void);
/* Ngắt kết nối + xoá profile khỏi NVS. */
void wifi_control_forget(void);
/* Dừng việc tự kết nối lại (gọi trước khi người dùng quét/chọn mạng khác). */
void wifi_control_abort_auto_reconnect(void);

/* Quét mạng (async). Sau khi scan_is_done()==true thì lấy kết quả. */
APP_RESULT wifi_control_scan_start(void);
bool wifi_control_scan_is_done(void);
/* Huỷ 1 lần quét đang chạy (ví dụ khi chờ quá lâu). */
APP_RESULT wifi_control_scan_stop(void);
/* Copy kết quả quét vào out[], trả về số mạng tìm được (0 nếu chưa xong/lỗi). */
uint16_t wifi_control_scan_get_results(wifi_ap_info_t *out, uint16_t max);

/******************************* VARIABLES *******************************/

#endif /* WIFI_CONTROL_H */
