#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

/* ===== GPIO pin definitions ===== */
#define LED_GREEN_GPIO   GPIO_NUM_25
#define LED_YELLOW_GPIO  GPIO_NUM_26
#define LED_RED_GPIO     GPIO_NUM_27

/* ===== Timing configuration ===== */
#define COUNTDOWN_SECONDS    30  /* Red / Green countdown duration (seconds) */
#define BLINK_TAIL_TIMES     3   /* Blink how many times at the tail of the countdown */
#define YELLOW_SECONDS       3   /* Yellow blink duration between switches (seconds) */
#define BLINK_HALF_PERIOD_MS 500 /* Blink toggles every 500 ms => 1 Hz */

static const char *TAG = "TRAFFIC";

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
}

static void leds_set(int green, int yellow, int red)
{
    gpio_set_level(LED_GREEN_GPIO, green ? 1 : 0);
    gpio_set_level(LED_YELLOW_GPIO, yellow ? 1 : 0);
    gpio_set_level(LED_RED_GPIO, red ? 1 : 0);
}

/*
 * Power-on self test: blink all three LEDs synchronously `times` times,
 * so that the user can visually confirm every LED is wired and working.
 */
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

/*
 * Run a one-second tick for the given state.
 * If `blink` is true, the corresponding light toggles at BLINK_HALF_PERIOD_MS;
 * otherwise it stays solidly on for the whole second.
 * Other lights are kept off.
 */
static void run_one_second(traffic_state_t state, bool blink)
{
    const int ticks = 1000 / BLINK_HALF_PERIOD_MS; /* sub-cycles per second */
    for (int i = 0; i < ticks; i++) {
        int on = blink ? (i % 2 == 0) : 1;
        switch (state) {
            case STATE_GREEN:  leds_set(on, 0, 0); break;
            case STATE_YELLOW: leds_set(0, on, 0); break;
            case STATE_RED:    leds_set(0, 0, on); break;
        }
        vTaskDelay(pdMS_TO_TICKS(BLINK_HALF_PERIOD_MS));
    }
}

/* Run a colored phase: solid for the first (total - blink_tail_times) seconds,
 * then blink the light once per second for the last `blink_tail_times` seconds.
 * Prints status every second.
 */
static void run_phase(traffic_state_t state, int total_seconds, int blink_tail_times)
{
    for (int remaining = total_seconds; remaining > 0; remaining--) {
        bool blink = (remaining <= (blink_tail_times+1));
        ESP_LOGI(TAG, "%s: %ds", state_name(state), remaining);
        run_one_second(state, blink);
    }
    /* Make sure the LED is off when leaving the phase */
    leds_set(0, 0, 0);
}

/* Yellow blinking transition phase. */
static void run_yellow_phase(int seconds)
{
    for (int remaining = seconds; remaining > 0; remaining--) {
        ESP_LOGI(TAG, "%s: %ds", state_name(STATE_YELLOW), remaining);
        run_one_second(STATE_YELLOW, true);
    }
    leds_set(0, 0, 0);
}

void app_main(void)
{
    leds_init();
    ESP_LOGI(TAG, "Traffic light starting...");

    /* Power-on self test: blink all 3 LEDs together twice so the user can
     * verify wiring before the normal cycle starts. */
    leds_self_test(2);

    /* Boot up directly into GREEN, matching the example output. */
    traffic_state_t color = STATE_GREEN;

    while (1) {
        /* 1) Current color (red or green) for COUNTDOWN_SECONDS,
         *    blink BLINK_TAIL_TIMES times (once per second) at the tail. */
        run_phase(color, COUNTDOWN_SECONDS, BLINK_TAIL_TIMES);

        /* 2) Yellow blinking for YELLOW_SECONDS. */
        run_yellow_phase(YELLOW_SECONDS);

        /* 3) Switch to the other color. */
        color = (color == STATE_GREEN) ? STATE_RED : STATE_GREEN;
    }
}