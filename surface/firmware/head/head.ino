// LMS Head, v0 (KISS): one ESP32-S3-DevKitC-1, nothing else between the panel and the host.
// Pots on the ESP's own ADC, buttons and encoder pushes on two resistor ladders (one ADC pin each),
// encoders on plain GPIO, the 20x4 on its I2C backpack, LED rings on four GPIO. USB-MIDI, class
// compliant, product name "LMS Head". Wire contract: ../../SPEC.md.  Wiring: ../../HEAD_V0.md
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
static const int POT_PIN[6]   = {1, 2, 4, 5, 6, 7};          // GAIN BASS MIDDLE TREBLE PRESENCE MASTER
static const int POT_CC[6]    = {20, 21, 22, 23, 24, 25};    // 14-bit: MSB cc, LSB cc+32
static const int BTN_LADDER   = 8;                           // 4 soft buttons on one ADC pin
static const int PUSH_LADDER  = 9;                           // 6 encoder pushes on one ADC pin
static const int ENC_A[6]     = {10, 12, 14, 16, 18, 38};    // AMP, CAB A, CAB B, soft 1, soft 2, soft 3
static const int ENC_B[6]     = {11, 13, 15, 17, 21, 39};
static const int LED_PIN[4]   = {42, 47, 3, 0};              // LED rings of the soft buttons (3 and 0 are strapping pins: outputs after boot is fine)
static const int SDA_PIN = 40, SCL_PIN = 41;

// ladder levels, millivolts, for a 10k pull-up to 3V3 and switch resistors to GND of
// 0, 1k, 3.3k, 10k (buttons) and 0, 1k, 2.2k, 4.7k, 10k, 22k (pushes). Nothing pressed reads ~3300.
static const int BTN_MV[4]  = {0, 300, 820, 1650};
static const int PUSH_MV[6] = {0, 300, 595, 1055, 1650, 2270};

// ---- state -------------------------------------------------------------------------------------
int      potVal[6];                 // last sent 14-bit value
bool     btnDown[4], pushDown[6];
uint32_t btnT = 0, pushT = 0;       // debounce
int      btnRaw = -1, pushRaw = -1; // what the ladder currently reads (index or -1)
int      encAcc[6];                 // soft encoders: 14-bit accumulators (26..28); selectors: index (102..104)
int      selIndex[3];
int      selCount[3] = {1, 1, 1};   // RANGE from the host
uint8_t  encPrev[6];
int8_t   encQ[6];                   // quarter steps toward a detent
char     line[4][21];
uint32_t lcdDue = 0;
bool     lcdDirty = true;
int      lastSlot = 0;              // the control the panel last touched, for line 4

static const int8_t QUAD[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

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
        } else if (cin == 0x0B && (p.byte1 & 0xF0) == 0xB0) {                     // CC from the host
            uint8_t n = p.byte2, v = p.byte3;
            if (n >= 20 && n <= 25) potVal[n - 20] = (potVal[n - 20] & 127) | (v << 7);      // pickup targets (v0: shown, not enforced)
            else if (n >= 26 && n <= 28) encAcc[n - 26 + 3] = (encAcc[n - 26 + 3] & 127) | (v << 7);
            else if (n >= 58 && n <= 60) encAcc[n - 58 + 3] = (encAcc[n - 58 + 3] & ~127) | v;
            else if (n >= 80 && n <= 83) digitalWrite(LED_PIN[n - 80], v >= 64);
            else if (n >= 102 && n <= 104) selIndex[n - 102] = v;
        }
    }
}

// ---- the panel, read ---------------------------------------------------------------------------
int ladder(int pin, const int *mv, int n)
{
    int x = analogReadMilliVolts(pin);
    if (x > 2900) return -1;                       // nothing pressed
    int best = 0;
    for (int i = 1; i < n; ++i) if (abs(x - mv[i]) < abs(x - mv[best])) best = i;
    return best;
}

void readPots()
{
    for (int i = 0; i < 6; ++i) {
        long acc = 0;
        for (int k = 0; k < 8; ++k) acc += analogRead(POT_PIN[i]);  // 12-bit x 8 -> 15-bit
        int v = (int)(acc >> 1);                                     // 14-bit
        if (abs(v - potVal[i]) > 24) {                               // ~0.15 % of travel
            potVal[i] = v; cc14(POT_CC[i], v);
            lastSlot = i; char b[21]; snprintf(b, 21, "%-9.9s %10.10s", label[i], value[i]); setLine(3, b);
        }
    }
}

void readLadders()
{
    uint32_t now = millis();
    int b = ladder(BTN_LADDER, BTN_MV, 4);
    if (b != btnRaw) { btnRaw = b; btnT = now; }
    else if (now - btnT > 30) for (int i = 0; i < 4; ++i) {
        bool down = (b == i);
        if (down && !btnDown[i]) { cc(80 + i, 127); lastSlot = 0x10 + i; }   // "pressed: the new state wanted"; host answers with the LED state
        btnDown[i] = down;
    }
    int p = ladder(PUSH_LADDER, PUSH_MV, 6);
    if (p != pushRaw) { pushRaw = p; pushT = now; }
    else if (now - pushT > 30) for (int i = 0; i < 6; ++i) {
        bool down = (p == i);
        if (down && !pushDown[i] && i < 3) cc(85 + i, 127);                   // AMP / CAB A / CAB B push
        pushDown[i] = down;
    }
}

void readEncoders()
{
    for (int i = 0; i < 6; ++i) {
        uint8_t cur = (digitalRead(ENC_A[i]) << 1) | digitalRead(ENC_B[i]);
        int8_t d = QUAD[(encPrev[i] << 2) | cur];
        encPrev[i] = cur;
        if (!d) continue;
        encQ[i] += d;
        if (abs(encQ[i]) < 4) continue;                                        // one detent = 4 quarter steps
        int step = encQ[i] > 0 ? 1 : -1; encQ[i] = 0;
        if (i < 3) {                                                           // selectors: an index, wrapping
            selIndex[i] = (selIndex[i] + step + selCount[i]) % selCount[i];
            cc(102 + i, selIndex[i]); lastSlot = 0x18 + i;
        } else {                                                               // soft encoders: absolute 14-bit, the box accumulates
            encAcc[i] = constrain(encAcc[i] + step * 128, 0, 16383);
            cc14(26 + (i - 3), encAcc[i]); lastSlot = 0x08 + (i - 3);
        }
    }
}

void setup()
{
    for (int i = 0; i < 6; ++i) { pinMode(ENC_A[i], INPUT_PULLUP); pinMode(ENC_B[i], INPUT_PULLUP); encPrev[i] = (digitalRead(ENC_A[i]) << 1) | digitalRead(ENC_B[i]); }
    for (int i = 0; i < 4; ++i) { pinMode(LED_PIN[i], OUTPUT); digitalWrite(LED_PIN[i], LOW); }
    analogReadResolution(12);
    for (int i = 0; i < 6; ++i) potVal[i] = -1000;
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
    readEncoders();                 // every pass: the encoders must not miss a quarter step
    static uint32_t slow = 0;
    if (millis() - slow >= 5) {     // 200 Hz for the rest
        slow = millis();
        readPots();
        readLadders();
    }
    if (lcdDirty && millis() >= lcdDue) {
        for (int n = 0; n < 4; ++n) { lcd.setCursor(0, n); lcd.print(line[n]); }
        lcdDirty = false; lcdDue = millis() + 50;
    }
}
