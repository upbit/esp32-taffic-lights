# TrafficLights

A minimal traffic-light demo for the ESP32, written in C on top of
ESP-IDF (via PlatformIO). It drives three LEDs (green / yellow / red)
through a classic countdown cycle and lets the user trigger an early
phase change with the on-board **BOOT** button.

## Features

- **Two-phase traffic-light FSM**: GREEN → YELLOW (blinking) → RED →
  YELLOW (blinking) → GREEN, looping forever.
- **Tail-blink countdown**: during the last few seconds of a GREEN or
  RED phase the active light blinks once per second to warn that the
  phase is about to end.
- **Yellow transition**: the yellow LED blinks at 1 Hz between every
  GREEN ↔ RED switch.
- **Manual override via BOOT button (GPIO0)**: press and release the
  BOOT button while GREEN or RED is counting down to immediately jump
  into the yellow transition and switch to the other color.
- **Power-on self-test**: all three LEDs blink twice at boot so the
  wiring can be visually verified before the cycle starts.
- **Per-second status logging** over the serial monitor.

## Hardware

Target board: **NodeMCU-32S** (any ESP32 dev board with the standard
BOOT button on GPIO0 will work).

| Signal       | ESP32 pin | Notes                                           |
| ------------ | --------- | ----------------------------------------------- |
| Green LED    | GPIO 25   | Active-high, through a current-limiting resistor |
| Yellow LED   | GPIO 26   | Active-high                                     |
| Red LED      | GPIO 27   | Active-high                                     |
| BOOT button  | GPIO 0    | Active-low, on-board (with internal pull-up)    |

Each LED should be driven through a series resistor (typ. 220–470 Ω)
to GND. The BOOT button is the one already mounted on the dev board,
no extra wiring is needed.

> ⚠️ **GPIO0 is a strapping pin.** It is sampled at reset to choose the
> boot mode (LOW = download mode, HIGH = normal boot). Do **not** hold
> the BOOT button while pressing EN/RESET, otherwise the chip will
> stay in download mode instead of running the application. After
> normal boot the pin is used as a regular input and is safe to press
> at any time.

## Project layout

```
TrafficLights/
├── platformio.ini    # PlatformIO config (board, framework, monitor speed)
├── src/
│   └── main.c        # All firmware logic (FSM + button task)
└── README.md
```

## Build & flash

This project uses [PlatformIO](https://platformio.org/) with the
`espidf` framework.

```bash
# Build
pio run

# Flash to the connected board
pio run -t upload

# Open the serial monitor (115200 baud)
pio device monitor
```

## Runtime behavior

After reset:

1. **Self-test** — all three LEDs blink together twice (~2 seconds).
2. **GREEN phase** — green LED on for `COUNTDOWN_SECONDS` seconds; in
   the last `BLINK_TAIL_TIMES + 1` seconds it blinks at 1 Hz.
3. **YELLOW phase** — yellow LED blinks at 1 Hz for `YELLOW_SECONDS`
   seconds.
4. **RED phase** — same as GREEN but with the red LED.
5. **YELLOW phase** again, then back to step 2.

### Manual override

While a GREEN or RED countdown is running, **press and release** the
BOOT button. On release, the current phase is cut short within
~500 ms and the firmware moves directly into the yellow transition,
then on to the opposite color. Releasing the button while yellow is
already blinking is intentionally ignored to avoid chattering.

### Sample serial output

```
I (...) TRAFFIC: Traffic light starting...
I (...) TRAFFIC: Self-test: blink all LEDs 2 time(s)
I (...) TRAFFIC: GREEN: 60s
I (...) TRAFFIC: GREEN: 59s
...
I (...) TRAFFIC: GREEN: 4s
I (...) TRAFFIC: GREEN: 3s    <- starts blinking from here
I (...) TRAFFIC: GREEN: 2s
I (...) TRAFFIC: GREEN: 1s
I (...) TRAFFIC: YELLOW: 3s
I (...) TRAFFIC: YELLOW: 2s
I (...) TRAFFIC: YELLOW: 1s
I (...) TRAFFIC: RED: 60s
...
```

## Configuration

All timing knobs live at the top of [`src/main.c`](src/main.c):

| Macro                   | Default | Meaning                                              |
| ----------------------- | ------: | ---------------------------------------------------- |
| `COUNTDOWN_SECONDS`     |    60   | Length of each GREEN / RED phase (seconds)           |
| `BLINK_TAIL_TIMES`      |     3   | How many seconds at the tail blink at 1 Hz           |
| `YELLOW_SECONDS`        |     3   | Length of the yellow transition (seconds)            |
| `BLINK_HALF_PERIOD_MS`  |   500   | Half period of the 1 Hz blink (ms)                   |
| `BUTTON_POLL_INTERVAL_MS` |  20   | BOOT-button polling period (ms)                      |
| `BUTTON_DEBOUNCE_MS`    |    30   | Debounce time for the BOOT button (ms)               |

Pin assignments are right next to them and can be changed to fit
your wiring.

## Architecture notes

- **Two FreeRTOS tasks** are running:
  - `app_main` (the IDF main task) hosts the traffic-light FSM.
  - `btn_task` polls GPIO0 with a 20 ms tick and applies a 30 ms
    debounce.
- They communicate through a single `volatile bool g_switch_request`
  flag — the button task sets it on release, the FSM consumes it.
  No mutex is needed for a one-bit flag with a single writer and
  single reader.
- The FSM checks the flag after every 500 ms LED sub-tick, so the
  worst-case reaction latency to a button release is one half-period
  (~500 ms).
- The yellow phase deliberately ignores the flag and clears it on
  entry and exit, so the user cannot keep re-triggering switches in
  rapid succession.

## License

This project is provided as-is for demo / educational purposes.
