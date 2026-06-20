#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

/* ===== GPIO pin definitions ===== */
#define LED_GREEN_GPIO   GPIO_NUM_25
#define LED_YELLOW_GPIO  GPIO_NUM_26
#define LED_RED_GPIO     GPIO_NUM_27

/* BOOT button on GPIO0: active-low, with internal pull-up enabled.
 * GPIO0 is a strapping pin -- don't hold it during reset, but it is safe
 * to use as a normal input once the app is running. */
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
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
}

/* Power-on self test: blink all three LEDs together `times` times. */
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