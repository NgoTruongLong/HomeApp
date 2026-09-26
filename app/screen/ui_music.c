/*
 * ui_music.c
 *
 * Music player (SD) - Phase 3:
 *  - Tự quét các bài hát .wav trên thẻ SD mỗi khi mở trang.
 *  - Danh sách bài hát ở giữa; chạm 1 bài để phát (bài đang phát được tô sáng).
 *  - Panel Now-Playing phía dưới: tên bài, thời gian đã phát / tổng thời lượng,
 *    thanh tiến trình (kéo/tap để tua) và các nút Prev / Play-Pause / Stop / Next.
 *  - Toàn bộ trạng thái phát nhạc lấy từ audio_control (audio_get_status).
 */
#include "ui_music.h"
#include "ui_common.h"
#include "lv_font_vn.h"
#include "micro_sdcard_control.h"
#include "audio_control.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define THIS_MODULE_NAME  "ui_music"

LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_16);
LV_FONT_DECLARE(lv_font_montserrat_20);

#define UI_TICK_MS        (100)
#define MUSIC_ROW_H       (40)
#define LIST_PAD          (6)

#define PLAYER_H          (104)    /* chiều cao panel Now-Playing phía dưới */
#define TOOL_H            (34)     /* hàng trên: số bài + nút Scan */

#define COL_TITLE   lv_color_hex(0xFFFFFF)
#define COL_LABEL   lv_color_hex(0x9AA0A6)
#define COL_ACCENT  lv_color_hex(0x38B6FF)
#define COL_GREEN   lv_color_hex(0x4CAF50)
#define COL_RED     lv_color_hex(0xEF5350)
#define COL_BTN     lv_color_hex(0x2A3752)
#define COL_BTN_RED lv_color_hex(0x9A2A2A)
#define COL_ROW     lv_color_hex(0x232C3B)
#define COL_ROW_ACT lv_color_hex(0x1E3A5F)
#define COL_ROW_PRS lv_color_hex(0x16234F)
#define COL_PANEL   lv_color_hex(0x1C2532)
#define COL_BG_LIST lv_color_hex(0x12171E)
#define COL_BORDER  lv_color_hex(0x3A414C)

typedef struct {
    lv_obj_t *page;
    lv_obj_t *content;

    /* thanh trên */
    lv_obj_t *count_lbl;

    /* danh sách + overlay */
    lv_obj_t *list;
    lv_obj_t *loading;
    lv_obj_t *empty;
    lv_obj_t *rows[PLAYLIST_MAX_SONGS];

    /* panel Now-Playing */
    lv_obj_t *title_lbl;
    lv_obj_t *bar;          /* lv_slider 0..1000 */
    lv_obj_t *time_cur_lbl;
    lv_obj_t *time_tot_lbl;
    lv_obj_t *btn_play_lbl; /* label con của nút Play/Pause để đổi icon */

    playlist_t playlist;
    uint16_t   song_cnt;
    lv_coord_t list_inner_w;   /* chiều rộng nội dung của list */

    bool       page_was_visible;
    bool       scanning;       /* đang hiện overlay & sẽ quét ở tick */
    bool       bar_dragging;
    bool       bar_enabled;
    int        cur_idx;        /* bài đang phát trong danh sách (-1: không) */

    /* cache trạng thái audio để chỉ vẽ lại khi có thay đổi */
    audio_state_t last_state;
    char          last_path[AUDIO_SONG_PATH_MAX];
} music_ui_t;

static music_ui_t g;

/******************************* FUNCTIONS PROTOTYPE *******************************/
static void music_tick(lv_timer_t *timer);
static void music_start_scan(void);
static void music_do_scan(void);
static void music_refresh_player(void);
static void music_row_click_cb(lv_event_t *e);
static void music_btn_click_cb(lv_event_t *e);
static void music_bar_event_cb(lv_event_t *e);

/* ---------- view helpers ---------- */
static void music_show_loading(void)
{
    lv_obj_clear_flag(g.loading, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g.empty, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g.list, LV_OBJ_FLAG_HIDDEN);
}

static void music_show_empty(void)
{
    lv_obj_add_flag(g.loading, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g.empty, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g.list, LV_OBJ_FLAG_HIDDEN);
}

static void music_show_list(void)
{
    lv_obj_add_flag(g.loading, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g.empty, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g.list, LV_OBJ_FLAG_HIDDEN);
}

