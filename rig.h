// LMS Rig — standalone host: Satan's Little Pedal Board -> amp head -> cab,
// then optionally RTW (channel strip) -> Mega Increasinator (comps + limiter).
// Built on top of ysfx (JSFX runtime). Each section is a YsfxProcessor.
// The cab DSP stays inside the amp (it needs the amp's density + load ohms),
// so the cab section here is persistent state pushed into whichever head is loaded.

#pragma once
#include "processor.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include <map>

class YsfxParameter;

// Amp sliders that make up the "cab" (matched by name; slider numbers differ per amp)
enum CabKey { cabType, cabOn, cabMic, cabTypeB, cabMicB, cabBreakup, numCabKeys };

class RigProcessor final : public juce::AudioProcessor {
public:
    RigProcessor();
    ~RigProcessor() override;

    YsfxProcessor &pedals() { return m_pedals; }
    YsfxProcessor &amp() { return m_amp; }
    YsfxProcessor &strip() { return m_strip; }
    YsfxProcessor &master() { return m_master; }

    // peak of the pedalboard's output, i.e. what hits the amp input; the UI
    // takes it with exchange(0) so nothing between two reads is missed
    std::atomic<float> ampInPeak{0.0f};

    // the post-amp stages are in the chain only when switched in
    std::atomic<bool> stripOn{false}, masterOn{false};

    const juce::StringArray &ampNames() const { return m_ampNames; }
    int ampIndex() const { return m_ampIndex; }
    void selectAmp(int index);

    // cab state, owned by the rig, survives head swaps
    double cabValue(CabKey k) const { return m_cab[k]; }
    void setCabValue(CabKey k, double v);
    YsfxParameter *ampCabParam(CabKey k);
    bool pollCabFromAmp();  // adopt edits made in the amp's own UI; true if changed

    // editor view prefs, saved with the rig
    bool hideInactivePedals = false;
    bool classicView = false;  // the JSFX's own UIs instead of the native panels
    int tab = 0;  // bottom area: 0 amp, 1 channel strip, 2 master

    //==========================================================================
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float> &buffer, juce::MidiBuffer &midi) override;
    bool isBusesLayoutSupported(const BusesLayout &layout) const override;

    juce::AudioProcessorEditor *createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "LMS Rig"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String &) override {}

    void getStateInformation(juce::MemoryBlock &dest) override;
    void setStateInformation(const void *data, int size) override;

private:
    struct StubPlayHead : juce::AudioPlayHead {
        int64_t samples = 0;
        double sampleRate = 48000;
        juce::Optional<PositionInfo> getPosition() const override;
    };

    void runChild(YsfxProcessor &child, juce::AudioBuffer<float> &buffer, juce::MidiBuffer &midi, bool silenceIfBusy);
    void loadAmp(int index);
    void pushCabToAmp();

    YsfxProcessor m_pedals;
    YsfxProcessor m_amp;
    YsfxProcessor m_strip;
    YsfxProcessor m_master;
    StubPlayHead m_playHead;

    juce::File m_lmsDir;
    juce::StringArray m_ampPaths;
    juce::StringArray m_ampNames;
    int m_ampIndex = -1;
    std::map<juce::String, juce::MemoryBlock> m_ampStates;  // per-head knob memory

    std::array<double, numCabKeys> m_cab{};
    bool m_cabKnown = false;  // false until first head supplies defaults
    std::array<int, numCabKeys> m_cabSlider{};  // slider index in the current amp, -1 if absent

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RigProcessor)
};
