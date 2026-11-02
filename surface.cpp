#include "surface.h"
#include "rig.h"
#include "parameter.h"
#include "ysfx.h"
#if !JUCE_WINDOWS
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#endif

// The serial box, per platform. POSIX: a raw 115200 tty. Windows: no serial
// (the S3 box's "LMS Head" MIDI port still works), so the port never opens.
#if JUCE_WINDOWS
static int ttyOpen(const char *) { return -1; }
static long ttyRead(int, void *, size_t) { return -1; }
static void ttyWrite(int, const void *, size_t) {}
static void ttyClose(int) {}
#else
static int ttyOpen(const char *path)
{
    int fd = ::open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    termios tio{};
    if (tcgetattr(fd, &tio) == 0) {
        cfmakeraw(&tio);
        cfsetispeed(&tio, B115200); cfsetospeed(&tio, B115200);
        tio.c_cflag |= CLOCAL | CREAD; tio.c_cflag &= ~CRTSCTS;
        tcsetattr(fd, TCSANOW, &tio);
    }
    tcflush(fd, TCIOFLUSH);
    return fd;
}
static long ttyRead(int fd, void *buf, size_t n) { return (long)::read(fd, buf, n); }
static void ttyWrite(int fd, const void *buf, size_t n) { (void)!::write(fd, buf, n); }
static void ttyClose(int fd) { ::close(fd); }
#endif

static const char *const kFixedNames[6] = {"Gain|Pre Gain", "Bass|Low", "Mid|Middle|Tone", "Treble|High", "Presence|Tone Cut|Resonance", "@master"};
static const juce::String kUnsent = juce::String::charToString(1);   // "never sent": not a value a slot can show
static const char *const kFixedLabels[6] = {"GAIN", "BASS", "MIDDLE", "TREBLE", "PRESENCE", "MASTER"};

static juce::String cleanName(const juce::String &raw)
{
    // "-Pre Gain (%)" -> "Pre Gain"
    juce::String n = raw.trimCharactersAtStart("-").trim();
    int paren = n.lastIndexOf(" (");
    if (paren > 0 && n.endsWithChar(')'))
        n = n.substring(0, paren);
    return n.trim();
}

ControlSurface::ControlSurface(RigProcessor &rig) : m_rig(rig)
{
    startTimerHz(30);
}

ControlSurface::~ControlSurface()
{
    stopTimer();
    if (m_in)
        m_in->stop();
    closeSerial();
}

//==============================================================================
// the serial box: every USB serial port gets a PING; the one that says HELLO is the head.
// (opening the port resets a classic ESP32 devkit, which then says HELLO on its own too)
void ControlSurface::closeSerial()
{
    if (m_tty >= 0) ttyClose(m_tty);
    m_tty = -1; m_ttyHello = false; m_ttyPath.clear(); m_inSx = false; m_sx.clear(); m_ccState = 0;
}

void ControlSurface::scanSerial()
{
    const juce::uint32 now = juce::Time::getMillisecondCounter();
    if (m_tty >= 0) {
        if (!juce::File(m_ttyPath).exists()) { closeSerial(); m_sentCC.clear(); m_sentLabel.fill(kUnsent); m_sentValue.fill(kUnsent); m_sentRange.fill(-1); return; }
        if (!m_ttyHello && now - m_ttyOpenedAt > 4000) {       // no answer: not ours, leave it alone for a while
            m_ttyTried[m_ttyPath] = now; closeSerial();
        }
        return;
    }
    juce::StringArray candidates;
    auto env = juce::SystemStats::getEnvironmentVariable("LMS_HEAD_TTY", {});
    if (env.isNotEmpty()) candidates.add(env);
    for (auto &f : juce::File("/dev").findChildFiles(juce::File::findFiles, false, "ttyUSB*;ttyACM*"))
        candidates.add(f.getFullPathName());
    for (auto &path : candidates) {
        auto t = m_ttyTried.find(path);
        if (t != m_ttyTried.end() && now - t->second < 15000) continue;
        int fd = ttyOpen(path.toRawUTF8());
        if (fd < 0) { m_ttyTried[path] = now; continue; }
        m_tty = fd; m_ttyPath = path; m_ttyHello = false; m_ttyOpenedAt = now;
        const uint8_t ping[] = {0xF0, 0x7D, 0x01, 0x12, 0x00, 0xF7};
        ttyWrite(fd, ping, sizeof ping);
        break;
    }
}