/* Tên hiển thị: bỏ đường dẫn thư mục & phần mở rộng ".wav". */
static void music_title_from_path(const char *path, char *out, size_t out_sz)
{
    const char *base = strrchr(path, '/');
    size_t n;

    base = (base != NULL) ? base + 1 : path;
    n = strlen(base);
    if (n >= 4 && strncasecmp(base + n - 4, ".wav", 4) == 0) {
        n -= 4;
    }
    if (n >= out_sz) {
        n = out_sz - 1;
    }
    memcpy(out, base, n);
    out[n] = '\0';
}

static void music_fmt_time(uint32_t ms, char *buf, size_t len)
{
    uint32_t sec = ms / 1000;
    snprintf(buf, len, "%lu:%02lu", (unsigned long)(sec / 60), (unsigned long)(sec % 60));
}

/* Chẩn đoán (tạm thời): in heap hiện tại + heap integrity + stack còn trống
 * của task đang chạy (task LVGL) để debug crash khi quét SD. */
static void music_log_heap_stack(const char *tag)
{
    bool ok = heap_caps_check_integrity_all(true);
    ESP_LOGI(THIS_MODULE_NAME,
             "[%s] heap free=%u min=%u | lvgl_stack_hw=%u words | heap_integrity=%s",
             tag,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             ok ? "OK" : "CORRUPT");
}

/* ---------- tạo 1 dòng bài hát ---------- */
static void music_set_row_active(uint16_t idx, bool active)
{
    lv_obj_t *icon;

    if (idx >= g.song_cnt || g.rows[idx] == NULL) {
        return;
    }
    lv_obj_set_style_bg_color(g.rows[idx], active ? COL_ROW_ACT : COL_ROW, 0);
    icon = lv_obj_get_child(g.rows[idx], 0);
    if (icon == NULL) {
        return;
    }
    ui_set_text(icon, active ? LV_SYMBOL_PLAY : LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(icon, active ? COL_GREEN : COL_ACCENT, 0);
}

static void music_row_create(uint16_t idx)
{
    const char *path = g.playlist.songs[idx];
    char name[SONG_NAME_MAX_LEN];
    lv_obj_t *row, *icon, *lbl;

    music_title_from_path(path, name, sizeof(name));

    row = lv_obj_create(g.list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), MUSIC_ROW_H);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_bg_color(row, COL_ROW, 0);
    lv_obj_set_style_bg_color(row, COL_ROW_PRS, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, music_row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

    icon = ui_make_label(row, LV_SYMBOL_AUDIO, &lv_font_montserrat_20, COL_ACCENT, 0, 0);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 12, 0);

    /* Tên file bài hát có thể có dấu tiếng Việt -> dùng font VN. */
    lbl = ui_make_label(row, name, &lv_font_vn_16, COL_TITLE, 0, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl, g.list_inner_w - 42 - 12);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 42, 0);

    g.rows[idx] = row;
}

static void music_build_list(void)
{
    uint16_t i;

    lv_obj_clean(g.list);
    for (i = 0; i < PLAYLIST_MAX_SONGS; i++) {
        g.rows[i] = NULL;
    }
    for (i = 0; i < g.song_cnt; i++) {
        music_row_create(i);
    }
    lv_obj_update_layout(g.list);
}

static void music_apply_highlight(audio_state_t state)
{
    uint16_t i;
    bool any = (state != AUDIO_STATE_IDLE);

    for (i = 0; i < g.song_cnt; i++) {
        music_set_row_active(i, any && ((int)i == g.cur_idx));
    }
}

/* ---------- quét thẻ SD ---------- */
static void music_start_scan(void)
{
    if (g.scanning) {
        return;
    }
    g.scanning = true;
    music_show_loading();
}

