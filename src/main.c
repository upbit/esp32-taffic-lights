#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#if defined(ONBOARD_LED_ENABLED) && ONBOARD_LED_ENABLED
#include "led_strip.h"
#endif

/* ===== GPIO pin definitions =====
 * The actual pin numbers are injected by platformio.ini via build_flags
 * (-DLED_GREEN_GPIO=..., etc.) so the same source can target different
 * boards. The defaults below are only used as a fallback. */
#ifndef LED_GREEN_GPIO
#define LED_GREEN_GPIO   GPIO_NUM_1
#endif
#ifndef LED_YELLOW_GPIO
#define LED_YELLOW_GPIO  GPIO_NUM_2
#endif
#ifndef LED_RED_GPIO
#define LED_RED_GPIO     GPIO_NUM_3
#endif

/* Optional on-board WS2812 RGB LED. The ESP32-S3-DevKitC-1 has one wired
 * to GPIO48. Enable by defining ONBOARD_LED_ENABLED=1 and ONBOARD_LED_GPIO
 * via platformio.ini build_flags. */
#ifndef ONBOARD_LED_GPIO
#define ONBOARD_LED_GPIO        GPIO_NUM_48
#endif
#ifndef ONBOARD_LED_BRIGHTNESS
#define ONBOARD_LED_BRIGHTNESS  32   /* 0..255, keep modest to avoid glare */
#endif

#if defined(ONBOARD_LED_ENABLED) && ONBOARD_LED_ENABLED
static led_strip_handle_t s_onboard_led = NULL;
#endif

/* BOOT button on GPIO0: active-low, with internal pull-up enabled.
 * GPIO0 is a strapping pin -- don't hold it during reset, but it is safe
 * to use as a normal input once the app is running. */
#ifndef BOOT_BUTTON_GPIO
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#endif
#define BUTTON_POLL_INTERVAL_MS 20
#define BUTTON_DEBOUNCE_MS      30

/* ===== Timing configuration ===== */
#define COUNTDOWN_SECONDS    60  /* Red / Green countdown duration (seconds) */
#define BLINK_TAIL_TIMES     3   /* Blink how many times at the tail of the countdown */
#define YELLOW_SECONDS       3   /* Yellow blink duration between switches (seconds) */
#define BLINK_HALF_PERIOD_MS 500 /* Blink toggles every 500 ms => 1 Hz */

static const char *TAG = "TRAFFIC";

/* Set by the button task on release, consumed by the FSM. */
static volatile bool g_switch_request = false;

typedef enum {
    STATE_GREEN = 0,
    STATE_YELLOW,
    STATE_RED,
} traffic_state_t;

static const char *state_name(traffic_state_t s)
{
    switch (s) {
        case STATE_GREEN:  return "GREEN";
        case STATE_YELLOW: return "YELLOW";
        case STATE_RED:    return "RED";
        default:           return "UNKNOWN";
    }
}

static void onboard_led_init(void)
{
#if defined(ONBOARD_LED_ENABLED) && ONBOARD_LED_ENABLED
    led_strip_config_t strip_cfg = {
        .strip_gpio_num   = ONBOARD_LED_GPIO,
        .max_leds         = 1,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .led_model        = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src        = RMT_CLK_SRC_DEFAULT,
        .resolution_hz  = 10 * 1000 * 1000, /* 10 MHz */
        .flags.with_dma = false,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_onboard_led);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "On-board LED init failed: %s", esp_err_to_name(err));
        s_onboard_led = NULL;
        return;
    }
    led_strip_clear(s_onboard_led);
#endif
}

static void onboard_led_set(uint8_t r, uint8_t g, uint8_t b)
{
#if defined(ONBOARD_LED_ENABLED) && ONBOARD_LED_ENABLED
    if (s_onboard_led == NULL) {
        return;
    }
    led_strip_set_pixel(s_onboard_led, 0, r, g, b);
    led_strip_refresh(s_onboard_led);
#else
    (void)r; (void)g; (void)b;
#endif
}

static void leds_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_GREEN_GPIO) |
                        (1ULL << LED_YELLOW_GPIO) |
                        (1ULL << LED_RED_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    gpio_set_level(LED_GREEN_GPIO, 0);
    gpio_set_level(LED_YELLOW_GPIO, 0);
    gpio_set_level(LED_RED_GPIO, 0);

    onboard_led_init();
    onboard_led_set(0, 0, 0);
}