void ControlSurface::readSerial()
{
    if (m_tty < 0) return;
    uint8_t buf[256];
    for (;;) {
        long n = ttyRead(m_tty, buf, sizeof buf);
        if (n <= 0) break;
        serialBytes(buf, (int)n);
    }
    // a box that reset (opening the port does that) says HELLO late: ping again until it does
    const juce::uint32 now = juce::Time::getMillisecondCounter();
    if (!m_ttyHello && (now - m_ttyOpenedAt) % 1000 < 40) {
        const uint8_t ping[] = {0xF0, 0x7D, 0x01, 0x12, 0x00, 0xF7};
        ttyWrite(m_tty, ping, sizeof ping);
    }
}

void ControlSurface::serialBytes(const uint8_t *d, int n)
{
    for (int i = 0; i < n; ++i) {
        uint8_t b = d[i];
        if (b == 0xF0) { m_inSx = true; m_sx.clear(); }
        if (m_inSx) {
            if (m_sx.size() < 64) m_sx.push_back(b);
            if (b == 0xF7) {
                m_inSx = false;
                if (m_sx.size() >= 5 && m_sx[1] == 0x7D && m_sx[3] == 0x10) {     // HELLO
                    m_ttyHello = true;
                    m_sentCC.clear(); m_sentLabel.fill(kUnsent); m_sentValue.fill(kUnsent); m_sentRange.fill(-1);
                    refresh(true);
                }
            }
            continue;
        }
        if (b & 0x80) { m_ccState = (b & 0xF0) == 0xB0 ? 1 : 0; continue; }
        if (m_ccState == 1) { m_ccNum = b; m_ccState = 2; }
        else if (m_ccState == 2) { handle({0xB0, m_ccNum, b}); m_ccState = 1; }   // running status: the next pair is another CC
    }
}

//==============================================================================
// ports: the box by its USB name, looked for once a second until it is there
void ControlSurface::scanPorts()
{
    if (!m_in) {
        for (auto &d : juce::MidiInput::getAvailableDevices())
            if (d.name.containsIgnoreCase("LMS Head")) {
                m_in = juce::MidiInput::openDevice(d.identifier, this);
                if (m_in) m_in->start();
                break;
            }
    }
    if (!m_out) {
        for (auto &d : juce::MidiOutput::getAvailableDevices())
            if (d.name.containsIgnoreCase("LMS Head")) {
                m_out = juce::MidiOutput::openDevice(d.identifier);
                if (m_out) refresh(true);
                break;
            }
    }
    // a box that went away: forget it, so a replug starts clean
    if (m_in) {
        bool present = false;
        for (auto &d : juce::MidiInput::getAvailableDevices()) present |= d.identifier == m_in->getIdentifier();
        if (!present) { m_in->stop(); m_in.reset(); m_out.reset(); m_sentCC.clear(); m_sentLabel.fill(kUnsent); m_sentValue.fill(kUnsent); m_sentRange.fill(-1); }
    }
}

void ControlSurface::handleIncomingMidiMessage(juce::MidiInput *, const juce::MidiMessage &m)
{
    // MIDI thread: queue, the timer applies
    if (m.isController()) {
        if (m_fifo.getFreeSpace() < 1) return;
        int start, n1, start2, n2;
        m_fifo.prepareToWrite(1, start, n1, start2, n2);
        m_events[(size_t)start] = {(uint8_t)m.getRawData()[0], (uint8_t)m.getControllerNumber(), (uint8_t)m.getControllerValue()};
        m_fifo.finishedWrite(1);
    } else if (m.isSysEx()) {
        const uint8_t *d = m.getSysExData(); int n = m.getSysExDataSize();
        if (n >= 3 && d[0] == 0x7D && d[2] == 0x10) {   // HELLO: the box (re)started, tell it everything
            if (m_fifo.getFreeSpace() < 1) return;
            int start, n1, start2, n2;
            m_fifo.prepareToWrite(1, start, n1, start2, n2);
            m_events[(size_t)start] = {0xF0, d[1], 0};
            m_fifo.finishedWrite(1);
        }
    }
}