/* Quét đồng bộ (danh sách nhỏ nên rất nhanh); gọi trong tick khi đang mở trang. */
static void music_do_scan(void)
{
    audio_status_t st;
    sdcard_t *sd = micro_sdcard_get_control();
    char buf[24];
    uint16_t i;
    APP_RESULT sret;

    music_log_heap_stack("scan-start");
    if (sd == NULL || !sd->is_mounted) {
        return;   /* SD chưa sẵn sàng: giữ overlay, thử lại ở tick sau */
    }

    /* Quét thẳng vào g.playlist (biến static). TUYỆT ĐỐI không dùng biến cục bộ
     * kiểu playlist_t (~12.8KB) vì sẽ tràn stack của task LVGL. */
    music_log_heap_stack("scan-before-list");
    sret = micro_sdcard_list_music_files(MOUNT_POINT, &g.playlist);
    music_log_heap_stack("scan-after-list");
    if (sret != APP_OK) {
        return;   /* thử lại ở tick sau (giữ overlay) */
    }

    g.song_cnt = g.playlist.count;
    g.scanning = false;

    snprintf(buf, sizeof(buf), "%u song%s", (unsigned)g.song_cnt,
             (g.song_cnt == 1) ? "" : "s");
    ui_set_text(g.count_lbl, buf);

    /* đồng bộ lại bài đang phát với danh sách mới */
    audio_get_status(&st);
    g.cur_idx = -1;
    if (st.state != AUDIO_STATE_IDLE && st.song_path[0]) {
        for (i = 0; i < g.song_cnt; i++) {
            if (strcmp(g.playlist.songs[i], st.song_path) == 0) {
                g.cur_idx = (int)i;
                break;
            }
        }
    }

    if (g.song_cnt == 0) {
        music_show_empty();
    } else {
        music_build_list();
        music_apply_highlight(st.state);
        music_show_list();
    }
    music_refresh_player();
}

