// LMS Rig — see rig.h

#include "rig.h"
#include "parameter.h"
#include "components/graphics_view.h"
#include "ysfx.h"
#include "BinaryData.h"

static const char *const kCabSliderNames[numCabKeys] = {
    "Cabinet", "Cab Sim", "Mic Distance (%)", "Cabinet B", "Mic B Distance (%)", "Speaker Breakup (%)",
};

static juce::String sliderName(ysfx_t *fx, uint32_t i)
{
    juce::String name = juce::CharPointer_UTF8(ysfx_slider_get_name(fx, i));
    return name.trimCharactersAtStart("-").trim();
}

// "desc:LMS - OJ 95 v2 (Plexi ...)" -> "OJ 95"
static juce::String ampDisplayName(const juce::File &file)
{
    juce::StringArray lines;
    file.readLines(lines);
    for (auto &line : lines) {
        if (!line.startsWith("desc:"))
            continue;
        juce::String name = line.fromFirstOccurrenceOf("desc:", false, false);
        name = name.fromFirstOccurrenceOf("LMS - ", false, false).upToFirstOccurrenceOf("(", false, false).trim();
        if (name.endsWith(" v2"))
            name = name.dropLastCharacters(3);
        if (name.isNotEmpty())
            return name;
    }
    return file.getFileNameWithoutExtension();
}

// Write the bundled JSFX out to the app-data dir (only files that changed) and return it.
static juce::File extractBundledJsfx()
{
    juce::File dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                         .getChildFile("LMS Rig").getChildFile("jsfx");
    dir.createDirectory();
    for (int i = 0; i < LmsRigData::namedResourceListSize; ++i) {
        const char *res = LmsRigData::namedResourceList[i];
        int size = 0;
        const char *data = LmsRigData::getNamedResource(res, size);
        juce::File f = dir.getChildFile(LmsRigData::getNamedResourceOriginalFilename(res));
        juce::MemoryBlock existing;
        if (f.loadFileAsData(existing) && existing.matches(data, (size_t)size))
            continue;
        f.replaceWithData(data, (size_t)size);
    }
    return dir;
}

//==============================================================================
RigProcessor::RigProcessor()
    : AudioProcessor(BusesProperties()
                     .withInput("Input", juce::AudioChannelSet::stereo(), true)
                     .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    m_cabSlider.fill(-1);

    // LMS_DIR=~/LMS runs the live repo copies instead of the bundled ones
    auto env = juce::SystemStats::getEnvironmentVariable("LMS_DIR", {});
    m_lmsDir = env.isNotEmpty() ? juce::File(env) : extractBundledJsfx();

    auto amps = m_lmsDir.findChildFiles(juce::File::findFiles, false, "lms_*_v2.jsfx");
    amps.sort();
    for (auto &f : amps) {
        m_ampPaths.add(f.getFullPathName());
        m_ampNames.add(ampDisplayName(f));
    }

    m_pedals.loadJsfxFile(m_lmsDir.getChildFile("lms_satans_pedalboard.jsfx").getFullPathName(), nullptr, false, false);
    if (!m_ampPaths.isEmpty())
        loadAmp(0);
}

RigProcessor::~RigProcessor() = default;

juce::Optional<juce::AudioPlayHead::PositionInfo> RigProcessor::StubPlayHead::getPosition() const
{
    PositionInfo p;
    p.setTimeInSamples(samples);
    p.setTimeInSeconds((double)samples / sampleRate);
    p.setBpm(120.0);
    p.setTimeSignature(TimeSignature{4, 4});
    p.setIsPlaying(false);
    return p;
}

//==============================================================================
void RigProcessor::selectAmp(int index)
{
    if (index < 0 || index >= m_ampPaths.size() || index == m_ampIndex)
        return;
    loadAmp(index);
}

void RigProcessor::loadAmp(int index)
{
    if (m_ampIndex >= 0) {
        juce::MemoryBlock mb;
        m_amp.getStateInformation(mb);
        m_ampStates[m_ampPaths[m_ampIndex]] = mb;
    }

    m_ampIndex = index;
    const juce::String &path = m_ampPaths[index];

    auto it = m_ampStates.find(path);
    if (it != m_ampStates.end())
        m_amp.setStateInformation(it->second.getData(), (int)it->second.getSize());
    else
        m_amp.loadJsfxFile(path, nullptr, false, false);

    m_cabSlider.fill(-1);
    if (auto info = m_amp.getCurrentInfo()) {
        ysfx_t *fx = info->effect.get();
        for (uint32_t i = 0; i < ysfx_max_sliders; ++i) {
            if (!ysfx_slider_exists(fx, i))
                continue;
            juce::String name = sliderName(fx, i);
            for (int k = 0; k < numCabKeys; ++k)
                if (name == kCabSliderNames[k])
                    m_cabSlider[(size_t)k] = (int)i;
        }
    }

    if (!m_cabKnown) {
        for (int k = 0; k < numCabKeys; ++k)
            if (auto *p = ampCabParam((CabKey)k))
                m_cab[(size_t)k] = p->convertToYsfxValue(p->getValue());
        m_cabKnown = true;
    }
    else
        pushCabToAmp();
}