//==============================================================================
// The Bluetooth box: BlueZ keeps the bond but doesn't reconnect a BLE-MIDI device on its own, so
// while no box is connected the rig asks it to, every 15 s, in the background. A paired "LMS Head"
// that's switched off costs a short scan each time; with nothing paired it's one bluetoothctl call.
void ControlSurface::reconnectBluetooth()
{
#if JUCE_LINUX
    if (connected()) return;
    if (m_btHelper && m_btHelper->isRunning()) return;
    const juce::uint32 now = juce::Time::getMillisecondCounter();
    if (m_btLastTry != 0 && now - m_btLastTry < 15000) return;
    m_btLastTry = now;
    if (!juce::File("/usr/bin/bluetoothctl").existsAsFile()) return;
    static const char *script =
        "for a in $(bluetoothctl devices Paired 2>/dev/null | awk '/LMS Head/ {print $2}'); do "
        "  bluetoothctl --timeout 6 scan le >/dev/null 2>&1 & sleep 4; "
        "  bluetoothctl --timeout 8 connect \"$a\" >/dev/null 2>&1; wait; "
        "done";
    m_btHelper = std::make_unique<juce::ChildProcess>();
    if (!m_btHelper->start(juce::StringArray{"/bin/sh", "-c", script}, 0))
        m_btHelper.reset();
#endif
}

void ControlSurface::timerCallback()
{
    if (++m_scanTick % 30 == 0) { scanPorts(); scanSerial(); reconnectBluetooth(); }
    readSerial();

    // a head swap or a channel change re-reads the head
    auto info = m_rig.amp().getCurrentInfo();
    if (info != m_info) { m_info = info; rebuild(); }
    if (m_channel >= 0) {
        if (auto *p = ampParam(m_channel)) {
            int v = (int)std::lround(p->convertToYsfxValue(p->getValue()));
            if (v != m_lastChannelValue) { m_lastChannelValue = v; rebuild(); recallGain(); }
        }
    }

    // the box's moves
    int n = m_fifo.getNumReady();
    while (n-- > 0) {
        int start, n1, start2, n2;
        m_fifo.prepareToRead(1, start, n1, start2, n2);
        Event e = m_events[(size_t)start];
        m_fifo.finishedRead(1);
        handle(e);
    }

    if (m_out || (m_tty >= 0 && m_ttyHello))
        refresh(false);

    // heartbeat: the box shows "waiting for the rig" when these stop
    if (m_tty >= 0 && m_ttyHello && m_scanTick % 30 == 15) {
        const uint8_t beat[] = {0xF0, 0x7D, 0x01, 0x13, 0x00, 0xF7};
        ttyWrite(m_tty, beat, sizeof beat);
    }
}

void ControlSurface::handle(const Event &e)
{
    if (e.status == 0xF0) { m_sentCC.clear(); m_sentLabel.fill(kUnsent); m_sentValue.fill(kUnsent); m_sentRange.fill(-1); refresh(true); return; }
    const int cc = e.d1, v = e.d2;
    if ((cc >= 20 && cc <= 28)) { m_msb[cc] = v; onPot(cc, v << 7); }               // MSB: apply at once...
    else if (cc >= 52 && cc <= 60) onPot(cc - 32, (m_msb[cc - 32] << 7) | v);        // ...LSB refines
    else if (cc >= 80 && cc <= 83) { if (v >= 64) onButton(cc - 80); }
    else if (cc >= 102 && cc <= 104) onSelect(cc - 102, v);
}

//==============================================================================
// the head's sliders, by role
YsfxParameter *ControlSurface::ampParam(int sliderIndex) const
{
    return sliderIndex < 0 ? nullptr : m_rig.amp().getYsfxParameter(sliderIndex);
}

int ControlSurface::findSlider(const juce::String &alternatives) const
{
    if (!m_info || !m_info->effect) return -1;
    ysfx_t *fx = m_info->effect.get();
    for (auto alt : juce::StringArray::fromTokens(alternatives, "|", {})) {
        for (uint32_t i = 0; i < ysfx_max_sliders; ++i)
            if (ysfx_slider_exists(fx, i) && cleanName(juce::CharPointer_UTF8(ysfx_slider_get_name(fx, i))).equalsIgnoreCase(alt.trim()))
                return (int)i;
    }
    return -1;
}

juce::String ControlSurface::channelName() const
{
    auto *p = ampParam(m_channel);
    if (!p || !p->isEnumSlider()) return {};
    int v = (int)std::lround(p->convertToYsfxValue(p->getValue()));
    return v >= 0 && v < p->getSliderEnumSize() ? juce::String(p->getSliderEnumName(v)) : juce::String();
}

