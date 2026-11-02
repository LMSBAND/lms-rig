// LMS Rig — the hardware control surface (the HEAD box, v0). The wire contract
// is surface/SPEC.md; the box is surface/firmware/head. The surface finds the
// box itself: a MIDI port named "LMS Head" (an S3 box), or a USB serial port
// that answers PING with HELLO (the classic ESP32 box, plain MIDI bytes at
// 115200). It works the same in the standalone and in a DAW; everything
// happens on the message thread from a
// timer: incoming CCs become slider moves, and what the box should show
// (labels, values, selections) goes back out, only when it changed.
//
// What the twelve pots and four buttons are, on whichever head is loaded:
//   pots 0-5   GAIN BASS MIDDLE TREBLE PRESENCE MASTER, bound by slider NAME so they
//              survive head swaps. GAIN and MASTER are PER CHANNEL: the head's own
//              "<Channel> Master" where it has one, and for gain a memory kept here,
//              so Clean and Crunch each remember their own gain.
//   pots 6-8   AMP / CAB A / CAB B: a selection index
//   pots 9-11  the head's remaining continuous sliders, three per page
//   buttons    the head's switches and enums, three per page, Channel first; button 4 = PAGE
#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "processor.h"
#include <array>
#include <map>
#include <memory>
#include <vector>

class RigProcessor;
class YsfxParameter;

class ControlSurface final : private juce::Timer, private juce::MidiInputCallback {
public:
    explicit ControlSurface(RigProcessor &rig);
    ~ControlSurface() override;

    juce::ValueTree getState() const;           // the per-channel gain memory
    void setState(const juce::ValueTree &state);

    bool connected() const { return m_in != nullptr || m_tty >= 0; }

private:
    struct Event { uint8_t status, d1, d2; };
    enum Slot { slotPots = 0x00, slotSoft = 0x08, slotButtons = 0x10, slotAmp = 0x18, slotCabA = 0x19, slotCabB = 0x1A, slotPage = 0x20, slotChannel = 0x21, numSlots = 0x22 };

    void timerCallback() override;
    void handleIncomingMidiMessage(juce::MidiInput *, const juce::MidiMessage &m) override;
    void scanPorts();
    void scanSerial();
    void readSerial();
    void serialBytes(const uint8_t *d, int n);        // the box's bytes, parsed into events
    void closeSerial();
    void reconnectBluetooth();                        // Linux: BlueZ won't reconnect a BLE-MIDI box by itself
    void handle(const Event &e);
    void onPot(int cc, int v14);
    void onButton(int i);
    void onSelect(int which, int index);

    // the head's sliders, by role
    void rebuild();                                   // after a head swap or a channel change
    YsfxParameter *ampParam(int sliderIndex) const;
    int findSlider(const juce::String &alternatives) const;   // "Gain|Pre Gain" -> slider index or -1
    int channelSlider() const { return m_channel; }
    juce::String channelName() const;
    int masterFor(const juce::String &channel) const;
    void rememberGain();
    void recallGain();

    // what the box shows
    void refresh(bool everything);
    void sendCC(int cc, int value);
    void send14(int cc, int v14);
    void sendSysex(int msg, int slot, const juce::String &data);
    void rawSysex(const juce::MemoryBlock &body);     // 7D 01 msg slot data..., framed and sent on whatever is connected
    juce::String labelOf(int sliderIndex) const;
    juce::String valueOf(int sliderIndex) const;

    RigProcessor &m_rig;
    std::unique_ptr<juce::MidiInput> m_in;
    std::unique_ptr<juce::MidiOutput> m_out;
    int m_scanTick = 0;

    // the serial box
    int m_tty = -1;                                   // the open port, or -1
    juce::String m_ttyPath;
    bool m_ttyHello = false;                          // it answered: it's ours
    juce::uint32 m_ttyOpenedAt = 0;
    std::map<juce::String, juce::uint32> m_ttyTried;  // ports that didn't answer, and when

    // the Bluetooth box: a background bluetoothctl that connects a paired "LMS Head"
    std::unique_ptr<juce::ChildProcess> m_btHelper;
    juce::uint32 m_btLastTry = 0;
    std::vector<uint8_t> m_sx; bool m_inSx = false; int m_ccState = 0; uint8_t m_ccNum = 0;

    juce::AbstractFifo m_fifo{512};
    std::array<Event, 512> m_events{};
    int m_msb[128] = {};

    // the head as the surface sees it
    YsfxInfo::Ptr m_info;
    int m_fixed[6] = {-1, -1, -1, -1, -1, -1};        // slider index per fixed pot
    int m_channel = -1;                                // the "Channel" enum slider
    int m_lastChannelValue = -1;
    std::vector<int> m_softKnobs, m_softSwitches;      // everything else, paged in threes
    int m_page = 0;
    std::map<juce::String, std::map<juce::String, double>> m_gainMemory;   // head file -> channel name -> gain (slider units)

    std::array<juce::String, numSlots> m_sentLabel, m_sentValue;   // what the box shows; a sentinel marks "never sent"
    std::map<int, int> m_sentCC;
    std::array<int, 3> m_sentRange{-1, -1, -1};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ControlSurface)
};
