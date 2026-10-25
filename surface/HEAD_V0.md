# LMS Head v0 — the KISS wiring

Twelve 10k pots, four buttons, the 20x4 you have, one classic ESP32 DevKit (the 38-pin board, "ESP-32"
on the can, EN and IO0 buttons). Nothing else. Firmware: `firmware/head/head.ino` (board esp32:esp32:esp32
"ESP32 Dev Module", library hd44780). The classic ESP32 has no native USB, so the box talks plain MIDI bytes
over its USB serial port at 115200; the rig pings every USB serial port and keeps the one that says HELLO.
Go by the labels printed on the board, not by position.

Every pot: one end **3V3**, other end GND, wiper to its pin. Every button: pin to GND.
The display is DFRobot's "I2C LCD Module V1.2" (a 2004 with the backpack and the blue contrast pot): of its
cable use VCC -> 5V (the pin labelled 5V or VIN), GND -> GND, SCL -> 22, SDA -> 21; the other two wires stay
unconnected. The library finds its address (DFRobot uses 0x20) by itself.

| board pin | goes to |
|---|---|
| VP (36), VN (39), 34, 35, 32, 33 | GAIN, BASS, MIDDLE, TREBLE, PRESENCE, MASTER |
| 25, 26, 27 | AMP, CAB A, CAB B (the pot's travel is split into as many bands as the rig has heads / cabs) |
| 14, 13, 4 | soft 1, 2, 3 (the rig's per-amp extra sliders, paged) |
| 16, 17, 18, 19 | soft buttons 1, 2, 3, 4 (4 = PAGE), other leg to GND |
| 21 / 22 | SDA / SCL of the LCD cable |
| 5V, GND | VCC and GND of the LCD cable |

Leave 0, 2, 12, 15 (they decide how it boots) and 1, 3 (the USB serial). Both GND pins of the board to the
GND rail; every pot's bottom end and every button's other leg to that rail; every pot's top end to the 3V3 rail.
The wipers are the only thing that moves.

## What v0 does

Pots send 14-bit CC 20–25 and 26–28 on change. The three selector pots send an index on CC 102–104.
Buttons send CC 80–83 presses. The LCD shows the amp and cab names the rig sends, and on the last line the
label and value of whatever you touched last. No soft takeover in v0: a pot sends where it is.

## Flash

```
cd ~/lms-rig/surface/firmware/head
arduino-cli compile --fqbn esp32:esp32:esp32 .
arduino-cli upload  --fqbn esp32:esp32:esp32 -p /dev/ttyUSB0 .
```

If the port is /dev/ttyACM0 instead, use that. If the upload says it can't connect, hold IO0 while it says
"Connecting...". Then open LMS Rig: within a few seconds the LCD shows the head's name and the knobs are live.