int ControlSurface::masterFor(const juce::String &channel) const
{
    int i = channel.isNotEmpty() ? findSlider(channel + " Master") : -1;
    if (i < 0) i = findSlider("Master|Output Level|Volume");
    return i;
}

void ControlSurface::rebuild()
{
    m_channel = findSlider("Channel");
    const juce::String ch = channelName();
    for (int k = 0; k < 5; ++k)
        m_fixed[k] = findSlider(kFixedNames[k]);
    m_fixed[5] = masterFor(ch);

    // everything else: continuous sliders to the soft pots, switches and enums to the buttons, Channel first
    m_softKnobs.clear(); m_softSwitches.clear();
    if (!m_info || !m_info->effect) return;
    ysfx_t *fx = m_info->effect.get();
    std::vector<int> taken(m_fixed, m_fixed + 6);
    for (int k = 0; k < numCabKeys; ++k)
        if (auto *p = m_rig.ampCabParam((CabKey)k)) taken.push_back(p->getSliderIndex());
    // every channel's master is spoken for by the MASTER pot
    if (auto *p = ampParam(m_channel))
        for (int v = 0; v < p->getSliderEnumSize(); ++v) taken.push_back(findSlider(juce::String(p->getSliderEnumName(v)) + " Master"));
    if (m_channel >= 0) m_softSwitches.push_back(m_channel);
    for (uint32_t i = 0; i < ysfx_max_sliders; ++i) {
        if (!ysfx_slider_exists(fx, i) || ysfx_slider_is_path(fx, i)) continue;   // (LMS sliders all start hidden: they have their own @gfx)
        if (std::find(taken.begin(), taken.end(), (int)i) != taken.end() || (int)i == m_channel) continue;
        auto *p = ampParam((int)i);
        if (!p) continue;
        if (p->isEnumSlider() || p->getSliderEnumSize() == 2) m_softSwitches.push_back((int)i);
        else {
            ysfx_slider_range_t r = p->getSliderRange();
            bool isSwitch = r.min == 0 && r.max == 1 && r.inc == 1;
            (isSwitch ? m_softSwitches : m_softKnobs).push_back((int)i);
        }
    }
    const int pages = juce::jmax(1, (int)std::ceil(juce::jmax(m_softKnobs.size(), m_softSwitches.size()) / 3.0));
    m_page = juce::jlimit(0, pages - 1, m_page);
    m_sentLabel.fill(kUnsent); m_sentValue.fill(kUnsent); m_sentRange.fill(-1);
}

// GAIN per channel: the head has one gain, the surface remembers one per channel
void ControlSurface::rememberGain()
{
    auto *g = ampParam(m_fixed[0]);
    if (!g) return;
    m_gainMemory[m_rig.ampFileName()][channelName()] = g->convertToYsfxValue(g->getValue());
}

void ControlSurface::recallGain()
{
    auto *g = ampParam(m_fixed[0]);
    if (!g) return;
    auto head = m_gainMemory.find(m_rig.ampFileName());
    if (head == m_gainMemory.end()) return;
    auto it = head->second.find(channelName());
    if (it == head->second.end()) return;
    float norm = g->convertFromYsfxValue(it->second);
    if (std::abs(norm - g->getValue()) > 1e-4f)
        g->setValueNotifyingHost(norm);
}

//==============================================================================
// the box's moves
void ControlSurface::onPot(int cc, int v14)
{
    const float norm = juce::jlimit(0.0f, 1.0f, v14 / 16383.0f);
    int slider = -1;
    if (cc >= 20 && cc <= 25) slider = m_fixed[cc - 20];
    else if (cc >= 26 && cc <= 28) { size_t k = (size_t)(m_page * 3 + (cc - 26)); if (k < m_softKnobs.size()) slider = m_softKnobs[k]; }
    if (auto *p = ampParam(slider)) {
        p->setValueNotifyingHost(norm);
        if (cc == 20) rememberGain();
    }
}

