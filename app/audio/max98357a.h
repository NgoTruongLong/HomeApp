#ifndef MAX98357A_H
#define MAX98357A_H

#include "debug.h"
#include "commons.h"
#include "driver/i2s_std.h"

/******************************* DEFINITIONS *******************************/
#define SPEAKER_SAMPLE_RATE (44100)
#define MICRO_SAMPLE_RATE   (16000)

#define I2S_BUFFER_SIZE  (1600)

#define TEST_AUDIO_SHARED_MEM_SIZE  (16000)

// I2S PIN TX (MAX98357A)
#define I2S_SPEAKER_MCLK                 (I2S_GPIO_UNUSED)
#define I2S_SPEAKER_BCLK                 (GPIO_NUM_36)
#define I2S_SPEAKER_WS                   (GPIO_NUM_37)   // LCR
#define I2S_SPEAKER_DOUT                 (GPIO_NUM_35)
#define I2S_SPEAKER_DIN                  (I2S_GPIO_UNUSED)

/******************************* FUNCTIONS PROTOTYPE *******************************/
APP_RESULT max98357a_init(i2s_chan_handle_t *i2s_speaker_handle);
/******************************* DATA TYPES *******************************/

/******************************* VARIABLES *******************************/

/******************************* FUNCTIONS IMPLEMENTATION *******************************/



#endif /* MAX98357A_H */