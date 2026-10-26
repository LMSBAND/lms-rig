# LMS Head — electronics (best version)

Everything on 3.3 V, nothing on the ESP's own ADC, every LED dimmable, and the faceplate
unplugs from the back panel. The ESP32-S3-DevKitC-1 sits on its shelf in the back panel with a
small hub board beside it carrying four breakouts; every faceplate lead ends in a JST-XH plug
into that hub.

| Job | Board | Bus | Why |
|---|---|---|---|
| 6 pots | **MCP3208** 12-bit 8-ch ADC | SPI | the ESP32 ADC is noisy and bent at both ends; this one oversampled ×16 gives a rock-steady 14-bit MIDI value |
| 6 encoders A/B | ESP GPIO direct | — | hardware pulse counters (PCNT) on four, interrupts on two; no missed detents |
| 4 buttons + 6 encoder pushes | **MCP23017** 16-ch I/O | I2C 0x20 | one interrupt line, debounced in firmware |
| 4 LED rings | **PCA9685** 16-ch 12-bit PWM | I2C 0x40 | dim when off, bright when on, breathing while a pot waits for pickup; 12 channels spare |
| 20x4 display | **Winstar WEH002004A** OLED | SPI | same 98x60 footprint and holes as the HD44780 it replaces, 3.3 V, instant refresh, no contrast pot, no backlight. The 2004 LCD + PCF8574 backpack (I2C 0x27, 5 V) stays as the fallback |

Buy the DIP-16 MCP3208 and MCP23017 (they go straight on the hub board), the Adafruit-style
PCA9685 breakout, and the OLED in its SPI build (or jumper-selectable).

## ESP32-S3 pins

GPIO 19/20 are USB (the MIDI port), 26–37 flash/PSRAM, 0/3/45/46 strapping, 43/44 UART0,
48 the onboard RGB LED. All of those are left alone.

| GPIO | Signal | Notes |
|---|---|---|
| 1 / 2 | AMP enc A / B | PCNT unit 0 |
| 4 / 5 | CAB A enc A / B | PCNT unit 1 |
| 6 / 7 | CAB B enc A / B | PCNT unit 2 |
| 8 / 14 | soft 1 enc A / B | PCNT unit 3 |
| 15 / 16 | soft 2 enc A / B | interrupt |
| 17 / 18 | soft 3 enc A / B | interrupt |
| 12 | SPI SCK | shared: ADC + OLED |
| 11 | SPI MOSI | |
| 13 | SPI MISO | |
| 10 | MCP3208 CS | |
| 9 | OLED CS | |
| 41 | SDA | 4.7k to 3V3 |
| 42 | SCL | 4.7k to 3V3 |
| 40 | MCP23017 INTA | falling edge |
| 21 / 38 / 39 / 47 | spare | |
| 3V3 / GND | | everything runs from the DevKit's 3V3 (~250 mA worst case) |

## Hub board

| Part | Pin | To |
|---|---|---|
| MCP3208 | CH0–CH5 | GAIN, BASS, MIDDLE, TREBLE, PRESENCE, MASTER wipers (pot ends to 3V3 and GND) |
| | VREF, VDD | 3V3, with 100 nF to GND right at the pin |
| | CLK / DIN / DOUT / CS | GPIO 12 / 11 / 13 / 10 |
| MCP23017 | GPA0–3 | soft button 0–3 (to GND, internal pull-ups) |
| | GPA4–6 | AMP / CAB A / CAB B push (to GND) |
| | GPB0–2 | soft 1–3 push (to GND) |
| | INTA | GPIO 40 |
| | A0 A1 A2 | GND (0x20); RESET to 3V3 |
| PCA9685 | PWM0–3 | LED ring 0–3 anodes; cathodes to GND; 220 Ω in series for bare ("3 V") rings, none for rings with their own resistor |
| | V+ | 3V3 (5 V only if the rings are the 5 V type) |
| OLED | SCLK / SDI / SDO / CS | GPIO 12 / 11 / 13 / 9 |
| | VDD | 3V3 |

## Harness plugs (JST-XH 2.54)

| Plug | Pins | Carries |
|---|---|---|
| POTS | 8 | 3V3, GND, six wipers |
| ENC A | 8 | GND, AMP A B, CAB A A B, CAB B A B, (push on SW plug) |
| ENC B | 8 | GND, soft 1–3 A B, spare |
| SW | 12 | GND, 4 buttons, 6 pushes, spare |
| LED | 6 | GND, 4 rings, spare |
| OLED | 6 | 3V3, GND, SCLK, SDI, SDO, CS |

## Lead colours

3V3 red, GND black, SPI clock yellow / MOSI blue / MISO green, I2C SDA blue / SCL yellow,
pot wipers white, encoder A grey / B purple, switches orange, LEDs pink.
