// LMS Head, v0 (KISS): one classic ESP32 DevKit (38 pins), nothing else between the panel and the host.
// Twelve 10k pots on the ESP's own ADC, four buttons on GPIO, the 20x4 on its I2C backpack. That's it.
// Two transports, both always live: plain MIDI bytes on the USB serial port (115200; the rig finds it
// by sending PING and hearing HELLO), and BLE-MIDI as "LMS Head" (BlueZ turns a paired BLE-MIDI device
// into an ALSA MIDI port with that name, which the rig already looks for). The box answers on whichever
// transport the rig was last heard on, so a box on both never doubles a button press.
// WiFi is out on purpose: it takes ADC2, and six pots are on ADC2. Bluetooth doesn't. Wire contract: ../../SPEC.md.
// Wiring: ../../HEAD_V0.md.  The six pots where the spec has encoders stand in for them: three send
// a selection index (AMP / CAB A / CAB B), three send the soft encoders' absolute 14-bit value.
//
// Board: esp32:esp32:esp32 (ESP32 Dev Module).  Library: hd44780 (Bill Perry).
#include <Arduino.h>
#include <Wire.h>
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <BLESecurity.h>   // BLESecurityCallbacks only
#include <esp_gap_ble_api.h>
#include <Preferences.h>

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

// ---- BLE-MIDI -----------------------------------------------------------------------------------
// The standard MIDI-over-BLE service. Every packet: a header byte (timestamp high bits), then each
// message preceded by a timestamp low byte. Incoming packets are unwrapped back into plain MIDI bytes
// and fed to the same parser as the serial port, from loop(), never from the BLE task.
#define BLE_MIDI_SERVICE "03b80e5a-ede8-4b33-a751-6ce34ec4c700"
#define BLE_MIDI_CHAR    "7772e5db-3868-4112-a1a9-f2669d106bf3"
BLECharacteristic *bleChar = nullptr;
volatile bool bleConnected = false;
volatile bool bleRestartAdvertising = false;
uint16_t bleMtuPayload = 20;

enum Via { viaNone, viaSerial, viaBle };
Via rigVia = viaNone;               // where the rig was last heard: replies go there

static uint8_t bleRx[1024];         // BLE task -> loop(), single producer / single consumer
static volatile uint16_t bleRxHead = 0, bleRxTail = 0;
static void bleRxPut(uint8_t b) { uint16_t h = bleRxHead, n = (h + 1) % sizeof bleRx; if (n != bleRxTail) { bleRx[h] = b; bleRxHead = n; } }

volatile bool bleIconDirty = true;  // the LCD's Bluetooth icon: a host is connected (rig or not)

struct ServerCallbacks : BLEServerCallbacks {
    void onConnect(BLEServer *s) override { bleConnected = true; bleIconDirty = true; log_i("BLE connected"); }
    void onDisconnect(BLEServer *s) override { bleConnected = false; bleIconDirty = true; bleRestartAdvertising = true; log_i("BLE disconnected"); }
};

// Just Works bonding: no screen to show a PIN on
struct SecurityCallbacks : BLESecurityCallbacks {
    uint32_t onPassKeyRequest() override { return 0; }
    void onPassKeyNotify(uint32_t) override {}
    bool onSecurityRequest() override { return true; }
    bool onConfirmPIN(uint32_t) override { return true; }
    void onAuthenticationComplete(esp_ble_auth_cmpl_t a) override
    {
        if (a.success) log_i("BLE bonded");
        else log_w("BLE pairing failed, reason 0x%x", a.fail_reason);
    }
};

// forget every host the box was paired with: stale keys on one side make pairing hang
void forgetHosts()
{
    int n = esp_ble_get_bond_device_num();
    if (n <= 0) return;
    esp_ble_bond_dev_t *list = (esp_ble_bond_dev_t *)malloc(sizeof(esp_ble_bond_dev_t) * n);
    if (!list) return;
    esp_ble_get_bond_device_list(&n, list);
    for (int i = 0; i < n; ++i) esp_ble_remove_bond_device(list[i].bd_addr);
    free(list);
    log_i("forgot %d paired host(s)", n);
}