void ControlSurface::onButton(int i)
{
    if (i == 3) {   // PAGE
        const int pages = juce::jmax(1, (int)std::ceil(juce::jmax(m_softKnobs.size(), m_softSwitches.size()) / 3.0));
        m_page = (m_page + 1) % pages;
        m_sentLabel.fill(kUnsent); m_sentValue.fill(kUnsent);
        return;
    }
    size_t k = (size_t)(m_page * 3 + i);
    if (k >= m_softSwitches.size()) return;
    auto *p = ampParam(m_softSwitches[k]);
    if (!p) return;
    const int steps = juce::jmax(2, p->getSliderEnumSize() > 0 ? p->getSliderEnumSize() : 2);
    int v = (int)std::lround(p->convertToYsfxValue(p->getValue()));
    v = (v + 1) % steps;                                       // a press cycles: a switch toggles, an enum steps
    if (m_softSwitches[k] == m_channel) rememberGain();       // leaving a channel: keep its gain
    p->setValueNotifyingHost(p->convertFromYsfxValue(v));
}

void ControlSurface::onSelect(int which, int index)
{
    if (which == 0) { if (index < m_rig.ampNames().size()) m_rig.selectAmp(index); return; }
    CabKey key = which == 1 ? cabType : cabTypeB;
    if (auto *p = m_rig.ampCabParam(key))
        if (index < juce::jmax(1, p->getSliderEnumSize()))
            m_rig.setCabValue(key, index);
}

//==============================================================================
// what the box shows
void ControlSurface::sendCC(int cc, int value)
{
    auto it = m_sentCC.find(cc);
    if (it != m_sentCC.end() && it->second == value) return;
    m_sentCC[cc] = value;
    if (m_out) m_out->sendMessageNow(juce::MidiMessage::controllerEvent(1, cc, value));
    if (m_tty >= 0 && m_ttyHello) { const uint8_t m[3] = {0xB0, (uint8_t)cc, (uint8_t)value}; ttyWrite(m_tty, m, 3); }
}

void ControlSurface::send14(int cc, int v14)
{
    v14 = juce::jlimit(0, 16383, v14);
    sendCC(cc, v14 >> 7);
    sendCC(cc + 32, v14 & 127);
}

void ControlSurface::sendSysex(int msg, int slot, const juce::String &data)
{
    juce::String d = data.substring(0, 20);
    if (msg == 1) { if (m_sentLabel[(size_t)slot] == d) return; m_sentLabel[(size_t)slot] = d; }
    if (msg == 2) { if (m_sentValue[(size_t)slot] == d) return; m_sentValue[(size_t)slot] = d; }
    juce::MemoryBlock b;
    const uint8_t head[4] = {0x7D, 0x01, (uint8_t)msg, (uint8_t)slot};
    b.append(head, 4);
    for (auto c : d.toStdString()) { uint8_t x = (uint8_t)c; if (x < 0x20 || x > 0x7E) x = '?'; b.append(&x, 1); }
    if (m_out) m_out->sendMessageNow(juce::MidiMessage::createSysExMessage(b.getData(), (int)b.getSize()));
    if (m_tty >= 0 && m_ttyHello) {
        const uint8_t f0 = 0xF0, f7 = 0xF7;
        ttyWrite(m_tty, &f0, 1); ttyWrite(m_tty, b.getData(), b.getSize()); ttyWrite(m_tty, &f7, 1);
    }
}

void ControlSurface::rawSysex(const juce::MemoryBlock &b)
{
    if (m_out) m_out->sendMessageNow(juce::MidiMessage::createSysExMessage(b.getData(), (int)b.getSize()));
    if (m_tty >= 0 && m_ttyHello) {
        const uint8_t f0 = 0xF0, f7 = 0xF7;
        ttyWrite(m_tty, &f0, 1); ttyWrite(m_tty, b.getData(), b.getSize()); ttyWrite(m_tty, &f7, 1);
    }
}

juce::String ControlSurface::labelOf(int sliderIndex) const
{
    auto *p = ampParam(sliderIndex);
    return p ? cleanName(juce::String(p->getSliderName())) : juce::String("--");
}

juce::String ControlSurface::valueOf(int sliderIndex) const
{
    auto *p = ampParam(sliderIndex);
    return p ? p->getText(p->getValue(), 20) : juce::String();
}

