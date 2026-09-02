#ifndef AUDIO_CONTROL
#define AUDIO_CONTROL

#include "debug.h"
#include "commons.h"
#include "driver/i2s_std.h"
/******************************* DEFINITIONS *******************************/

/******************************* FUNCTIONS PROTOTYPE *******************************/
APP_RESULT audio_init(void);
/******************************* DATA TYPES *******************************/
typedef struct {
    i2s_chan_handle_t i2s_speaker_handle;
} audio_t;
/******************************* VARIABLES *******************************/

/******************************* FUNCTIONS IMPLEMENTATION *******************************/

#endif /* AUDIO_CONTROL */