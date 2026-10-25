// LMS Head, v0 (KISS): one ESP32-S3-DevKitC-1, nothing else between the panel and the host.
// Twelve 10k pots on the ESP's own ADC, four buttons on GPIO, the 20x4 on its I2C backpack. That's it.
// USB-MIDI, class compliant, product name "LMS Head". Wire contract: ../../SPEC.md.
// Wiring: ../../HEAD_V0.md.  The six pots where the spec has encoders stand in for them: three send
// a selection index (AMP / CAB A / CAB B), three send the soft encoders' absolute 14-bit value.
//
// Board: esp32:esp32:esp32s3  USB Mode: USB-OTG (TinyUSB).  Library: hd44780 (Bill Perry).
#include <Arduino.h>
#include <USB.h>
#include <USBMIDI.h>
#include <Wire.h>
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>

USBMIDI MIDI;
hd44780_I2Cexp lcd;

// ---- the panel ---------------------------------------------------------------------------------
static const int POT_PIN[6]   = {1, 2, 4, 5, 6, 7};          // GAIN BASS MIDDLE TREBLE PRESENCE MASTER (ADC1)
static const int POT_CC[6]    = {20, 21, 22, 23, 24, 25};    // 14-bit: MSB cc, LSB cc+32
static const int SEL_PIN[3]   = {8, 9, 10};                  // AMP, CAB A, CAB B: pots that pick an index (ADC1)
static const int SOFT_PIN[3]  = {11, 12, 13};                // soft 1..3: pots, absolute 14-bit (ADC2: fine with WiFi off)
static const int BTN_PIN[4]   = {14, 15, 16, 17};            // soft buttons 1..4 (4 = PAGE), to GND
static const int SDA_PIN = 38, SCL_PIN = 39;

// ---- state -------------------------------------------------------------------------------------
int      potVal[6], softVal[3];     // last sent 14-bit values
int      selIndex[3] = {-1, -1, -1};
int      selCount[3] = {1, 1, 1};   // RANGE from the host
bool     btnDown[4];
uint32_t btnT[4];                   // debounce
char     line[4][21];
uint32_t lcdDue = 0;
bool     lcdDirty = true;
int      lastSlot = 0;              // the control the panel last touched, for line 4

void setLine(int n, const char *s) { snprintf(line[n], 21, "%-20.20s", s); lcdDirty = true; }

// ---- MIDI out ----------------------------------------------------------------------------------
void cc(uint8_t n, uint8_t v) { MIDI.controlChange(n, v, 1); }
void cc14(uint8_t n, int v) { v = constrain(v, 0, 16383); cc(n, (v >> 7) & 127); cc(n + 32, v & 127); }
void hello()
{
    uint8_t m[] = {0xF0, 0x7D, 0x01, 0x10, 0x01, 'H', 'E', 'A', 'D', 0xF7};   // HELLO, firmware 1, "HEAD"
    sysexOut(m, sizeof m);
}
void sysexOut(const uint8_t *d, int n)
{
    // TinyUSB MIDI takes 4-byte packets; stream the SysEx in threes
    for (int i = 0; i < n; i += 3) {
        int left = n - i;
        midiEventPacket_t p = {0, 0, 0, 0};
        p.header = left > 3 ? 0x04 : (left == 3 ? 0x07 : left == 2 ? 0x06 : 0x05);
        p.byte1 = d[i]; p.byte2 = left > 1 ? d[i + 1] : 0; p.byte3 = left > 2 ? d[i + 2] : 0;
        MIDI.writePacket(&p);
    }
}

// ---- MIDI in: the host's labels, values, ranges, LED states -----------------------------------
uint8_t sx[64]; int sxn = 0; bool inSx = false;
char label[0x22][21], value[0x22][21];

void onSysex()
{
    // F0 7D <box> <msg> <slot> <data...> F7
    if (sxn < 5 || sx[1] != 0x7D) return;
    uint8_t msg = sx[3], slot = sx[4];
    const uint8_t *data = sx + 5; int n = sxn - 6;
    if (msg == 0x12) { hello(); return; }
    if (slot >= 0x22) return;
    if (msg == 0x01 || msg == 0x02) {
        char *dst = msg == 0x01 ? label[slot] : value[slot];
        int k = min(n, 20); memcpy(dst, data, k); dst[k] = 0;
        if (slot >= 0x18 && slot <= 0x1A) { char b[21]; snprintf(b, 21, "%c %-18.18s", "ACB"[slot - 0x18], value[slot]); setLine(slot - 0x18, b); }
        if (slot == lastSlot) { char b[21]; snprintf(b, 21, "%-9.9s %10.10s", label[slot], value[slot]); setLine(3, b); }
    } else if (msg == 0x03 && n >= 2 && slot >= 0x18 && slot <= 0x1A) {
        selCount[slot - 0x18] = max(1, (data[0] << 7) | data[1]);
    } else if (msg == 0x04) {
        char b[21]; int k = min(n, 20); memcpy(b, data, k); b[k] = 0; setLine(3, b);
    }
}