void ControlSurface::refresh(bool everything)
{
    if (!m_out && !(m_tty >= 0 && m_ttyHello)) return;
    if (everything) { m_sentCC.clear(); m_sentLabel.fill(kUnsent); m_sentValue.fill(kUnsent); m_sentRange.fill(-1); }

    // the fixed pots: the bound slider's name on this head, its value, and the value as a pickup target
    for (int k = 0; k < 6; ++k) {
        sendSysex(1, slotPots + k, m_fixed[k] >= 0 ? labelOf(m_fixed[k]) : juce::String("--"));
        sendSysex(2, slotPots + k, valueOf(m_fixed[k]));
        if (auto *p = ampParam(m_fixed[k])) send14(20 + k, (int)std::lround(p->getValue() * 16383));
    }
    // the soft pots and buttons of this page
    for (int k = 0; k < 3; ++k) {
        size_t i = (size_t)(m_page * 3 + k);
        int s = i < m_softKnobs.size() ? m_softKnobs[i] : -1;
        sendSysex(1, slotSoft + k, s >= 0 ? labelOf(s) : juce::String("--"));
        sendSysex(2, slotSoft + k, valueOf(s));
        if (auto *p = ampParam(s)) send14(26 + k, (int)std::lround(p->getValue() * 16383));
        int b = i < m_softSwitches.size() ? m_softSwitches[i] : -1;
        sendSysex(1, slotButtons + k, b >= 0 ? labelOf(b) : juce::String("--"));
        sendSysex(2, slotButtons + k, valueOf(b));
        if (auto *p = ampParam(b)) sendCC(80 + k, p->getValue() > 0.5f ? 127 : 0);
    }
    sendSysex(1, slotButtons + 3, "PAGE");
    // the selections
    const int ampCount = juce::jmax(1, m_rig.ampNames().size());
    sendSysex(1, slotAmp, "AMP");
    sendSysex(2, slotAmp, m_rig.ampIndex() >= 0 ? m_rig.ampNames()[m_rig.ampIndex()] : juce::String("--"));
    if (m_sentRange[0] != ampCount) {
        m_sentRange[0] = ampCount;
        juce::MemoryBlock b; const uint8_t head[4] = {0x7D, 0x01, 0x03, (uint8_t)slotAmp}; b.append(head, 4);
        uint8_t r[2] = {(uint8_t)(ampCount >> 7), (uint8_t)(ampCount & 127)}; b.append(r, 2);
        rawSysex(b);
    }
    sendCC(102, juce::jmax(0, m_rig.ampIndex()));
    for (int c = 0; c < 2; ++c) {
        CabKey key = c == 0 ? cabType : cabTypeB;
        auto *p = m_rig.ampCabParam(key);
        const int count = p ? juce::jmax(1, p->getSliderEnumSize()) : 1;
        sendSysex(1, slotCabA + c, c == 0 ? "CAB A" : "CAB B");
        sendSysex(2, slotCabA + c, p ? valueOf(p->getSliderIndex()) : juce::String("--"));
        if (m_sentRange[(size_t)c + 1] != count) {
            m_sentRange[(size_t)c + 1] = count;
            juce::MemoryBlock b; const uint8_t head[4] = {0x7D, 0x01, 0x03, (uint8_t)(slotCabA + c)}; b.append(head, 4);
            uint8_t r[2] = {(uint8_t)(count >> 7), (uint8_t)(count & 127)}; b.append(r, 2);
            rawSysex(b);
        }
        sendCC(103 + c, p ? (int)std::lround(p->convertToYsfxValue(p->getValue())) : 0);
    }
    const int pages = juce::jmax(1, (int)std::ceil(juce::jmax(m_softKnobs.size(), m_softSwitches.size()) / 3.0));
    sendSysex(2, slotPage, juce::String(m_page + 1) + "/" + juce::String(pages));
    sendSysex(2, slotChannel, channelName());
}

//==============================================================================
juce::ValueTree ControlSurface::getState() const
{
    juce::ValueTree t("surface");
    for (auto &[head, channels] : m_gainMemory) {
        juce::ValueTree h("head");
        h.setProperty("file", head, nullptr);
        for (auto &[ch, gain] : channels) {
            juce::ValueTree c("channel");
            c.setProperty("name", ch, nullptr);
            c.setProperty("gain", gain, nullptr);
            h.addChild(c, -1, nullptr);
        }
        t.addChild(h, -1, nullptr);
    }
    return t;
}

void ControlSurface::setState(const juce::ValueTree &t)
{
    m_gainMemory.clear();
    for (auto h : t)
        for (auto c : h)
            m_gainMemory[h.getProperty("file").toString()][c.getProperty("name").toString()] = (double)c.getProperty("gain", 50.0);
}
