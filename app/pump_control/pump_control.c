#include "pump_control.h"
#include <driver/ledc.h>
#include <string.h>

/******************************* DEFINITIONS *******************************/
#define PUMP_PWM_GPIO           (GPIO_NUM_4)
#define PUMP_PWM_FREQ_HZ        (5000)              /* PWM frequency in Hz */
#define PUMP_PWM_DUTY_RES       (LEDC_TIMER_10_BIT) /* duty resolution: 10-bit -> 0..1023 */
#define PUMP_PWM_TIMER          (LEDC_TIMER_0)
#define PUMP_PWM_CHANNEL        (LEDC_CHANNEL_0)
#define PUMP_PWM_MODE           (LEDC_LOW_SPEED_MODE)
#define PUMP_PWM_MAX_DUTY       ((1 << PUMP_PWM_DUTY_RES) - 1)  /* max duty value (1023) */

/******************************* FUNCTIONS PROTOTYPE *******************************/
static APP_RESULT pump_set_duty_raw(uint32_t duty);
/******************************* DATA TYPES *******************************/
typedef struct {
    PUMP_STATE eCurrentState;
    uint32_t   duty;   /* current duty in [0, PUMP_PWM_MAX_DUTY] */
} pump_t;
/******************************* VARIABLES *******************************/
static pump_t pump_control;

/******************************* FUNCTIONS IMPLEMENTATION *******************************/
static APP_RESULT pump_set_duty_raw(uint32_t duty) {
    APP_RESULT ret = APP_OK;

    ret = ledc_set_duty(PUMP_PWM_MODE, PUMP_PWM_CHANNEL, duty);
    ASSERT(ret == APP_OK, ret);

    ret = ledc_update_duty(PUMP_PWM_MODE, PUMP_PWM_CHANNEL);
    ASSERT(ret == APP_OK, ret);

    pump_control.duty = duty;
    return ret;
}

APP_RESULT pump_init() {
    APP_RESULT ret = APP_OK;
    memset((void *)&pump_control, 0, sizeof(pump_control));

    /* Configure LEDC timer */
    ledc_timer_config_t timer_conf = {
        .speed_mode      = PUMP_PWM_MODE,
        .duty_resolution = PUMP_PWM_DUTY_RES,
        .timer_num       = PUMP_PWM_TIMER,
        .freq_hz         = PUMP_PWM_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ret = ledc_timer_config(&timer_conf);
    ASSERT_CRITICAL(ret == APP_OK, ret, ret);

    /* Configure LEDC channel on pump GPIO */
    ledc_channel_config_t channel_conf = {
        .gpio_num   = PUMP_PWM_GPIO,
        .speed_mode = PUMP_PWM_MODE,
        .channel    = PUMP_PWM_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = PUMP_PWM_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ret = ledc_channel_config(&channel_conf);
    ASSERT_CRITICAL(ret == APP_OK, ret, ret);

    return ret;
}

APP_RESULT pump_set_state(PUMP_STATE state) {
    APP_RESULT ret = APP_OK;

    if (state != pump_control.eCurrentState) {
        pump_control.eCurrentState = state;
        ret = pump_set_duty_raw((state == PUMP_ON) ? PUMP_PWM_MAX_DUTY : 0);
        ASSERT(ret == APP_OK, ret);
    }
    return ret;
}

APP_RESULT pump_set_duty(uint8_t duty_percent) {
    APP_RESULT ret = APP_OK;
    uint32_t duty;

    if (duty_percent > PUMP_DUTY_MAX) {
        duty_percent = PUMP_DUTY_MAX;
    }

    duty = (uint32_t)duty_percent * PUMP_PWM_MAX_DUTY / PUMP_DUTY_MAX;
    ret = pump_set_duty_raw(duty);
    ASSERT(ret == APP_OK, ret);

    pump_control.eCurrentState = (duty_percent > 0) ? PUMP_ON : PUMP_OFF;
    return ret;
}

uint8_t pump_get_duty() {
    return (uint8_t)((uint32_t)pump_control.duty * PUMP_DUTY_MAX / PUMP_PWM_MAX_DUTY);
}

PUMP_STATE pump_get_state() {
    return pump_control.eCurrentState;
}