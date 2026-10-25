// LMS Rig — standalone host: Satan's Little Pedal Board -> amp head -> cab,
// then optionally RTW (channel strip) -> Mega Increasinator (comps + limiter).
// Built on top of ysfx (JSFX runtime). Each section is a YsfxProcessor.
// The cab DSP stays inside the amp (it needs the amp's density + load ohms),
// so the cab section here is persistent state pushed into whichever head is loaded.

#pragma once
#include "processor.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <vector>

class YsfxParameter;
class ControlSurface;

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
    juce::String ampFileName() const { return m_ampIndex >= 0 ? juce::File(m_ampPaths[m_ampIndex]).getFileName() : juce::String(); }
    void selectAmp(int index);

    // cab state, owned by the rig, survives head swaps
    double cabValue(CabKey k) const { return m_cab[k]; }
    void setCabValue(CabKey k, double v);
    YsfxParameter *ampCabParam(CabKey k);
    bool pollCabFromAmp();  // adopt edits made in the amp's own UI; true if changed

    // Record an idea. Two 24-bit WAVs at the device rate land in
    // recordingsDir(): "Idea <stamp>.wav" is the rig's output, what you
    // heard; "Idea <stamp> DI.wav" is the dry input, so a good take can be
    // re-amped later. Start/stop from the message thread; the audio
    // thread only hands blocks to a background writer and never blocks.
    bool startRecording();
    void stopRecording();
    bool isRecording() const { return m_recWriter.load() != nullptr; }
    double recordedSeconds() const { return m_rate > 0 ? (double)m_recFrames.load() / m_rate : 0.0; }
    juce::File lastRecording() const { return m_lastRecording; }
    static juce::File recordingsDir();
    static bool revealDir(const juce::File &dir);

    // An idea is a take plus the sidecar JSON written when it stopped: the
    // head, every knob of every stage, the click tempo. That is what lets
    // the list below filter by head and what REAPER gets rebuilt from.
    struct Idea {
        juce::File json, take, di;
        juce::String stamp, ampFile, ampName;
        double seconds = 0, bpm = 0;
        int64_t time = 0;
    };
    static std::vector<Idea> ideas(const juce::String &ampFile);  // newest first; "" = every head

    // "Idea <stamp>.RPP" next to the take: track 1 the take, no FX; track 2
    // the DI through this rig's chain with the knobs the take was played
    // with; project tempo = the click. Then REAPER opens it.
    bool openInReaper(const Idea &idea, juce::String &error);

    // play a take back, mixed into the output after the chain (never recorded)
    bool playIdea(const Idea &idea);
    void stopIdea();
    bool isPlaying() const { return m_playBuf.load() != nullptr; }
    double playSeconds() const;
    double playLength() const;

    // metronome, mixed into the output after the chain (never recorded)
    std::atomic<bool> clickOn{false};
    std::atomic<double> clickBpm{120.0};
    std::atomic<float> clickLevel{0.5f};

    // the hardware control surface (surface.h): opens the HEAD box's MIDI ports by name
    ControlSurface &surface() { return *m_surface; }

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
    bool acceptsMidi() const override { return true; }
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

    std::unique_ptr<ControlSurface> m_surface;
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

    juce::TimeSliceThread m_recThread{"LMS Rig recorder"};
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> m_recOut, m_recDi;
    std::atomic<juce::AudioFormatWriter::ThreadedWriter *> m_recWriter{nullptr};  // non-null while recording
    juce::AudioFormatWriter::ThreadedWriter *m_recDiWriter = nullptr;
    juce::SpinLock m_recLock;
    std::atomic<int64_t> m_recFrames{0};
    double m_rate = 48000.0;
    juce::AudioBuffer<float> m_diCopy;
    juce::File m_lastRecording;
    juce::var settingsNow() const;
    static juce::var stageSliders(const YsfxProcessor &child);

    struct PlayBuf {
        juce::AudioBuffer<float> audio;
        double rate = 48000.0;
    };
    std::shared_ptr<PlayBuf> m_playHold;
    std::atomic<PlayBuf *> m_playBuf{nullptr};
    juce::SpinLock m_playLock;
    std::atomic<double> m_playPos{0.0};

    double m_clickCountdown = 0, m_clickPhase = 0, m_clickEnv = 0, m_clickHz = 0;
    int m_clickBeat = 0;
    bool m_clickWasOn = false;
    std::atomic<bool> m_clickResync{false};  // beat one on the take's first sample

    std::array<double, numCabKeys> m_cab{};
    bool m_cabKnown = false;  // false until first head supplies defaults
    std::array<int, numCabKeys> m_cabSlider{};  // slider index in the current amp, -1 if absent

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RigProcessor)
};