struct MidiCallbacks : BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *c) override
    {
        // unwrap: drop the header and every timestamp byte, keep statuses and data
        const uint8_t *p = c->getData(); size_t n = c->getLength();
        log_d("BLE rx %u bytes", (unsigned)n);
        if (n < 2 || !(p[0] & 0x80)) return;
        size_t i = 1;
        while (i < n) {
            uint8_t b = p[i++];
            if (b & 0x80) {                                    // a timestamp; a status may follow
                if (i < n && (p[i] & 0x80)) bleRxPut(p[i++]);
            } else bleRxPut(b);                                // data (running status, or SysEx continuing)
        }
    }
};

void bleSend(const uint8_t *m, int n)
{
    // one MIDI message per packet: header, timestamp, the message; SysEx gets a timestamp before F7
    if (!bleConnected || !bleChar) return;
    uint16_t t = millis() & 0x1FFF;
    uint8_t hdr = 0x80 | ((t >> 7) & 0x3F), ts = 0x80 | (t & 0x7F);
    uint8_t pkt[64]; int k = 0;
    pkt[k++] = hdr; pkt[k++] = ts;
    for (int i = 0; i < n && k < (int)sizeof pkt - 1; ++i) {
        if (m[i] == 0xF7 && i > 0) pkt[k++] = ts;
        pkt[k++] = m[i];
    }
    if (k > bleMtuPayload) k = bleMtuPayload;              // our messages fit 20 bytes; never send a broken one bigger
    bleChar->setValue(pkt, k);
    bleChar->notify();
}

void bleBegin(bool forget)
{
    BLEDevice::init("LMS Head");
    // Bond when the host pairs, but never start pairing from here: BlueZ wants an agent to approve
    // device-initiated pairing and cancels without one. (BLESecurity would force it on every connect,
    // so the SMP parameters are set on the IDF layer directly.) The box keeps the keys, so after a
    // reboot the host's stored keys still match and it reconnects on its own.
    BLEDevice::setSecurityCallbacks(new SecurityCallbacks());
    uint8_t authReq = ESP_LE_AUTH_BOND, iocap = ESP_IO_CAP_NONE, keySize = 16;
    uint8_t keys = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &authReq, 1);
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, 1);
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &keySize, 1);
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &keys, 1);
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &keys, 1);

    // hold PAGE at power-up to forget paired hosts; a new pairing scheme does it once on its own
    Preferences prefs;
    prefs.begin("lmshead", false);
    if (forget || prefs.getUChar("bondScheme", 0) != 3) { forgetHosts(); prefs.putUChar("bondScheme", 3); }
    prefs.end();

    BLEServer *server = BLEDevice::createServer();
    server->setCallbacks(new ServerCallbacks());
    BLEService *svc = server->createService(BLE_MIDI_SERVICE);
    bleChar = svc->createCharacteristic(BLE_MIDI_CHAR,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_NOTIFY);
    bleChar->addDescriptor(new BLE2902());
    bleChar->setCallbacks(new MidiCallbacks());
    uint8_t empty[2] = {0x80, 0x80};
    bleChar->setValue(empty, 0);                            // a read returns no payload, per the spec
    svc->start();
    BLEAdvertising *adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(BLE_MIDI_SERVICE);
    adv->setScanResponse(true);
    BLEDevice::startAdvertising();
}

// ---- MIDI out: to the transport the rig was last heard on (both until it's heard) ----------------
void midiOut(const uint8_t *m, int n)
{
    // Bluetooth only once the rig has spoken on it (HELLO is the exception, see hello()): an idle
    // link flooded with pot jitter starves pairing and drops the connection
    if (rigVia != viaBle) Serial.write(m, n);
    if (rigVia == viaBle) bleSend(m, n);
}
void cc(uint8_t n, uint8_t v) { uint8_t m[3] = {0xB0, n, v}; midiOut(m, 3); }
void cc14(uint8_t n, int v) { v = constrain(v, 0, 16383); cc(n, (v >> 7) & 127); cc(n + 32, v & 127); }
void hello()
{
    uint8_t m[] = {0xF0, 0x7D, 0x01, 0x10, 0x01, 'H', 'E', 'A', 'D', 0xF7};   // HELLO, firmware 1, "HEAD"
    sysexOut(m, sizeof m);
    if (rigVia == viaNone) bleSend(m, sizeof m);    // announce on both until the rig picks one
}
void sysexOut(const uint8_t *d, int n) { midiOut(d, n); }

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

