// LMS Head, v0 (KISS): one classic ESP32 DevKit (38 pins), nothing else between the panel and the host.
// Twelve 10k pots on the ESP's own ADC, four buttons on GPIO, the 20x4 on its I2C backpack. That's it.
// The classic ESP32 has no native USB, so this is MIDI over the USB serial port (115200, plain MIDI
// bytes); the rig finds it by sending PING and hearing HELLO. Wire contract: ../../SPEC.md.
// Wiring: ../../HEAD_V0.md.  The six pots where the spec has encoders stand in for them: three send
// a selection index (AMP / CAB A / CAB B), three send the soft encoders' absolute 14-bit value.
//
// Board: esp32:esp32:esp32 (ESP32 Dev Module).  Library: hd44780 (Bill Perry).
#include <Arduino.h>
#include <Wire.h>
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>

hd44780_I2Cexp lcd;

// ---- the panel ---------------------------------------------------------------------------------
static const int POT_PIN[6]   = {36, 39, 34, 35, 32, 33};    // GAIN BASS MIDDLE TREBLE PRESENCE MASTER (ADC1; VP VN 34 35 32 33)
static const int POT_CC[6]    = {20, 21, 22, 23, 24, 25};    // 14-bit: MSB cc, LSB cc+32
static const int SEL_PIN[3]   = {25, 26, 27};                // AMP, CAB A, CAB B: pots that pick an index (ADC2: fine, no WiFi here)
static const int SOFT_PIN[3]  = {14, 13, 4};                 // soft 1..3: pots, absolute 14-bit (ADC2)
static const int BTN_PIN[4]   = {16, 17, 18, 19};            // soft buttons 1..4 (4 = PAGE), to GND
static const int SDA_PIN = 21, SCL_PIN = 22;

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
uint32_t pageShowUntil = 0;         // while set, lines 3-4 show the page card
uint32_t rigSeen = 0; bool rigLive = false;   // the rig beats once a second; 3 s of silence = waiting
void waiting();
void showPage();

void setLine(int n, const char *s) { snprintf(line[n], 21, "%-20.20s", s); lcdDirty = true; }

// ---- MIDI out, as plain bytes on the USB serial port -------------------------------------------
void cc(uint8_t n, uint8_t v) { uint8_t m[3] = {0xB0, n, v}; Serial.write(m, 3); }
void cc14(uint8_t n, int v) { v = constrain(v, 0, 16383); cc(n, (v >> 7) & 127); cc(n + 32, v & 127); }
void hello()
{
    uint8_t m[] = {0xF0, 0x7D, 0x01, 0x10, 0x01, 'H', 'E', 'A', 'D', 0xF7};   // HELLO, firmware 1, "HEAD"
    sysexOut(m, sizeof m);
}
void sysexOut(const uint8_t *d, int n) { Serial.write(d, n); }

// ---- MIDI in: the host's labels, values, ranges, LED states -----------------------------------
uint8_t sx[64]; int sxn = 0; bool inSx = false;
char label[0x22][21], value[0x22][21];

void onSysex()
{
    // F0 7D <box> <msg> <slot> <data...> F7
    if (sxn < 5 || sx[1] != 0x7D) return;
    uint8_t msg = sx[3], slot = sx[4];
    const uint8_t *data = sx + 5; int n = sxn - 6;
    rigSeen = millis(); if (!rigLive) { rigLive = true; }
    if (msg == 0x12) { hello(); return; }
    if (msg == 0x13) return;        // heartbeat
    if (slot >= 0x22) return;
    if (msg == 0x01 || msg == 0x02) {
        char *dst = msg == 0x01 ? label[slot] : value[slot];
        int k = min(n, 20); memcpy(dst, data, k); dst[k] = 0;
        if (slot >= 0x18 && slot <= 0x1A && !(pageShowUntil && slot == 0x1A)) showSel(slot - 0x18);
        if (msg == 0x01 && ((slot >= 0x08 && slot <= 0x0A) || (slot >= 0x10 && slot <= 0x12))) showPage();
        else if (slot == lastSlot && !pageShowUntil) { char b[21]; snprintf(b, 21, "%-9.9s %10.10s", label[slot], value[slot]); setLine(3, b); }
    } else if (msg == 0x03 && n >= 2 && slot >= 0x18 && slot <= 0x1A) {
        selCount[slot - 0x18] = max(1, (data[0] << 7) | data[1]);
    } else if (msg == 0x04) {
        char b[21]; int k = min(n, 20); memcpy(b, data, k); b[k] = 0; setLine(3, b);
    }
}