/* ---------- cập nhật panel Now-Playing ---------- */
static void music_refresh_player(void)
{
    audio_status_t st;
    bool has_song;
    char tmp[SONG_NAME_MAX_LEN];
    uint32_t pct = 0;

    audio_get_status(&st);
    /* STREAMING = giọng AI đang phát -> không coi là đang phát nhạc. */
    has_song = (st.state == AUDIO_STATE_PLAYING || st.state == AUDIO_STATE_PAUSED);

    /* tên bài đang phát */
    if (has_song && st.song_path[0]) {
        music_title_from_path(st.song_path, tmp, sizeof(tmp));
        ui_set_text(g.title_lbl, tmp);
    } else {
        ui_set_text(g.title_lbl, "Select a song from the list");
    }

    /* thời gian đã phát / tổng thời lượng */
    if (has_song) {
        music_fmt_time(st.elapsed_ms, tmp, sizeof(tmp));
        ui_set_text(g.time_cur_lbl, tmp);
        music_fmt_time(st.duration_ms, tmp, sizeof(tmp));
        ui_set_text(g.time_tot_lbl, tmp);
    } else {
        ui_set_text(g.time_cur_lbl, "0:00");
        ui_set_text(g.time_tot_lbl, "0:00");
    }

    /* bật/tắt thanh trượt theo việc có bài hay không */
    if (has_song != g.bar_enabled) {
        g.bar_enabled = has_song;
        if (has_song) {
            lv_obj_clear_state(g.bar, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(g.bar, LV_STATE_DISABLED);
            lv_slider_set_value(g.bar, 0, LV_ANIM_OFF);
        }
    }

    /* thanh tiến trình (không ghi đè lúc người dùng đang kéo) */
    if (has_song && st.duration_ms > 0 && !g.bar_dragging) {
        pct = (uint32_t)(((uint64_t)st.elapsed_ms * 1000ULL) / st.duration_ms);
        if (pct > 1000) {
            pct = 1000;
        }
        lv_slider_set_value(g.bar, (int32_t)pct, LV_ANIM_OFF);
    }

    /* trạng thái đổi -> cập nhật icon Play/Pause & tô sáng dòng tương ứng */
    if (st.state != g.last_state || strcmp(st.song_path, g.last_path) != 0) {
        uint16_t i;

        g.last_state = st.state;
        snprintf(g.last_path, sizeof(g.last_path), "%s", st.song_path);

        g.cur_idx = -1;
        if (has_song && st.song_path[0]) {
            for (i = 0; i < g.song_cnt; i++) {
                if (strcmp(g.playlist.songs[i], st.song_path) == 0) {
                    g.cur_idx = (int)i;
                    break;
                }
            }
        }
        music_apply_highlight(st.state);

        if (g.btn_play_lbl != NULL) {
            ui_set_text(g.btn_play_lbl,
                        (st.state == AUDIO_STATE_PLAYING) ? LV_SYMBOL_PAUSE
                                                          : LV_SYMBOL_PLAY);
        }
    }
}

/* ---------- chạm vào 1 bài hát ---------- */
static void music_row_click_cb(lv_event_t *e)
{
    uint16_t idx = (uint16_t)(intptr_t)lv_event_get_user_data(e);
    audio_status_t st;

    if (idx >= g.song_cnt) {
        return;
    }
    audio_get_status(&st);
    if (st.state != AUDIO_STATE_IDLE &&
        strcmp(st.song_path, g.playlist.songs[idx]) == 0) {
        /* đang ở bài này: bấm lại để Pause / Resume */
        if (st.state == AUDIO_STATE_PLAYING) {
            audio_pause();
        } else {
            audio_resume();
        }
    } else {
        g.cur_idx = (int)idx;
        audio_request_new_song(g.playlist.songs[idx]);
    }
}

/* ---------- nút Prev / Play-Pause / Stop / Next / Scan ---------- */
static void music_btn_click_cb(lv_event_t *e)
{
    intptr_t act = (intptr_t)lv_event_get_user_data(e);
    audio_status_t st;
    int nxt;

    switch (act) {
    case 0:   /* Prev */
        if (g.song_cnt == 0) break;
        nxt = (g.cur_idx - 1 + (int)g.song_cnt) % (int)g.song_cnt;
        g.cur_idx = nxt;
        audio_request_new_song(g.playlist.songs[nxt]);
        break;
    case 1:   /* Play / Pause */
        audio_get_status(&st);
        if (st.state == AUDIO_STATE_PLAYING) {
            audio_pause();
        } else if (st.state == AUDIO_STATE_PAUSED) {
            audio_resume();
        } else if (g.song_cnt > 0) {
            nxt = (g.cur_idx >= 0 && g.cur_idx < (int)g.song_cnt) ? g.cur_idx : 0;
            g.cur_idx = nxt;
            audio_request_new_song(g.playlist.songs[nxt]);
        }
        break;
    case 2:   /* Stop */
        audio_stop();
        g.cur_idx = -1;
        break;
    case 3:   /* Next */
        if (g.song_cnt == 0) break;
        nxt = (g.cur_idx + 1 + (int)g.song_cnt) % (int)g.song_cnt;
        g.cur_idx = nxt;
        audio_request_new_song(g.playlist.songs[nxt]);
        break;
    case 4:   /* Scan */
        music_start_scan();
        break;
    default:
        break;
    }
}

/* ---------- thanh tiến trình (kéo / tap để tua) ---------- */
static void music_bar_seek_to_point(lv_event_t *e)
{
    lv_obj_t *bar = lv_event_get_target(e);
    lv_indev_t *indev;
    lv_point_t p;
    lv_area_t a;
    int32_t v;
    audio_status_t st;

    indev = lv_indev_get_act();
    if (indev == NULL) {
        return;
    }
    lv_indev_get_point(indev, &p);
    lv_obj_get_coords(bar, &a);
    if (a.x2 <= a.x1) {
        return;
    }
    v = (int32_t)(((int64_t)(p.x - a.x1) * 1000L) / (a.x2 - a.x1));
    if (v < 0) {
        v = 0;
    }
    if (v > 1000) {
        v = 1000;
    }
    lv_slider_set_value(bar, v, LV_ANIM_OFF);

    audio_get_status(&st);
    if (st.state != AUDIO_STATE_IDLE && st.duration_ms > 0) {
        uint32_t ms = (uint32_t)(((uint64_t)v * st.duration_ms) / 1000ULL);
        audio_seek(ms);
    }
}

static void music_bar_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        g.bar_dragging = true;
    } else if (code == LV_EVENT_RELEASED) {
        audio_status_t st;

        g.bar_dragging = false;
        audio_get_status(&st);
        if (st.state != AUDIO_STATE_IDLE && st.duration_ms > 0) {
            int32_t v = lv_slider_get_value(g.bar);
            uint32_t ms = (uint32_t)(((uint64_t)v * st.duration_ms) / 1000ULL);
            audio_seek(ms);
        }
    } else if (code == LV_EVENT_CLICKED) {
        /* chạm vào thanh -> nhảy tới vị trí đó */
        music_bar_seek_to_point(e);
    }
}