struct Parser {
    // a byte at a time: SysEx is gathered, CCs from the host (pickup targets, LED states) are
    // ignored in v0 -- pots send where they are, and there are no LEDs. One per transport, so a
    // message split across BLE packets never mixes with serial bytes.
    uint8_t buf[64]; int n = 0; bool inSx = false; int ccLeft = 0;
    void feed(uint8_t b, Via via)
    {
        if (b == 0xF0) { inSx = true; n = 0; }
        if (inSx) {
            if (n < 64) buf[n++] = b;
            if (b == 0xF7) {
                inSx = false;
                rigVia = via;                                  // the rig is talking on this one
                memcpy(sx, buf, n); sxn = n; onSysex();
            }
            return;
        }
        if (b & 0x80) { ccLeft = (b & 0xF0) == 0xB0 ? 2 : 0; return; }
        if (ccLeft > 0) --ccLeft;
    }
};
Parser serialParser, bleParser;

void midiIn()
{
    while (Serial.available()) serialParser.feed((uint8_t)Serial.read(), viaSerial);
    while (bleRxTail != bleRxHead) { uint8_t b = bleRx[bleRxTail]; bleRxTail = (bleRxTail + 1) % sizeof bleRx; bleParser.feed(b, viaBle); }
    if (bleRestartAdvertising) {                      // a dropped link: be findable again, and stop replying on it
        bleRestartAdvertising = false;
        if (rigVia == viaBle) rigVia = viaNone;
        delay(100); BLEDevice::startAdvertising();
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
void showSel(int i) { char b[21]; snprintf(b, 21, "%c %-17.17s", "ACB"[i], value[0x18 + i]); setLine(i, b); }   // col 19 of line 0: the BT icon
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
    static uint8_t btGlyph[8] = {0b00100, 0b10110, 0b01101, 0b00110, 0b01101, 0b10110, 0b00100, 0b00000};
    lcd.createChar(0, btGlyph);
    waiting();

    Serial.begin(115200);
    const bool forget = digitalRead(BTN_PIN[3]) == LOW;    // PAGE held at power-up
    bleBegin(forget);
    if (forget) { setLine(3, "forgot paired hosts"); }
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
    if (rigLive && millis() - rigSeen > 3000) { waiting(); rigVia = viaNone; }
    if (!rigLive) {                                   // the idle card: each message scrolls through once, 15 s of quiet, the other
        static const char *msgs[] = {"it's in your ears...", "NOTICE: THEY ARE TRYING TO STEAL YOUR COMPUTER. INSTALL LINUX NOW."};
        static uint32_t due = 0; static int which = 0, pos = 0;
        if (millis() >= due) {
            const char *m = msgs[which]; const int n = strlen(m);
            if (pos <= n + 20) {
                char b[21];
                for (int k = 0; k < 20; ++k) { int c = pos - 20 + k; b[k] = (c >= 0 && c < n) ? m[c] : ' '; }
                b[20] = 0; setLine(2, b);
                ++pos; due = millis() + 200;
            } else { pos = 0; which ^= 1; due = millis() + 15000; }
        }
    }
    if (pageShowUntil && millis() >= pageShowUntil) { pageShowUntil = 0; showSel(2); showSlot(lastSlot); }
    if (lcdDirty && millis() >= lcdDue) {
        for (int n = 0; n < 4; ++n) { lcd.setCursor(0, n); lcd.print(line[n]); }
        lcdDirty = false; lcdDue = millis() + 50; bleIconDirty = true;
    }
    if (bleIconDirty) {                                 // a Bluetooth host is connected: the rune, top right
        bleIconDirty = false;
        lcd.setCursor(19, 0);
        if (bleConnected) lcd.write((uint8_t)0); else lcd.print(line[0][19]);
    }
}
