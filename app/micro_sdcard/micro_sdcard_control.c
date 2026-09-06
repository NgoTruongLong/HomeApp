#include "micro_sdcard_control.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "debug.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include <dirent.h>

/******************************* DEFINITIONS *******************************/
#define THIS_MODULE_NAME "micro_sdcard_control"
#define MOUNT_POINT "/sdcard"   /* mount point for the SD card filesystem */
/******************************* FUNCTIONS PROTOTYPE *******************************/
static bool micro_sdcard_is_wav(const char *file_name, size_t len);
/******************************* DATA TYPES *******************************/
sdcard_t sdcard_control;
/******************************* VARIABLES *******************************/

/******************************* FUNCTIONS IMPLEMENTATION *******************************/
APP_RESULT micro_sdcard_init() {
    APP_RESULT ret = APP_OK;
    memset(&sdcard_control, 0, sizeof(sdcard_t));
    // sd card instance
    sdmmc_card_t *card;

    // config properties for mounting the SD card
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.unaligned_multi_block_rw_max_chunk_size = 8;
    host.max_freq_khz = 10000; // Set the maximum frequency to 40 MHz

    // config SPI bus pins for the SD card
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    ASSERT_CRITICAL(ret == APP_OK, ret, APP_ERROR);

    // config device interface for the SD card
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = host.slot;

    ESP_LOGI(THIS_MODULE_NAME, "Mounting filesystem");
    ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);

    if (ret != APP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(THIS_MODULE_NAME, "Failed to mount filesystem. "
                     "If you want the card to be formatted, set the CONFIG_EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option.");
        } else {
            ESP_LOGE(THIS_MODULE_NAME, "Failed to initialize the card (%s). "
                     "Make sure SD card lines have pull-up resistors in place.", esp_err_to_name(ret));
        }
        ASSERT_CRITICAL(false, ret, APP_ERROR);
    }
    ESP_LOGI(THIS_MODULE_NAME, "Filesystem mounted");
    sdcard_control.is_initialized = true;
    sdcard_control.is_mounted = true;
    // Card has been initialized, print its properties
    sdmmc_card_print_info(stdout, card);

    //scan the SD card for .wav files and populate the playlist
    ret = micro_sdcard_list_music_files(MOUNT_POINT, &sdcard_control.playlist);
    ASSERT_CRITICAL(ret == APP_OK, ret, APP_ERROR);

    for (int i = 0; i < sdcard_control.playlist.count; i++) {
        ESP_LOGI(THIS_MODULE_NAME, "Found song: %s", sdcard_control.playlist.songs[i]);
    }

#if 0
    size_t len = 0;
    // write to file to test the SD card
    const char *file_test = MOUNT_POINT"/test.txt";
    const char *data_to_write = "Hello, SD card!";
    len = strlen(data_to_write);
    ret = micro_sdcard_write_file(file_test, data_to_write, &len, true);
    ASSERT_CRITICAL(ret == APP_OK, ret, APP_ERROR);

    vTaskDelay(pdMS_TO_TICKS(100)); // Delay to ensure the write operation is completed

    // read from file to test the SD card
    char data_to_read[100] = {0};
    len = sizeof(data_to_read);
    ret = micro_sdcard_read_file(file_test, (void *)data_to_read, &len);
    ASSERT(ret == APP_OK, ret);

    printf("Data read from SD card: %s\n", data_to_read);
#endif
    return ret;
}

APP_RESULT micro_sdcard_write_file(const char *file_path, const void *data, size_t *len, bool append) {
    APP_RESULT ret = APP_OK;

    ASSERT_CRITICAL(data != NULL, APP_ERROR, APP_ERROR);

    FILE *f = fopen(file_path, append ? "a" : "w");
    ASSERT_CRITICAL(f != NULL, APP_ERROR, APP_ERROR);

    size_t bytes_written = fwrite(data, 1, *len, f);
    ASSERT(bytes_written == *len, APP_ERROR);
    *len = bytes_written; // Update the length to reflect the actual number of bytes written

    fclose(f);
    ESP_LOGI(THIS_MODULE_NAME, "Wrote %d bytes to %s", bytes_written, file_path);
    return ret;
}

APP_RESULT micro_sdcard_read_file(const char *file_path, void *data, size_t *len) {
    APP_RESULT ret = APP_OK;

    ASSERT_CRITICAL(data != NULL, APP_ERROR, APP_ERROR);

    FILE *f = fopen(file_path, "r");
    ASSERT_CRITICAL(f != NULL, APP_ERROR, APP_ERROR);


    /* Đi đến cuối file */
    fseek(f, 0, SEEK_END);

    /* Lấy kích thước file */
    long file_size = ftell(f);

    /* Quay lại đầu file */
    fseek(f, 0, SEEK_SET);

    if(*len < file_size) {
        ESP_LOGW(THIS_MODULE_NAME, "Buffer size (%d) is smaller than file size (%d). Only reading %d bytes.", *len, file_size, *len);
        ASSERT_CRITICAL(false, APP_ERROR, APP_ERROR);
    } else {
        *len = file_size;
    }

    size_t byte_read = fread(data, 1, *len, f);
    ASSERT(byte_read == *len, APP_ERROR);

    fclose(f);
    ESP_LOGI(THIS_MODULE_NAME, "Read %d bytes from %s", byte_read, file_path);
    return ret;
}

static bool micro_sdcard_is_wav(const char *file_name, size_t len) {
    if (len < 4) {
        return false;
    }
    return (strcasecmp(file_name + len - strlen(".wav"), ".wav") == 0);
}

APP_RESULT micro_sdcard_list_music_files(const char* directory_path, playlist_t *playlist) {
    APP_RESULT ret = APP_OK;
    size_t song_name_length;
    DIR *dir = NULL;

    playlist->count = 0;
    dir = opendir(directory_path);
    ASSERT_CRITICAL(dir != NULL, APP_ERROR, APP_ERROR);

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {

        if(entry->d_type != DT_REG) {
            continue;
        }

        song_name_length = strlen(entry->d_name);
        if (!micro_sdcard_is_wav(entry->d_name, song_name_length)) {
            continue;
        }

        if (playlist->count > PLAYLIST_MAX_SONGS) {
            ESP_LOGW(THIS_MODULE_NAME, "Reached maximum number of songs (%d). Some files may not be listed.", PLAYLIST_MAX_SONGS);
            break;
        }

        sprintf(playlist->songs[playlist->count], "%s/%s", directory_path, entry->d_name);
        ESP_LOGI(THIS_MODULE_NAME, "Found song: %s", playlist->songs[playlist->count]);
        playlist->count++;
    }

    closedir(dir);
    if (playlist->count == 0) {
        ESP_LOGW(THIS_MODULE_NAME, "Cannot found any .wav file in %s", directory_path);
    }
    return ret;
}

sdcard_t* micro_sdcard_get_control() {
    return &sdcard_control;
}