/*
 * mic_inmp441.h - driver I2S RX cho microphone INMP441.
 *
 * INMP441 nối: BCLK=GPIO5, WS=GPIO6, DIN=GPIO15 (chân L/R nối GND).
 * Dùng I2S_NUM_1 (I2S0 đã dành cho loa MAX98357A), slot 32-bit Philips,
 * đọc mono kênh LEFT rồi dịch phải 16 bit để ra PCM 16-bit @24 kHz.
 */
#ifndef MIC_INMP441_H
#define MIC_INMP441_H

#include "debug.h"
#include "driver/i2s_std.h"

/******************************* DEFINITIONS *******************************/
#define MIC_I2S_PORT        (I2S_NUM_1)
#define MIC_I2S_BCLK_PIN    (GPIO_NUM_5)
#define MIC_I2S_WS_PIN      (GPIO_NUM_6)
#define MIC_I2S_DIN_PIN     (GPIO_NUM_15)
#define MIC_SAMPLE_RATE     (24000)

/******************************* FUNCTIONS PROTOTYPE *******************************/
APP_RESULT mic_inmp441_init(void);
/* Đọc PCM 16-bit mono (đã scale về int16). Trả về số frame đọc được. */
size_t mic_inmp441_read(int16_t *out, size_t max_frames, uint32_t timeout_ms);

#endif /* MIC_INMP441_H */