/* ---------- lv_timer định kỳ ---------- */
static void music_tick(lv_timer_t *timer)
{
    (void)timer;
    bool vis;

    if (g.page == NULL || g.content == NULL) {
        return;
    }
    vis = !lv_obj_has_flag(g.page, LV_OBJ_FLAG_HIDDEN);
    if (vis && !g.page_was_visible) {
        /* vừa mở trang Music -> tự quét */
        g.page_was_visible = true;
        music_start_scan();
    } else if (!vis && g.page_was_visible) {
        g.page_was_visible = false;
        return;
    }
    if (!vis) {
        return;
    }

    if (g.scanning) {
        music_do_scan();
    }
    music_refresh_player();
}

/* ---------- tạo màn hình ---------- */
static lv_obj_t *music_icon_button(lv_obj_t *parent, const char *sym, lv_coord_t w,
                                   lv_coord_t h, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *btn = ui_make_button(parent, sym, w, h, cb, ud);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    return btn;
}

void ui_music_create(lv_obj_t *content)
{
    lv_coord_t W, H, panel_y, panel_w, list_y, list_h;
    lv_obj_t *panel, *btn, *btn_scan;
    uint16_t i;

    memset(&g, 0, sizeof(g));
    g.content = content;
    g.page    = lv_obj_get_parent(content);
    g.cur_idx = -1;

    /* Lưu ý: lúc dựng UI coords chưa được tính nên dùng kích thước an toàn. */
    W = lv_obj_get_width(content);
    H = lv_obj_get_height(content);
    if (W <= 20 || W > 600) {
        W = 480;   /* LCD_H_RES */
    }
    if (H <= 20 || H > 340) {
        H = 282;   /* LCD_V_RES - HEADER_H - 2 */
    }
    g.list_inner_w = (W - 16) - 2 * LIST_PAD;
    panel_w = W - 16;

    /* ---- thanh trên: số bài + nút Scan ---- */
    g.count_lbl = ui_make_label(content, "0 songs", &lv_font_montserrat_16, COL_LABEL, 12, 8);

    btn_scan = ui_make_button(content, "Scan", 72, 30, music_btn_click_cb,
                              (void *)(intptr_t)4);
    lv_obj_set_pos(btn_scan, W - 12 - 72, 4);
    lv_obj_set_style_bg_color(btn_scan, COL_ACCENT, 0);

    /* ---- vùng danh sách ---- */
    list_y  = TOOL_H + 2;
    panel_y = H - 8 - PLAYER_H;
    list_h  = panel_y - list_y - 4;

    g.list = lv_obj_create(content);
    lv_obj_remove_style_all(g.list);
    lv_obj_set_pos(g.list, 8, list_y);
    lv_obj_set_size(g.list, W - 16, list_h);
    lv_obj_set_style_radius(g.list, 8, 0);
    lv_obj_set_style_bg_color(g.list, COL_BG_LIST, 0);
    lv_obj_set_style_bg_opa(g.list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g.list, 1, 0);
    lv_obj_set_style_border_color(g.list, COL_BORDER, 0);
    lv_obj_set_style_pad_all(g.list, LIST_PAD, 0);
    lv_obj_set_style_pad_row(g.list, 4, 0);
    lv_obj_set_style_pad_column(g.list, 0, 0);
    lv_obj_set_layout(g.list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(g.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g.list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(g.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g.list, LV_SCROLLBAR_MODE_AUTO);

    /* ---- overlay: đang quét ---- */
    g.loading = lv_obj_create(content);
    lv_obj_remove_style_all(g.loading);
    lv_obj_set_pos(g.loading, 8, list_y);
    lv_obj_set_size(g.loading, W - 16, list_h);
    lv_obj_set_style_radius(g.loading, 8, 0);
    lv_obj_set_style_bg_color(g.loading, COL_BG_LIST, 0);
    lv_obj_set_style_bg_opa(g.loading, LV_OPA_90, 0);
    lv_obj_set_style_border_width(g.loading, 1, 0);
    lv_obj_set_style_border_color(g.loading, COL_BORDER, 0);

    lv_obj_t *spin = lv_spinner_create(g.loading, 900, 60);
    lv_obj_set_size(spin, 34, 34);
    lv_obj_align(spin, LV_ALIGN_CENTER, 0, -14);
    lv_obj_set_style_arc_color(spin, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spin, 3, LV_PART_INDICATOR);

    lv_obj_t *scan_lbl = ui_make_label(g.loading, "Scanning songs...", &lv_font_montserrat_14,
                                       COL_LABEL, 0, 0);
    lv_obj_set_width(scan_lbl, lv_pct(100));
    lv_obj_set_style_text_align(scan_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(scan_lbl, LV_ALIGN_CENTER, 0, 26);

    /* ---- overlay: không có bài hát ---- */
    g.empty = lv_obj_create(content);
    lv_obj_remove_style_all(g.empty);
    lv_obj_set_pos(g.empty, 8, list_y);
    lv_obj_set_size(g.empty, W - 16, list_h);
    lv_obj_set_style_radius(g.empty, 8, 0);
    lv_obj_set_style_bg_color(g.empty, COL_BG_LIST, 0);
    lv_obj_set_style_bg_opa(g.empty, LV_OPA_90, 0);
    lv_obj_set_style_border_width(g.empty, 1, 0);
    lv_obj_set_style_border_color(g.empty, COL_BORDER, 0);

    lv_obj_t *empty_lbl = ui_make_label(g.empty, "No .wav songs on SD card\nTap Scan to retry",
                                        &lv_font_montserrat_16, COL_LABEL, 0, 0);
    lv_obj_set_width(empty_lbl, lv_pct(100));
    lv_obj_set_style_text_align(empty_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(empty_lbl, LV_ALIGN_CENTER, 0, 0);

    /* ---- panel Now-Playing ---- */
    panel = lv_obj_create(content);
    lv_obj_remove_style_all(panel);
    lv_obj_set_pos(panel, 8, panel_y);
    lv_obj_set_size(panel, panel_w, PLAYER_H);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_bg_color(panel, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, COL_BORDER, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);

    g.title_lbl = ui_make_label(panel, "Select a song from the list", &lv_font_vn_20,
                                COL_TITLE, 12, 6);
    lv_label_set_long_mode(g.title_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(g.title_lbl, panel_w - 24);

    g.bar = lv_slider_create(panel);
    lv_obj_set_size(g.bar, panel_w - 24, 16);
    lv_obj_set_pos(g.bar, 12, 32);
    lv_obj_add_event_cb(g.bar, music_bar_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_set_style_bg_color(g.bar, COL_BTN, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g.bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(g.bar, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g.bar, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g.bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(g.bar, COL_TITLE, LV_PART_KNOB);
    lv_obj_set_style_radius(g.bar, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_add_state(g.bar, LV_STATE_DISABLED);
    g.bar_enabled = false;

    g.time_cur_lbl = ui_make_label(panel, "0:00", &lv_font_montserrat_14, COL_LABEL, 12, 54);
    g.time_tot_lbl = ui_make_label(panel, "0:00", &lv_font_montserrat_14, COL_LABEL, 0, 0);
    lv_obj_align(g.time_tot_lbl, LV_ALIGN_TOP_RIGHT, -12, 54);

    /* nhóm nút điều khiển (căn giữa panel) */
    btn = music_icon_button(panel, LV_SYMBOL_PREV, 44, 28, music_btn_click_cb,
                            (void *)(intptr_t)0);
    lv_obj_set_pos(btn, panel_w / 2 - 102, 72);
    lv_obj_set_style_bg_color(btn, COL_BTN, 0);

    btn = music_icon_button(panel, LV_SYMBOL_PLAY, 54, 28, music_btn_click_cb,
                            (void *)(intptr_t)1);
    lv_obj_set_pos(btn, panel_w / 2 - 52, 72);
    lv_obj_set_style_bg_color(btn, COL_ACCENT, 0);
    g.btn_play_lbl = lv_obj_get_child(btn, 0);

    btn = music_icon_button(panel, LV_SYMBOL_STOP, 44, 28, music_btn_click_cb,
                            (void *)(intptr_t)2);
    lv_obj_set_pos(btn, panel_w / 2 + 8, 72);
    lv_obj_set_style_bg_color(btn, COL_BTN_RED, 0);

    btn = music_icon_button(panel, LV_SYMBOL_NEXT, 44, 28, music_btn_click_cb,
                            (void *)(intptr_t)3);
    lv_obj_set_pos(btn, panel_w / 2 + 58, 72);
    lv_obj_set_style_bg_color(btn, COL_BTN, 0);

    for (i = 0; i < PLAYLIST_MAX_SONGS; i++) {
        g.rows[i] = NULL;
    }

    music_show_loading();

    lv_timer_create(music_tick, UI_TICK_MS, NULL);
}