# LMS Head v0 — the KISS wiring

One ESP32-S3-DevKitC-1 and resistors. No expander, no external ADC, the 20x4 you already have on its I2C
backpack. Firmware: `firmware/head/head.ino` (board esp32:esp32:esp32s3, USB Mode "USB-OTG (TinyUSB)",
library hd44780). The rig finds it by its USB name, "LMS Head".

Pot ends to **3V3** and GND (not 5V). Encoder commons to GND. Every switch to GND.

## ESP32-S3 pins

| GPIO | goes to |
|---|---|
| 1, 2, 4, 5, 6, 7 | GAIN, BASS, MIDDLE, TREBLE, PRESENCE, MASTER wipers |
| 8 | the soft-button ladder (below) |
| 9 | the encoder-push ladder (below) |
| 10 / 11 | AMP encoder A / B |
| 12 / 13 | CAB A encoder A / B |
| 14 / 15 | CAB B encoder A / B |
| 16 / 17 | soft 1 encoder A / B |
| 18 / 21 | soft 2 encoder A / B |
| 38 / 39 | soft 3 encoder A / B |
| 40 / 41 | SDA / SCL to the LCD backpack (its own pull-ups are fine) |
| 42, 47, 3, 0 | LED rings of soft buttons 1–4 (bare "3V" rings: 220 Ω in series; 5 V rings: NPN or 2N7000 from the pin) |
| 5V, GND | LCD backpack VCC / GND |

3 and 0 are strapping pins: driving an LED from them after boot is fine, just don't hold a button on them
while plugging in. 19/20 are the USB pins, leave them. 48 is the onboard RGB LED, leave it.

## The two ladders (ten switches on two pins)

One 10 kΩ from the pin to 3V3. Each switch connects the pin to GND through its own resistor:

| soft buttons (GPIO 8) | resistor | reads |
|---|---|---|
| button 1 | wire | 0 V |
| button 2 | 1 kΩ | 0.30 V |
| button 3 | 3.3 kΩ | 0.82 V |
| button 4 (PAGE) | 10 kΩ | 1.65 V |

| encoder pushes (GPIO 9) | resistor | reads |
|---|---|---|
| AMP | wire | 0 V |
| CAB A | 1 kΩ | 0.30 V |
| CAB B | 2.2 kΩ | 0.60 V |
| soft 1 | 4.7 kΩ | 1.06 V |
| soft 2 | 10 kΩ | 1.65 V |
| soft 3 | 22 kΩ | 2.27 V |

Nothing pressed reads 3.3 V. One press at a time per ladder, which is how buttons get used anyway.

## What v0 does

Pots send 14-bit CC 20–25 on change. Buttons send CC 80–83 presses, the rig answers with the LED state.
AMP / CAB A / CAB B encoders step an index on CC 102–104 (the rig tells the box the count). Soft encoders
accumulate CC 26–28. The LCD shows the amp and cab names the rig sends, and on the last line the label and
value of whatever you touched last. Soft takeover is not in v0: a pot sends where it is.

## Flash

```
cd ~/lms-rig/surface/firmware/head
arduino-cli compile --fqbn esp32:esp32:esp32s3:USBMode=default .
arduino-cli upload  --fqbn esp32:esp32:esp32s3:USBMode=default -p /dev/ttyACM0 .
```