void midiIn()
{
    // a byte at a time off the serial port: SysEx is gathered, CCs from the host (pickup targets,
    // LED states) are ignored in v0 -- pots send where they are, and there are no LEDs
    static int ccLeft = 0;
    while (Serial.available()) {
        uint8_t b = (uint8_t)Serial.read();
        if (b == 0xF0) { inSx = true; sxn = 0; }
        if (inSx) {
            if (sxn < 64) sx[sxn++] = b;
            if (b == 0xF7) { inSx = false; onSysex(); }
            continue;
        }
        if (b & 0x80) { ccLeft = (b & 0xF0) == 0xB0 ? 2 : 0; continue; }
        if (ccLeft > 0) --ccLeft;
    }
}

// ---- the panel, read ---------------------------------------------------------------------------
int read14(int pin)
{
    static int filt[40];                                            // per-pin low-pass, indexed by GPIO
    long acc = 0;
    for (int k = 0; k < 8; ++k) acc += analogRead(pin);            // 12-bit x 8 -> 15-bit
    int raw = (int)(acc >> 1);                                      // 14-bit
    if (!filt[pin]) filt[pin] = raw;
    filt[pin] += (raw - filt[pin]) / 4;                             // ~20 ms at 200 Hz; the ESP32 ADC is ~1 % noisy mid-travel
    return filt[pin];
}

void showSlot(int slot)
{
    lastSlot = slot;
    if (pageShowUntil) return;      // the page card has the bottom lines for a moment
    char b[21]; snprintf(b, 21, "%-9.9s %10.10s", label[slot], value[slot]); setLine(3, b);
}
void showSel(int i) { char b[21]; snprintf(b, 21, "%c %-18.18s", "ACB"[i], value[0x18 + i]); setLine(i, b); }
void waiting()                      // nothing on the wire: the idle card, labels back to "--"
{
    rigLive = false; pageShowUntil = 0;
    for (int i = 0; i < 0x22; ++i) { strcpy(label[i], "--"); value[i][0] = 0; }
    setLine(0, "LMS HEAD"); setLine(1, "waiting for the rig"); setLine(2, ""); setLine(3, "");
}
void showPage()                     // PAGE pressed or the rig re-labelled the page: what the 1 2 3 knobs and buttons are now
{
    char b[21];
    snprintf(b, 21, "%-6.6s %-6.6s %-6.6s", label[0x08], label[0x09], label[0x0A]); setLine(2, b);
    snprintf(b, 21, "%-6.6s %-6.6s %-6.6s", label[0x10], label[0x11], label[0x12]); setLine(3, b);
    pageShowUntil = millis() + 3000;
}

void readPots()
{
    for (int i = 0; i < 6; ++i) {
        int v = read14(POT_PIN[i]);
        if (abs(v - potVal[i]) > 48) { potVal[i] = v; cc14(POT_CC[i], v); showSlot(i); }   // ~0.15 % of travel
    }
    for (int i = 0; i < 3; ++i) {
        int v = read14(SOFT_PIN[i]);
        if (abs(v - softVal[i]) > 48) { softVal[i] = v; cc14(26 + i, v); showSlot(0x08 + i); }
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
            if (!down) continue;
            if (i == 3) {                                   // PAGE: first press shows this page's card, a press while it's up advances
                bool up = pageShowUntil != 0;
                showPage();
                if (up) { cc(83, 127); showSlot(0x13); }
            } else { cc(80 + i, 127); showSlot(0x10 + i); }   // "pressed: the new state wanted"
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
    for (int t = 0; t < 5 && lcd.begin(20, 4) != 0; ++t) delay(200);   // the backpack can need a beat after power-up
    lcd.noBacklight();              // DFRobot I2C LCD V1.2: hd44780's polarity autodetect lands backwards; this is ON
    waiting();

    Serial.begin(115200);
    delay(300);
    hello();                        // and again whenever the rig asks (PING)
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
    if (rigLive && millis() - rigSeen > 3000) waiting();
    if (!rigLive) {                                   // the idle card scrolls
        static const char msg[] = "waiting for the rig   *   LMS RIG   *   in your ears   *   ";
        static uint32_t due = 0; static int pos = 0;
        if (millis() >= due) {
            due = millis() + 250;
            const int n = sizeof msg - 1; char b[21];
            for (int k = 0; k < 20; ++k) b[k] = msg[(pos + k) % n];
            b[20] = 0; setLine(2, b);
            pos = (pos + 1) % n;
        }
    }
    if (pageShowUntil && millis() >= pageShowUntil) { pageShowUntil = 0; showSel(2); showSlot(lastSlot); }
    if (lcdDirty && millis() >= lcdDue) {
        for (int n = 0; n < 4; ++n) { lcd.setCursor(0, n); lcd.print(line[n]); }
        lcdDirty = false; lcdDue = millis() + 50;
    }
}
