# LMS Head v0 — the KISS wiring

Twelve 10k pots, four buttons, the 20x4 you have, one ESP32-S3-DevKitC-1. Nothing else.
Firmware: `firmware/head/head.ino` (board esp32:esp32:esp32s3, USB Mode "USB-OTG (TinyUSB)", library
hd44780). The rig finds it by its USB name, "LMS Head".

Every pot: one end **3V3**, other end GND, wiper to its pin. Every button: pin to GND.
The display is DFRobot's "I2C LCD Module V1.2" (a 2004 with the backpack and the blue contrast pot): of its
cable use VCC -> 5V, GND -> GND, SCL -> 39, SDA -> 38; the other two wires stay unconnected. The library finds
its address (DFRobot uses 0x20) by itself.

| GPIO | goes to |
|---|---|
| 1, 2, 4, 5, 6, 7 | GAIN, BASS, MIDDLE, TREBLE, PRESENCE, MASTER |
| 8, 9, 10 | AMP, CAB A, CAB B (the pot's travel is split into as many bands as the rig has heads / cabs) |
| 11, 12, 13 | soft 1, 2, 3 (the rig's per-amp extra sliders, paged) |
| 14, 15, 16, 17 | soft buttons 1, 2, 3, 4 (4 = PAGE) |
| 38 / 39 | SDA / SCL of the LCD cable |
| 5V, GND | VCC and GND of the LCD cable |

Leave 19/20 (USB), 0/3/45/46 (strapping), 48 (onboard LED). Ground every pot's bottom end to the same
GND rail as the ESP; the wipers are the only thing that moves.

## What v0 does

Pots send 14-bit CC 20–25 and 26–28 on change. The three selector pots send an index on CC 102–104.
Buttons send CC 80–83 presses. The LCD shows the amp and cab names the rig sends, and on the last line the
label and value of whatever you touched last. No soft takeover in v0: a pot sends where it is.

## Flash

```
cd ~/lms-rig/surface/firmware/head
arduino-cli compile --fqbn esp32:esp32:esp32s3:USBMode=default .
arduino-cli upload  --fqbn esp32:esp32:esp32s3:USBMode=default -p /dev/ttyACM0 .
```