YsfxParameter *RigProcessor::ampCabParam(CabKey k)
{
    int idx = m_cabSlider[(size_t)k];
    return idx < 0 ? nullptr : m_amp.getYsfxParameter(idx);
}

void RigProcessor::setCabValue(CabKey k, double v)
{
    m_cab[(size_t)k] = v;
    if (auto *p = ampCabParam(k))
        p->setValueNotifyingHost(p->convertFromYsfxValue(v));
}

void RigProcessor::pushCabToAmp()
{
    for (int k = 0; k < numCabKeys; ++k)
        setCabValue((CabKey)k, m_cab[(size_t)k]);
}

bool RigProcessor::pollCabFromAmp()
{
    bool changed = false;
    for (int k = 0; k < numCabKeys; ++k) {
        if (auto *p = ampCabParam((CabKey)k)) {
            double v = p->convertToYsfxValue(p->getValue());
            if (std::abs(v - m_cab[(size_t)k]) > 1e-3) {
                m_cab[(size_t)k] = v;
                changed = true;
            }
        }
    }
    return changed;
}

//==============================================================================
void RigProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    m_playHead.sampleRate = sampleRate;
    for (YsfxProcessor *child : {&m_pedals, &m_amp}) {
        child->setPlayHead(&m_playHead);
        child->setRateAndBufferSizeDetails(sampleRate, samplesPerBlock);
        child->prepareToPlay(sampleRate, samplesPerBlock);
    }
}

void RigProcessor::releaseResources()
{
    m_pedals.releaseResources();
    m_amp.releaseResources();
}

void RigProcessor::runChild(YsfxProcessor &child, juce::AudioBuffer<float> &buffer, juce::MidiBuffer &midi, bool silenceIfBusy)
{
    // a child is suspended while it (re)loads a JSFX; don't block the audio thread on it
    const juce::ScopedTryLock lock(child.getCallbackLock());
    if (!lock.isLocked() || child.isSuspended()) {
        if (silenceIfBusy)
            buffer.clear();
        return;
    }
    child.processBlock(buffer, midi);
}

void RigProcessor::processBlock(juce::AudioBuffer<float> &buffer, juce::MidiBuffer &midi)
{
    juce::ScopedNoDenormals noDenormals;
    runChild(m_pedals, buffer, midi, false);
    runChild(m_amp, buffer, midi, true);
    m_playHead.samples += buffer.getNumSamples();
}