static void button_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
}

/* Poll the BOOT button with debounce; raise g_switch_request on release. */
static void button_task(void *arg)
{
    int last_stable        = gpio_get_level(BOOT_BUTTON_GPIO);
    int last_sample        = last_stable;
    TickType_t stable_since = xTaskGetTickCount();

    while (1) {
        int level = gpio_get_level(BOOT_BUTTON_GPIO);

        if (level != last_sample) {
            last_sample  = level;
            stable_since = xTaskGetTickCount();
        } else if (level != last_stable &&
                   (xTaskGetTickCount() - stable_since) >=
                       pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS)) {
            last_stable = level;
            if (level == 1) { /* released */
                g_switch_request = true;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_INTERVAL_MS));
    }
}

static void leds_set(int green, int yellow, int red)
{
    gpio_set_level(LED_GREEN_GPIO, green ? 1 : 0);
    gpio_set_level(LED_YELLOW_GPIO, yellow ? 1 : 0);
    gpio_set_level(LED_RED_GPIO, red ? 1 : 0);

    /* Mirror the discrete LED pattern onto the on-board WS2812.
     * - Self-test (all three on) -> white
     * - Single color on          -> matching color
     * - All off                  -> dark */
    const uint8_t lvl = ONBOARD_LED_BRIGHTNESS;
    uint8_t r = red    ? lvl : 0;
    uint8_t g = green  ? lvl : 0;
    uint8_t b = 0;
    if (yellow) { /* yellow = red + green */
        r = lvl;
        g = lvl;
    }
    onboard_led_set(r, g, b);
}

/* Power-on self test: blink all three LEDs together `times` times.
 * The on-board LED follows along in white via leds_set(). */
static void leds_self_test(int times)
{
    ESP_LOGI(TAG, "Self-test: blink all LEDs %d time(s)", times);
    for (int i = 0; i < times; i++) {
        leds_set(1, 1, 1);
        vTaskDelay(pdMS_TO_TICKS(BLINK_HALF_PERIOD_MS));
        leds_set(0, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(BLINK_HALF_PERIOD_MS));
    }
}

/* Run a one-second tick. Returns false if the button asked us to switch. */
static bool run_one_second(traffic_state_t state, bool blink)
{
    const int ticks = 1000 / BLINK_HALF_PERIOD_MS;
    for (int i = 0; i < ticks; i++) {
        int on = blink ? (i % 2 == 0) : 1;
        switch (state) {
            case STATE_GREEN:  leds_set(on, 0, 0); break;
            case STATE_YELLOW: leds_set(0, on, 0); break;
            case STATE_RED:    leds_set(0, 0, on); break;
        }
        vTaskDelay(pdMS_TO_TICKS(BLINK_HALF_PERIOD_MS));
        if (g_switch_request) {
            return false;
        }
    }
    return true;
}

/* Run a colored phase; cut short on a button-driven switch request. */
static void run_phase(traffic_state_t state, int total_seconds, int blink_tail_times)
{
    for (int remaining = total_seconds; remaining > 0; remaining--) {
        bool blink = (remaining <= (blink_tail_times + 1));
        ESP_LOGI(TAG, "%s: %ds", state_name(state), remaining);
        if (!run_one_second(state, blink)) {
            break;
        }
    }
    leds_set(0, 0, 0);
}

/* Yellow blinking transition; not interruptible. */
static void run_yellow_phase(int seconds)
{
    g_switch_request = false;
    for (int remaining = seconds; remaining > 0; remaining--) {
        ESP_LOGI(TAG, "%s: %ds", state_name(STATE_YELLOW), remaining);
        (void)run_one_second(STATE_YELLOW, true);
    }
    leds_set(0, 0, 0);
    g_switch_request = false;
}

void app_main(void)
{
    leds_init();
    button_init();
    ESP_LOGI(TAG, "Traffic light starting...");

    xTaskCreate(button_task, "btn_task", 2048, NULL, 5, NULL);

    leds_self_test(2);

    traffic_state_t color = STATE_GREEN;
    while (1) {
        run_phase(color, COUNTDOWN_SECONDS, BLINK_TAIL_TIMES);
        run_yellow_phase(YELLOW_SECONDS);
        color = (color == STATE_GREEN) ? STATE_RED : STATE_GREEN;
    }
}