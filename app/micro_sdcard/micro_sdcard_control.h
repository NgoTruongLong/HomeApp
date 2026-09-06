#ifndef SDCARD_CONTROL
#define SDCARD_CONTROL

#include "debug.h"
#include "commons.h"

/******************************* DEFINITIONS *******************************/
#define PIN_NUM_MOSI    (GPIO_NUM_39)
#define PIN_NUM_MISO    (GPIO_NUM_38)
#define PIN_NUM_CLK     (GPIO_NUM_40)
#define PIN_NUM_CS      (GPIO_NUM_41)

#define PLAYLIST_MAX_SONGS 100
#define SONG_NAME_MAX_LEN 128

/******************************* DATA TYPES *******************************/
typedef struct {
    char    songs[PLAYLIST_MAX_SONGS][SONG_NAME_MAX_LEN];  /* path đầy đủ, ví dụ "/sdcard/bai1.wav" */
    uint16_t count;                                        /* số bài tìm được */
} playlist_t;

typedef struct {
    bool is_initialized;
    bool is_mounted;
    playlist_t playlist;
} sdcard_t;

/******************************* FUNCTIONS PROTOTYPE *******************************/
APP_RESULT micro_sdcard_init();
sdcard_t* micro_sdcard_get_control();
APP_RESULT micro_sdcard_write_file(const char *file_path, const void *data, size_t *len, bool append);
APP_RESULT micro_sdcard_read_file(const char *file_path, void *data, size_t *len);
APP_RESULT micro_sdcard_list_music_files(const char* directory_path, playlist_t *playlist);

/******************************* VARIABLES *******************************/

/******************************* FUNCTIONS IMPLEMENTATION *******************************/

#endif /* SDCARD_CONTROL */