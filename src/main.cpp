#include <Arduino.h>

/* ===== GPIO pin definitions for ESP8266 NodeMCU =====
 * Default pins can be overridden via platformio.ini build_flags:
 *   -DLED_GREEN_GPIO=5  etc. */
#ifndef LED_GREEN_GPIO
#define LED_GREEN_GPIO 5 // D1
#endif
#ifndef LED_YELLOW_GPIO
#define LED_YELLOW_GPIO 4 // D2
#endif
#ifndef LED_RED_GPIO
#define LED_RED_GPIO 14 // D5
#endif

/* BOOT/Flash button on GPIO0: active-low with internal pull-up.
 * On NodeMCU this is the FLASH button (D3). */
#ifndef BOOT_BUTTON_GPIO
#define BOOT_BUTTON_GPIO 0 // D3 (Flash button)
#endif
#define BUTTON_DEBOUNCE_MS 30

/* ===== Timing configuration ===== */
#define COUNTDOWN_SECONDS 60     /* Red / Green countdown duration (seconds) */
#define BLINK_TAIL_TIMES 3       /* Blink how many times at the tail of the countdown */
#define YELLOW_SECONDS 3         /* Yellow blink duration between switches (seconds) */
#define BLINK_HALF_PERIOD_MS 500 /* Blink toggles every 500 ms => 1 Hz */

/* Set by button_poll() on release, consumed by the FSM. */
static volatile bool g_switch_request = false;

typedef enum
{
    STATE_GREEN = 0,
    STATE_YELLOW,
    STATE_RED,
} traffic_state_t;

static const char *state_name(traffic_state_t s)
{
    switch (s)
    {
    case STATE_GREEN:
        return "GREEN";
    case STATE_YELLOW:
        return "YELLOW";
    case STATE_RED:
        return "RED";
    default:
        return "UNKNOWN";
    }
}

static void leds_init(void)
{
    pinMode(LED_GREEN_GPIO, OUTPUT);
    pinMode(LED_YELLOW_GPIO, OUTPUT);
    pinMode(LED_RED_GPIO, OUTPUT);

    digitalWrite(LED_GREEN_GPIO, LOW);
    digitalWrite(LED_YELLOW_GPIO, LOW);
    digitalWrite(LED_RED_GPIO, LOW);
}

static void button_init(void)
{
    pinMode(BOOT_BUTTON_GPIO, INPUT_PULLUP);
}

/* Poll the BOOT button with debounce; raise g_switch_request on release.
 * Call this function frequently (it is called inside every delay cycle). */
static void button_poll(void)
{
    static int last_stable = HIGH;
    static int last_sample = HIGH;
    static unsigned long stable_since = 0;

    int level = digitalRead(BOOT_BUTTON_GPIO);
    unsigned long now = millis();

    if (level != last_sample)
    {
        last_sample = level;
        stable_since = now;
    }
    else if (level != last_stable &&
             (now - stable_since) >= BUTTON_DEBOUNCE_MS)
    {
        last_stable = level;
        if (level == HIGH)
        { /* released (INPUT_PULLUP: HIGH = not pressed) */
            g_switch_request = true;
        }
    }
}

/* Non-blocking delay that also polls the button.
 * Returns false if aborted by a button-driven switch request. */
static bool delay_poll(unsigned long ms)
{
    unsigned long start = millis();
    while (millis() - start < ms)
    {
        button_poll();
        if (g_switch_request)
        {
            return false;
        }
        delay(10);
    }
    return true;
}

static void leds_set(int green, int yellow, int red)
{
    digitalWrite(LED_GREEN_GPIO, green ? HIGH : LOW);
    digitalWrite(LED_YELLOW_GPIO, yellow ? HIGH : LOW);
    digitalWrite(LED_RED_GPIO, red ? HIGH : LOW);
}

/* Power-on self test: blink all three LEDs together `times` times. */
static void leds_self_test(int times)
{
    Serial.printf("Self-test: blink all LEDs %d time(s)\n", times);
    for (int i = 0; i < times; i++)
    {
        leds_set(1, 1, 1);
        delay(BLINK_HALF_PERIOD_MS);
        leds_set(0, 0, 0);
        delay(BLINK_HALF_PERIOD_MS);
    }
}

/* Run a one-second tick. Returns false if the button asked us to switch. */
static bool run_one_second(traffic_state_t state, bool blink)
{
    const int ticks = 1000 / BLINK_HALF_PERIOD_MS;
    for (int i = 0; i < ticks; i++)
    {
        int on = blink ? (i % 2 == 0) : 1;
        switch (state)
        {
        case STATE_GREEN:
            leds_set(on, 0, 0);
            break;
        case STATE_YELLOW:
            leds_set(0, on, 0);
            break;
        case STATE_RED:
            leds_set(0, 0, on);
            break;
        }
        if (!delay_poll(BLINK_HALF_PERIOD_MS))
        {
            return false;
        }
    }
    return true;
}

/* Run a colored phase; cut short on a button-driven switch request. */
static void run_phase(traffic_state_t state, int total_seconds, int blink_tail_times)
{
    for (int remaining = total_seconds; remaining > 0; remaining--)
    {
        bool blink = (remaining <= (blink_tail_times + 1));
        Serial.printf("%s: %ds\n", state_name(state), remaining);
        if (!run_one_second(state, blink))
        {
            break;
        }
    }
    leds_set(0, 0, 0);
}

/* Yellow blinking transition; not interruptible. */
static void run_yellow_phase(int seconds)
{
    g_switch_request = false;
    for (int remaining = seconds; remaining > 0; remaining--)
    {
        Serial.printf("%s: %ds\n", state_name(STATE_YELLOW), remaining);
        (void)run_one_second(STATE_YELLOW, true);
    }
    leds_set(0, 0, 0);
    g_switch_request = false;
}

void setup(void)
{
    Serial.begin(74880);
    leds_init();
    button_init();
    Serial.println("Traffic light starting...");

    leds_self_test(2);
}

void loop(void)
{
    static traffic_state_t color = STATE_GREEN;

    run_phase(color, COUNTDOWN_SECONDS, BLINK_TAIL_TIMES);
    run_yellow_phase(YELLOW_SECONDS);
    color = (color == STATE_GREEN) ? STATE_RED : STATE_GREEN;
}