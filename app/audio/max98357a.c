#include "max98357a.h"

/******************************* DEFINITIONS *******************************/

/******************************* FUNCTIONS PROTOTYPE *******************************/

/******************************* DATA TYPES *******************************/

/******************************* VARIABLES *******************************/

/******************************* FUNCTIONS IMPLEMENTATION *******************************/
APP_RESULT max98357a_init(i2s_chan_handle_t *i2s_speaker_handle) {
    APP_RESULT ret = APP_OK;
    // init i2s speaker
    static i2s_chan_config_t i2s_master_speaker_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ret = i2s_new_channel(&i2s_master_speaker_cfg, i2s_speaker_handle, NULL);
    ASSERT_CRITICAL(ret == APP_OK, ret, ret);

    i2s_std_config_t i2s_speaker_std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SPEAKER_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),  //I2S_STD_MSB_SLOT_DEFAULT_CONFIG
        .gpio_cfg = {
            .mclk = I2S_SPEAKER_MCLK,    // some codecs may require mclk signal, this example doesn't need it
            .bclk = I2S_SPEAKER_BCLK,
            .ws   = I2S_SPEAKER_WS,
            .dout = I2S_SPEAKER_DOUT,
            .din  = I2S_SPEAKER_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(*i2s_speaker_handle, &i2s_speaker_std_cfg);
    ASSERT_CRITICAL(ret == APP_OK, ret, ret);
    ret = i2s_channel_enable(*i2s_speaker_handle);
    ASSERT_CRITICAL(ret == APP_OK, ret, ret);

    return APP_OK;
}