bool RigProcessor::isBusesLayoutSupported(const BusesLayout &layout) const
{
    return layout.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
        && layout.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

//==============================================================================
void RigProcessor::getStateInformation(juce::MemoryBlock &dest)
{
    if (m_ampIndex >= 0) {
        juce::MemoryBlock mb;
        m_amp.getStateInformation(mb);
        m_ampStates[m_ampPaths[m_ampIndex]] = mb;
    }

    juce::ValueTree root("lmsrig");
    root.setProperty("version", 1, nullptr);
    if (m_ampIndex >= 0)
        root.setProperty("amp", juce::File(m_ampPaths[m_ampIndex]).getFileName(), nullptr);

    juce::ValueTree cab("cab");
    for (int k = 0; k < numCabKeys; ++k)
        cab.setProperty(juce::String(k), m_cab[(size_t)k], nullptr);
    root.addChild(cab, -1, nullptr);

    juce::MemoryBlock pedalState;
    m_pedals.getStateInformation(pedalState);
    juce::ValueTree pedals("pedals");
    pedals.setProperty("data", pedalState.toBase64Encoding(), nullptr);
    root.addChild(pedals, -1, nullptr);

    juce::ValueTree amps("amps");
    for (auto &[path, mb] : m_ampStates) {
        juce::ValueTree a("amp");
        a.setProperty("file", juce::File(path).getFileName(), nullptr);
        a.setProperty("data", mb.toBase64Encoding(), nullptr);
        amps.addChild(a, -1, nullptr);
    }
    root.addChild(amps, -1, nullptr);

    juce::MemoryOutputStream stream(dest, false);
    root.writeToStream(stream);
}

void RigProcessor::setStateInformation(const void *data, int size)
{
    juce::ValueTree root = juce::ValueTree::readFromData(data, (size_t)size);
    if (!root.hasType("lmsrig"))
        return;

    juce::MemoryBlock pedalState;
    if (pedalState.fromBase64Encoding(root.getChildWithName("pedals").getProperty("data").toString()))
        m_pedals.setStateInformation(pedalState.getData(), (int)pedalState.getSize());

    // stored by filename so the rig survives the LMS folder moving
    m_ampStates.clear();
    for (auto a : root.getChildWithName("amps")) {
        juce::File f = m_lmsDir.getChildFile(a.getProperty("file").toString());
        juce::MemoryBlock mb;
        if (f.existsAsFile() && mb.fromBase64Encoding(a.getProperty("data").toString())) {
            // rewrite the embedded path to the current location
            juce::ValueTree t = juce::ValueTree::readFromData(mb.getData(), mb.getSize());
            if (t.isValid()) {
                t.setProperty("path", f.getFullPathName(), nullptr);
                mb.reset();
                juce::MemoryOutputStream s(mb, false);
                t.writeToStream(s);
            }
            m_ampStates[f.getFullPathName()] = mb;
        }
    }

    juce::ValueTree cab = root.getChildWithName("cab");
    if (cab.isValid()) {
        for (int k = 0; k < numCabKeys; ++k)
            if (cab.hasProperty(juce::String(k)))
                m_cab[(size_t)k] = (double)cab.getProperty(juce::String(k));
        m_cabKnown = true;
    }

    int index = m_ampPaths.indexOf(m_lmsDir.getChildFile(root.getProperty("amp").toString()).getFullPathName());
    if (index < 0)
        index = 0;
    m_ampIndex = -1;  // don't snapshot the throwaway default head over saved state
    if (!m_ampPaths.isEmpty())
        loadAmp(index);
}

//==============================================================================
namespace {

constexpr int kWidth = 1500;
constexpr int kPedalHeight = 580;
constexpr int kBarHeight = 44;
constexpr int kAmpColumn = 640;
constexpr int kBottomHeight = 760;
constexpr int kHeight = kPedalHeight + kBarHeight + kBottomHeight;

const juce::Colour kBg{0xff0e0b0b};
const juce::Colour kPanel{0xff1a1414};
const juce::Colour kAccent{0xffc0392b};
const juce::Colour kText{0xffe8dcdc};

class RigEditor final : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit RigEditor(RigProcessor &p)
        : AudioProcessorEditor(p), m_proc(p)
    {
        auto &lf = getLookAndFeel();
        lf.setColour(juce::ComboBox::backgroundColourId, kPanel.brighter(0.08f));
        lf.setColour(juce::ComboBox::outlineColourId, kAccent.withAlpha(0.6f));
        lf.setColour(juce::ComboBox::textColourId, kText);
        lf.setColour(juce::PopupMenu::backgroundColourId, kPanel);
        lf.setColour(juce::PopupMenu::highlightedBackgroundColourId, kAccent);
        lf.setColour(juce::TextButton::buttonColourId, kPanel.brighter(0.08f));
        lf.setColour(juce::TextButton::textColourOffId, kText);
        lf.setColour(juce::Slider::thumbColourId, kAccent);
        lf.setColour(juce::Slider::rotarySliderFillColourId, kAccent);
        lf.setColour(juce::Slider::trackColourId, kAccent.withAlpha(0.7f));
        lf.setColour(juce::Slider::textBoxTextColourId, kText);
        lf.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        lf.setColour(juce::Label::textColourId, kText);
        lf.setColour(juce::ToggleButton::textColourId, kText);
        lf.setColour(juce::ToggleButton::tickColourId, kAccent);

        addAndMakeVisible(m_content);
        m_content.setSize(kWidth, kHeight);
        m_content.addAndMakeVisible(m_pedalView);
        m_content.addAndMakeVisible(m_ampView);

        // amp head picker
        m_prev.setButtonText("<");
        m_next.setButtonText(">");
        m_prev.onClick = [this] { step(-1); };
        m_next.onClick = [this] { step(+1); };
        m_ampBox.addItemList(m_proc.ampNames(), 1);
        m_ampBox.setJustificationType(juce::Justification::centred);
        m_ampBox.onChange = [this] { m_proc.selectAmp(m_ampBox.getSelectedItemIndex()); refreshAll(); };
        for (auto *c : std::initializer_list<juce::Component *>{&m_prev, &m_next, &m_ampBox})
            m_content.addAndMakeVisible(c);

        // cab section
        setupLabel(m_cabTitle, "CABINET", 26.0f);
        setupLabel(m_aLabel, "CAB A", 16.0f);
        setupLabel(m_bLabel, "CAB B", 16.0f);
        setupLabel(m_micALabel, "Mic distance", 13.0f);
        setupLabel(m_micBLabel, "Mic distance", 13.0f);
        setupLabel(m_breakupLabel, "Speaker breakup", 13.0f);

        m_typeA.onChange = [this] { m_proc.setCabValue(cabType, m_typeA.getSelectedItemIndex()); };
        m_typeB.onChange = [this] { m_proc.setCabValue(cabTypeB, m_typeB.getSelectedItemIndex()); };
        m_cabOn.setButtonText("Cab sim");
        m_cabOn.onClick = [this] { m_proc.setCabValue(cabOn, m_cabOn.getToggleState() ? 1.0 : 0.0); };
        setupKnob(m_micA, cabMic);
        setupKnob(m_micB, cabMicB);
        setupKnob(m_breakup, cabBreakup);
        for (auto *c : std::initializer_list<juce::Component *>{&m_typeA, &m_typeB, &m_cabOn})
            m_content.addAndMakeVisible(c);

        layoutContent();
        refreshAll();

        auto area = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->userArea;
        double scale = juce::jmin(1.0, area.getHeight() * 0.92 / kHeight, area.getWidth() * 0.95 / kWidth);
        setResizable(true, true);
        getConstrainer()->setFixedAspectRatio((double)kWidth / kHeight);
        setResizeLimits(kWidth / 3, kHeight / 3, kWidth * 2, kHeight * 2);
        setSize(juce::roundToInt(kWidth * scale), juce::roundToInt(kHeight * scale));

        startTimerHz(15);
    }

    ~RigEditor() override
    {
        m_pedalView.setEffect(nullptr);
        m_ampView.setEffect(nullptr);
    }

    void paint(juce::Graphics &g) override { g.fillAll(kBg); }

    void resized() override
    {
        float s = (float)getWidth() / (float)kWidth;
        m_content.setTransform(juce::AffineTransform::scale(s));
    }

private:
    struct Content : juce::Component {
        void paint(juce::Graphics &g) override
        {
            g.fillAll(kBg);
            g.setColour(kPanel);
            g.fillRect(0, kPedalHeight, kWidth, kBarHeight);
            g.fillRect(kAmpColumn + 20, kPedalHeight + kBarHeight + 10, kWidth - kAmpColumn - 30, kBottomHeight - 20);
            g.setColour(kAccent);
            g.fillRect(0, kPedalHeight, kWidth, 2);
            g.fillRect(0, kPedalHeight + kBarHeight - 2, kWidth, 2);
        }
    };

    void setupLabel(juce::Label &l, const juce::String &text, float size)
    {
        l.setText(text, juce::dontSendNotification);
        l.setFont(juce::FontOptions(size, juce::Font::bold));
        l.setJustificationType(juce::Justification::centred);
        m_content.addAndMakeVisible(l);
    }

    void setupKnob(juce::Slider &s, CabKey k)
    {
        s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        s.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 70, 20);
        s.setRange(0.0, 100.0, 1.0);
        s.setTextValueSuffix(" %");
        s.onValueChange = [this, &s, k] { m_proc.setCabValue(k, s.getValue()); };
        m_content.addAndMakeVisible(s);
    }

    void layoutContent()
    {
        m_pedalView.setBounds(0, 0, kWidth, kPedalHeight);

        int barY = kPedalHeight + 6;
        int bh = kBarHeight - 12;
        m_prev.setBounds(10, barY, 40, bh);
        m_ampBox.setBounds(56, barY, kAmpColumn - 112, bh);
        m_next.setBounds(kAmpColumn - 50, barY, 40, bh);

        layoutAmpView();

        int x0 = kAmpColumn + 20, w = kWidth - kAmpColumn - 30;
        int y = kPedalHeight + kBarHeight + 30;
        m_cabTitle.setBounds(x0, y, w, 36);
        y += 70;
        int colW = w / 2;
        m_aLabel.setBounds(x0, y, colW, 24);
        m_bLabel.setBounds(x0 + colW, y, colW, 24);
        y += 34;
        m_typeA.setBounds(x0 + 30, y, colW - 60, 30);
        m_typeB.setBounds(x0 + colW + 30, y, colW - 60, 30);
        y += 60;
        m_micALabel.setBounds(x0, y, colW, 20);
        m_micBLabel.setBounds(x0 + colW, y, colW, 20);
        y += 22;
        m_micA.setBounds(x0 + colW / 2 - 60, y, 120, 130);
        m_micB.setBounds(x0 + colW + colW / 2 - 60, y, 120, 130);
        y += 190;
        m_breakupLabel.setBounds(x0, y, w, 20);
        y += 22;
        m_breakup.setBounds(x0 + w / 2 - 70, y, 140, 150);
        y += 190;
        m_cabOn.setBounds(x0 + w / 2 - 60, y, 120, 30);
    }

    void layoutAmpView()
    {
        int w = kAmpColumn, h = kBottomHeight;
        if (auto info = m_proc.amp().getCurrentInfo()) {
            uint32_t dim[2] = {};
            if (info->effect && ysfx_get_gfx_dim(info->effect.get(), dim) && dim[0] && dim[1]) {
                w = juce::jmin((int)dim[0], kAmpColumn);
                h = juce::jmin((int)dim[1], kBottomHeight);
            }
        }
        m_ampView.setBounds((kAmpColumn - w) / 2, kPedalHeight + kBarHeight + (kBottomHeight - h) / 2, w, h);
    }

    void step(int dir)
    {
        int n = m_proc.ampNames().size();
        if (n == 0)
            return;
        int i = (m_proc.ampIndex() + dir + n) % n;
        m_proc.selectAmp(i);
        refreshAll();
    }

    void fillCabTypes(juce::ComboBox &box, CabKey k)
    {
        box.clear(juce::dontSendNotification);
        auto *p = m_proc.ampCabParam(k);
        if (!p)
            return;
        for (int i = 0; i < p->getSliderEnumSize(); ++i)
            box.addItem(juce::String(juce::CharPointer_UTF8(p->getSliderEnumName(i))), i + 1);
    }

    void refreshCab()
    {
        m_typeA.setSelectedItemIndex((int)std::lround(m_proc.cabValue(cabType)), juce::dontSendNotification);
        m_typeB.setSelectedItemIndex((int)std::lround(m_proc.cabValue(cabTypeB)), juce::dontSendNotification);
        m_cabOn.setToggleState(m_proc.cabValue(cabOn) > 0.5, juce::dontSendNotification);
        m_micA.setValue(m_proc.cabValue(cabMic), juce::dontSendNotification);
        m_micB.setValue(m_proc.cabValue(cabMicB), juce::dontSendNotification);
        m_breakup.setValue(m_proc.cabValue(cabBreakup), juce::dontSendNotification);
    }

    void refreshAll()
    {
        m_ampBox.setSelectedItemIndex(m_proc.ampIndex(), juce::dontSendNotification);
        timerCallback();
    }

    void timerCallback() override
    {
        if (auto info = m_proc.pedals().getCurrentInfo(); info != m_pedalInfo) {
            m_pedalInfo = info;
            m_pedalView.setEffect(info ? info->effect.get() : nullptr);
        }
        if (auto info = m_proc.amp().getCurrentInfo(); info != m_ampInfo) {
            m_ampInfo = info;
            m_ampView.setEffect(info ? info->effect.get() : nullptr);
            layoutAmpView();
            fillCabTypes(m_typeA, cabType);
            fillCabTypes(m_typeB, cabTypeB);
            refreshCab();
        }
        if (m_proc.pollCabFromAmp())
            refreshCab();
    }

    RigProcessor &m_proc;
    YsfxInfo::Ptr m_pedalInfo, m_ampInfo;

    Content m_content;
    YsfxGraphicsView m_pedalView, m_ampView;
    juce::TextButton m_prev, m_next;
    juce::ComboBox m_ampBox;

    juce::Label m_cabTitle, m_aLabel, m_bLabel, m_micALabel, m_micBLabel, m_breakupLabel;
    juce::ComboBox m_typeA, m_typeB;
    juce::ToggleButton m_cabOn;
    juce::Slider m_micA, m_micB, m_breakup;
};

} // namespace

juce::AudioProcessorEditor *RigProcessor::createEditor()
{
    return new RigEditor(*this);
}

juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter()
{
    return new RigProcessor;
}