void midiIn()
{
    midiEventPacket_t p;
    while (MIDI.readPacket(&p)) {
        uint8_t cin = p.header & 0x0F;
        if (cin == 0x04 || cin == 0x05 || cin == 0x06 || cin == 0x07) {          // sysex pieces
            int n = cin == 0x04 ? 3 : cin - 4;
            uint8_t b[3] = {p.byte1, p.byte2, p.byte3};
            for (int i = 0; i < n; ++i) {
                if (b[i] == 0xF0) { inSx = true; sxn = 0; }
                if (inSx && sxn < 64) sx[sxn++] = b[i];
                if (b[i] == 0xF7) { inSx = false; onSysex(); }
            }
        }
        // CCs from the host (pickup targets, LED states) are ignored in v0: pots send where they are, no LEDs
    }
}

// ---- the panel, read ---------------------------------------------------------------------------
int read14(int pin)
{
    long acc = 0;
    for (int k = 0; k < 8; ++k) acc += analogRead(pin);            // 12-bit x 8 -> 15-bit
    return (int)(acc >> 1);                                         // 14-bit
}

void showSlot(int slot)
{
    lastSlot = slot;
    char b[21]; snprintf(b, 21, "%-9.9s %10.10s", label[slot], value[slot]); setLine(3, b);
}

void readPots()
{
    for (int i = 0; i < 6; ++i) {
        int v = read14(POT_PIN[i]);
        if (abs(v - potVal[i]) > 24) { potVal[i] = v; cc14(POT_CC[i], v); showSlot(i); }   // ~0.15 % of travel
    }
    for (int i = 0; i < 3; ++i) {
        int v = read14(SOFT_PIN[i]);
        if (abs(v - softVal[i]) > 24) { softVal[i] = v; cc14(26 + i, v); showSlot(0x08 + i); }
    }
    for (int i = 0; i < 3; ++i) {
        // a pot as a selector: its travel split into `count` bands, with a little hysteresis at the edges
        int v = read14(SEL_PIN[i]);
        int idx = constrain((int)((long)v * selCount[i] / 16384), 0, selCount[i] - 1);
        if (idx != selIndex[i]) {
            long edge = (long)(idx > selIndex[i] ? idx : idx + 1) * 16384 / selCount[i];
            if (selIndex[i] < 0 || abs(v - edge) > 120) { selIndex[i] = idx; cc(102 + i, idx); showSlot(0x18 + i); }
        }
    }
}

void readButtons()
{
    uint32_t now = millis();
    for (int i = 0; i < 4; ++i) {
        bool down = digitalRead(BTN_PIN[i]) == LOW;
        if (down != btnDown[i] && now - btnT[i] > 30) {
            btnDown[i] = down; btnT[i] = now;
            if (down) { cc(80 + i, 127); showSlot(0x10 + i); }   // "pressed: the new state wanted"
        }
    }
}

void setup()
{
    for (int i = 0; i < 4; ++i) pinMode(BTN_PIN[i], INPUT_PULLUP);
    analogReadResolution(12);
    for (int i = 0; i < 6; ++i) potVal[i] = -1000;
    for (int i = 0; i < 3; ++i) softVal[i] = -1000;
    for (int i = 0; i < 0x22; ++i) { strcpy(label[i], "--"); value[i][0] = 0; }

    Wire.begin(SDA_PIN, SCL_PIN);
    lcd.begin(20, 4);
    setLine(0, "LMS HEAD"); setLine(1, "waiting for the rig"); setLine(2, ""); setLine(3, "");

    USB.productName("LMS Head");
    USB.manufacturerName("LMS");
    MIDI.begin();
    USB.begin();
    delay(300);
    hello();
}

void loop()
{
    midiIn();
    static uint32_t slow = 0;
    if (millis() - slow >= 5) {     // 200 Hz
        slow = millis();
        readPots();
        readButtons();
    }
    if (lcdDirty && millis() >= lcdDue) {
        for (int n = 0; n < 4; ++n) { lcd.setCursor(0, n); lcd.print(line[n]); }
        lcdDirty = false; lcdDue = millis() + 50;
    }
}
