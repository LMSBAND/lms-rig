// LMS Rig — see rig.h

#include "rig.h"
#include "surface.h"
#include "rig_ui.h"
#include "parameter.h"
#include "components/graphics_view.h"
#include "ysfx.h"
#include "BinaryData.h"
#include <unistd.h>
#include <sys/wait.h>

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
    m_strip.loadJsfxFile(m_lmsDir.getChildFile("lms_rtw.jsfx").getFullPathName(), nullptr, false, false);
    m_master.loadJsfxFile(m_lmsDir.getChildFile("lms_mega_increasinator.jsfx").getFullPathName(), nullptr, false, false);
    if (!m_ampPaths.isEmpty())
        loadAmp(0);
    m_surface = std::make_unique<ControlSurface>(*this);
}

RigProcessor::~RigProcessor()
{
    m_surface.reset();
    stopRecording();
    m_recThread.stopThread(2000);
}

//==============================================================================
juce::File RigProcessor::recordingsDir()
{
    return juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("LMS Rig");
}

bool RigProcessor::startRecording()
{
    stopRecording();
    auto dir = recordingsDir();
    if (!dir.createDirectory())
        return false;
    const juce::String stamp = juce::Time::getCurrentTime().formatted("%Y-%m-%d %H.%M.%S");
    juce::File out = dir.getChildFile("Idea " + stamp + ".wav");
    juce::File di = dir.getChildFile("Idea " + stamp + " DI.wav");

    auto make = [this](const juce::File &f) -> std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> {
        f.deleteFile();
        std::unique_ptr<juce::FileOutputStream> os(f.createOutputStream());
        if (!os || os->failedToOpen())
            return nullptr;
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(os.get(), m_rate, 2, 24, {}, 0));
        if (!w)
            return nullptr;
        os.release();  // the writer owns the stream now
        return std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(w.release(), m_recThread, 1 << 16);
    };

    if (!m_recThread.isThreadRunning())
        m_recThread.startThread();
    auto o = make(out);
    auto d = make(di);
    if (!o || !d) {
        out.deleteFile();
        di.deleteFile();
        return false;
    }
    m_recFrames = 0;
    m_clickResync = true;  // the take starts on the one, so it sits on REAPER's grid at 0
    {
        const juce::SpinLock::ScopedLockType lock(m_recLock);
        m_recOut = std::move(o);
        m_recDi = std::move(d);
        m_recDiWriter = m_recDi.get();
        m_recWriter = m_recOut.get();
    }
    m_lastRecording = out;
    return true;
}

void RigProcessor::stopRecording()
{
    const bool was = isRecording();
    {
        const juce::SpinLock::ScopedLockType lock(m_recLock);
        m_recWriter = nullptr;
        m_recDiWriter = nullptr;
    }
    // destroying a ThreadedWriter flushes what is buffered and closes the file
    m_recOut.reset();
    m_recDi.reset();
    if (!was || !m_lastRecording.existsAsFile())
        return;

    // the sidecar: what this take was played through
    auto *o = new juce::DynamicObject();
    o->setProperty("take", m_lastRecording.getFileName());
    o->setProperty("di", m_lastRecording.getSiblingFile(m_lastRecording.getFileNameWithoutExtension() + " DI.wav").getFileName());
    o->setProperty("stamp", m_lastRecording.getFileNameWithoutExtension().fromFirstOccurrenceOf("Idea ", false, false));
    o->setProperty("time", juce::Time::getCurrentTime().toMilliseconds());
    o->setProperty("seconds", recordedSeconds());
    o->setProperty("rate", m_rate);
    o->setProperty("settings", settingsNow());
    juce::MemoryBlock state;
    getStateInformation(state);
    o->setProperty("state", juce::Base64::toBase64(state.getData(), state.getSize()));
    m_lastRecording.withFileExtension("json").replaceWithText(juce::JSON::toString(juce::var(o)));
}

// Every slider the JSFX defines, by 1-based number as REAPER counts them;
// a path slider is its string, which REAPER writes as "".
juce::var RigProcessor::stageSliders(const YsfxProcessor &child)
{
    auto *o = new juce::DynamicObject();
    if (auto info = const_cast<YsfxProcessor &>(child).getCurrentInfo()) {
        ysfx_t *fx = info->effect.get();
        if (fx) {
            o->setProperty("file", juce::File(juce::CharPointer_UTF8(ysfx_get_file_path(fx))).getFileName());
            auto *s = new juce::DynamicObject();
            for (uint32_t i = 0; i < ysfx_max_sliders; ++i) {
                if (!ysfx_slider_exists(fx, i))
                    continue;
                if (ysfx_slider_is_path(fx, i))
                    s->setProperty(juce::String(i + 1), juce::String(juce::CharPointer_UTF8(ysfx_slider_path(fx, i))));
                else
                    s->setProperty(juce::String(i + 1), (double)ysfx_slider_get_value(fx, i));
            }
            o->setProperty("sliders", juce::var(s));
        }
    }
    return juce::var(o);
}

juce::var RigProcessor::settingsNow() const
{
    auto *o = new juce::DynamicObject();
    o->setProperty("ampFile", m_ampIndex >= 0 ? juce::File(m_ampPaths[m_ampIndex]).getFileName() : juce::String());
    o->setProperty("ampName", m_ampIndex >= 0 ? m_ampNames[m_ampIndex] : juce::String());
    o->setProperty("stripOn", stripOn.load());
    o->setProperty("masterOn", masterOn.load());
    o->setProperty("clickOn", clickOn.load());
    o->setProperty("bpm", clickBpm.load());
    auto *cab = new juce::DynamicObject();
    for (int k = 0; k < numCabKeys; ++k)
        cab->setProperty(kCabSliderNames[k], m_cab[(size_t)k]);
    o->setProperty("cab", juce::var(cab));
    o->setProperty("pedals", stageSliders(m_pedals));
    o->setProperty("amp", stageSliders(m_amp));
    o->setProperty("strip", stageSliders(m_strip));
    o->setProperty("master", stageSliders(m_master));
    return juce::var(o);
}

std::vector<RigProcessor::Idea> RigProcessor::ideas(const juce::String &ampFile)
{
    std::vector<Idea> out;
    for (const auto &f : recordingsDir().findChildFiles(juce::File::findFiles, false, "Idea *.json")) {
        juce::var v = juce::JSON::parse(f);
        if (!v.isObject())
            continue;
        Idea idea;
        idea.json = f;
        idea.take = f.getSiblingFile(v["take"].toString());
        idea.di = f.getSiblingFile(v["di"].toString());
        idea.stamp = v["stamp"].toString();
        idea.seconds = (double)v["seconds"];
        idea.time = (juce::int64)v["time"];
        auto s = v["settings"];
        idea.ampFile = s["ampFile"].toString();
        idea.ampName = s["ampName"].toString();
        idea.bpm = (double)s["bpm"];
        if (ampFile.isNotEmpty() && idea.ampFile != ampFile)
            continue;
        if (!idea.take.existsAsFile())
            continue;
        out.push_back(std::move(idea));
    }
    std::sort(out.begin(), out.end(), [](const Idea &a, const Idea &b) { return a.time > b.time; });
    return out;
}

// fork/exec, detached. xdg-open hangs on this desktop (xdg-mime never
// returns), so the file manager and REAPER are started directly.
static bool launchDetached(const juce::StringArray &argv)
{
    if (argv.isEmpty() || !juce::File(argv[0]).existsAsFile())
        return false;
    std::vector<std::string> strs;
    std::vector<char *> ptrs;
    for (const auto &a : argv)
        strs.push_back(a.toStdString());
    for (auto &s : strs)
        ptrs.push_back(s.data());
    ptrs.push_back(nullptr);
    pid_t pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        setsid();
        if (fork() != 0)
            _exit(0);  // grandchild carries on, nothing waits on it
        execv(ptrs[0], ptrs.data());
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return true;
}

static juce::String findExe(std::initializer_list<const char *> names)
{
    juce::StringArray dirs;
    dirs.addTokens(juce::SystemStats::getEnvironmentVariable("PATH", "/usr/local/bin:/usr/bin:/bin"), ":", "");
    dirs.insert(0, "/usr/local/bin");
    for (const char *n : names)
        for (const auto &d : dirs)
            if (juce::File(d).getChildFile(n).existsAsFile())
                return juce::File(d).getChildFile(n).getFullPathName();
    return {};
}

bool RigProcessor::revealDir(const juce::File &dir)
{
    dir.createDirectory();
    juce::String fm = findExe({"dolphin", "nautilus", "thunar", "pcmanfm", "nemo", "xdg-open"});
    return fm.isNotEmpty() && launchDetached({fm, dir.getFullPathName()});
}

static juce::String rppGuid()
{
    return "{" + juce::Uuid().toDashedString().toUpperCase() + "}";
}

static juce::String rppNumber(double v)
{
    juce::String s(v, 6);
    if (s.contains("."))
        s = s.trimCharactersAtEnd("0").trimCharactersAtEnd(".");
    return s.isEmpty() ? "0" : s;
}

// <JS "LMS Plugins/LMS/<file>" "" + one line of every slider, "-" where none
static juce::String rppJs(const juce::var &stage)
{
    juce::String file = stage["file"].toString();
    if (file.isEmpty())
        return {};
    auto sliders = stage["sliders"];
    int last = 64;
    if (auto *o = sliders.getDynamicObject())
        for (const auto &p : o->getProperties())
            last = juce::jmax(last, p.name.toString().getIntValue());
    juce::StringArray vals;
    for (int i = 1; i <= last; ++i) {
        auto v = sliders.getProperty(juce::Identifier(juce::String(i)), juce::var());
        vals.add(v.isVoid() ? "-" : v.isString() ? "\"\"" : rppNumber((double)v));
    }
    juce::String s;
    s << "      BYPASS 0 0 0\n"
      << "      <JS \"LMS Plugins/LMS/" << file << "\" \"\"\n"
      << "        " << vals.joinIntoString(" ") << "\n"
      << "      >\n"
      << "      FXID " << rppGuid() << "\n"
      << "      WAK 0 0\n";
    return s;
}

// The LMS Rig VST3 on a track, with the state blob the take was played
// through. REAPER's VST chunk, as decoded from files REAPER itself saved:
//   header line:  id, EE5EEDFE, ins + 64-bit masks, outs + masks, body size, 1, FFFF1080
//   body lines:   [int32 state size][int32 1][component state][int32 0][int32 0]
//                 (the trailing pair is the edit-controller state, empty for JUCE),
//                 base64, 128 chars a line
//   last line:    AAAQAAAA (empty program name)
// The id and class id are what REAPER registered the plugin under; read
// from its plugin list when present, else the values it gave this build.
static juce::String rppVst(const juce::String &stateB64)
{
    juce::String id = "753583815", fuid = "ABCDEF019182FAEB4C4D53784C726967";
    auto ini = juce::File("~/.config/REAPER/reaper-vstplugins64.ini");
    if (ini.existsAsFile()) {
        juce::StringArray lines;
        ini.readLines(lines);
        for (const auto &l : lines) {
            if (!l.startsWith("LMS_Rig.vst3="))
                continue;
            auto reg = l.fromFirstOccurrenceOf(",", false, false);  // "753583815{ABCD...,LMS Rig (LMS)"
            id = reg.upToFirstOccurrenceOf("{", false, false).trim();
            fuid = reg.fromFirstOccurrenceOf("{", false, false).upToFirstOccurrenceOf(",", false, false).trim();
            break;
        }
    }
    juce::MemoryOutputStream raw;
    juce::Base64::convertFromBase64(raw, stateB64);
    juce::MemoryOutputStream state;
    state.writeInt((int)raw.getDataSize());
    state.writeInt(1);
    state.write(raw.getData(), raw.getDataSize());
    state.writeInt(0);
    state.writeInt(0);

    juce::MemoryOutputStream head;
    head.writeInt((int)id.getLargeIntValue());
    const uint8_t magic[] = {0xEE, 0x5E, 0xED, 0xFE};
    head.write(magic, 4);
    head.writeInt(2);
    head.writeInt64(1);
    head.writeInt64(2);
    head.writeInt(2);
    head.writeInt64(1);
    head.writeInt64(2);
    head.writeInt((int)state.getDataSize());
    head.writeInt(1);
    const uint8_t tail[] = {0xFF, 0xFF, 0x10, 0x80};
    head.write(tail, 4);

    juce::String body = juce::Base64::toBase64(state.getData(), state.getDataSize());
    juce::String s;
    s << "      BYPASS 0 0 0\n"
      << "      <VST \"VST3: LMS Rig (LMS)\" \"LMS Rig.vst3\" 0 \"\" " << id << "{" << fuid << "} \"\"\n"
      << "        " << juce::Base64::toBase64(head.getData(), head.getDataSize()) << "\n";
    for (int i = 0; i < body.length(); i += 128)
        s << "        " << body.substring(i, i + 128) << "\n";
    s << "        AAAQAAAA\n"
      << "      >\n"
      << "      FXID " << rppGuid() << "\n"
      << "      WAK 0 0\n";
    return s;
}

static juce::String rppItem(const juce::File &wav, double seconds)
{
    juce::String s;
    s << "    <ITEM\n"
      << "      POSITION 0\n"
      << "      LENGTH " << rppNumber(seconds) << "\n"
      << "      LOOP 0\n"
      << "      IGUID " << rppGuid() << "\n"
      << "      NAME \"" << wav.getFileName() << "\"\n"
      << "      GUID " << rppGuid() << "\n"
      << "      <SOURCE WAVE\n"
      << "        FILE \"" << wav.getFullPathName() << "\"\n"
      << "      >\n"
      << "    >\n";
    return s;
}

bool RigProcessor::openInReaper(const Idea &idea, juce::String &error)
{
    juce::var v = juce::JSON::parse(idea.json);
    if (!v.isObject()) {
        error = "no sidecar for that take";
        return false;
    }
    auto s = v["settings"];
    const double bpm = (double)s["bpm"] > 0 ? (double)s["bpm"] : 120.0;

    juce::String p;
    p << "<REAPER_PROJECT 0.1 \"7.25/linux-x86_64\" " << (int64_t)(juce::Time::currentTimeMillis() / 1000) << "\n"
      << "  TEMPO " << rppNumber(bpm) << " 4 4\n"
      << "  <TRACK " << rppGuid() << "\n"
      << "    NAME \"Idea " << idea.stamp << "\"\n"
      << "    PEAKCOL 16576\n"
      << "    MUTESOLO 0 0 0\n"
      << "    NCHAN 2\n"
      << "    FX 1\n"
      << "    MAINSEND 1 0\n"
      << rppItem(idea.take, idea.seconds)
      << "  >\n"
      << "  <TRACK " << rppGuid() << "\n"
      << "    NAME \"LMS Rig - " << s["ampName"].toString() << "\"\n"
      << "    PEAKCOL 2763519\n"
      << "    MUTESOLO 0 0 0\n"
      << "    NCHAN 2\n"
      << "    FX 1\n"
      << "    MAINSEND 1 0\n"
      << "    <FXCHAIN\n"
      << "      SHOW 0\n"
      << "      LASTSEL 0\n"
      << "      DOCKED 0\n";
    if (v["state"].isString()) {
        // the rig itself, exactly as it was
        p << rppVst(v["state"].toString());
    } else {
        // an older sidecar: the chain as JSFX, knob by knob
        p << rppJs(s["pedals"]) << rppJs(s["amp"]);
        if ((bool)s["stripOn"])
            p << rppJs(s["strip"]);
        if ((bool)s["masterOn"])
            p << rppJs(s["master"]);
    }
    p << "    >\n"
      << "  >\n"   // the rig alone on this track: play into it, or drop the DI on it to re-amp
      << ">\n";

    juce::File rpp = idea.json.withFileExtension("RPP");
    if (!rpp.replaceWithText(p)) {
        error = "could not write " + rpp.getFileName();
        return false;
    }
    juce::String reaper = findExe({"reaper"});
    if (reaper.isEmpty()) {
        error = "reaper not found on PATH; project written: " + rpp.getFileName();
        return false;
    }
    launchDetached({reaper, rpp.getFullPathName()});
    return true;
}

//==============================================================================
bool RigProcessor::playIdea(const Idea &idea)
{
    stopIdea();
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(idea.take));
    if (!r || r->lengthInSamples <= 0)
        return false;
    auto buf = std::make_shared<PlayBuf>();
    buf->rate = r->sampleRate;
    buf->audio.setSize(2, (int)r->lengthInSamples);
    r->read(&buf->audio, 0, (int)r->lengthInSamples, 0, true, true);
    if (r->numChannels == 1)
        buf->audio.copyFrom(1, 0, buf->audio, 0, 0, buf->audio.getNumSamples());
    m_playPos = 0.0;
    {
        const juce::SpinLock::ScopedLockType lock(m_playLock);
        m_playHold = buf;
        m_playBuf = buf.get();
    }
    return true;
}

void RigProcessor::stopIdea()
{
    {
        const juce::SpinLock::ScopedLockType lock(m_playLock);
        m_playBuf = nullptr;
    }
    m_playHold.reset();
}

double RigProcessor::playSeconds() const
{
    auto *b = m_playBuf.load();
    return b ? m_playPos.load() / b->rate : 0.0;
}

double RigProcessor::playLength() const
{
    auto *b = m_playBuf.load();
    return b ? b->audio.getNumSamples() / b->rate : 0.0;
}

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
    if (isRecording() && sampleRate != m_rate)
        stopRecording();  // a take cannot change rate halfway through
    m_rate = sampleRate;
    m_diCopy.setSize(2, juce::jmax(samplesPerBlock, 64));
    m_playHead.sampleRate = sampleRate;
    for (YsfxProcessor *child : {&m_pedals, &m_amp, &m_strip, &m_master}) {
        child->setPlayHead(&m_playHead);
        child->setRateAndBufferSizeDetails(sampleRate, samplesPerBlock);
        child->prepareToPlay(sampleRate, samplesPerBlock);
    }
}

void RigProcessor::releaseResources()
{
    m_pedals.releaseResources();
    m_amp.releaseResources();
    m_strip.releaseResources();
    m_master.releaseResources();
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
    const int n = buffer.getNumSamples();
    const bool rec = m_recWriter.load() != nullptr;
    if (rec) {
        // the DI, before anything touches it
        if (n > m_diCopy.getNumSamples())
            m_diCopy.setSize(2, n, false, false, true);
        for (int ch = 0; ch < 2; ++ch)
            m_diCopy.copyFrom(ch, 0, buffer, juce::jmin(ch, buffer.getNumChannels() - 1), 0, n);
    }
    runChild(m_pedals, buffer, midi, false);

    float pk = 0.0f;
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        pk = juce::jmax(pk, buffer.getMagnitude(ch, 0, buffer.getNumSamples()));
    for (float old = ampInPeak.load(); pk > old && !ampInPeak.compare_exchange_weak(old, pk);) {}

    runChild(m_amp, buffer, midi, true);
    if (stripOn)
        runChild(m_strip, buffer, midi, false);
    if (masterOn)
        runChild(m_master, buffer, midi, false);

    if (rec && buffer.getNumChannels() >= 2) {
        const juce::SpinLock::ScopedTryLockType lock(m_recLock);
        if (lock.isLocked() && m_recWriter.load() != nullptr) {
            m_recWriter.load()->write(buffer.getArrayOfReadPointers(), n);
            m_recDiWriter->write(m_diCopy.getArrayOfReadPointers(), n);
            m_recFrames += n;
        }
    }

    // ---- after the tap: things you hear but never record ----
    // a take playing back
    if (m_playBuf.load() != nullptr) {
        const juce::SpinLock::ScopedTryLockType lock(m_playLock);
        if (PlayBuf *pb = lock.isLocked() ? m_playBuf.load() : nullptr) {
            const double step = pb->rate / m_rate;
            const int len = pb->audio.getNumSamples();
            double pos = m_playPos.load();
            const int chans = juce::jmin(2, buffer.getNumChannels());
            for (int i = 0; i < n && pos < len - 1; ++i, pos += step) {
                const int i0 = (int)pos;
                const float fr = (float)(pos - i0);
                for (int ch = 0; ch < chans; ++ch) {
                    const float *src = pb->audio.getReadPointer(ch);
                    buffer.addSample(ch, i, src[i0] + fr * (src[i0 + 1] - src[i0]));
                }
            }
            m_playPos = pos;
            if (pos >= len - 1)
                m_playBuf = nullptr;  // done; the message thread drops the buffer
        }
    }

    // the click: a short sine ping, higher on the one
    const bool click = clickOn.load();
    if (click || m_clickEnv > 1e-4) {
        if ((click && !m_clickWasOn) || (rec && m_clickResync.exchange(false))) {
            m_clickCountdown = 0;
            m_clickBeat = 0;
        }
        const double beat = 60.0 / juce::jlimit(20.0, 400.0, clickBpm.load()) * m_rate;
        const double decay = std::exp(-1.0 / (0.030 * m_rate));
        const float level = clickLevel.load() * 0.5f;
        const int chans = juce::jmin(2, buffer.getNumChannels());
        for (int i = 0; i < n; ++i) {
            if (click) {
                m_clickCountdown -= 1;
                if (m_clickCountdown <= 0) {
                    m_clickCountdown += beat;
                    m_clickHz = (m_clickBeat % 4 == 0) ? 1760.0 : 1174.7;
                    m_clickBeat += 1;
                    m_clickEnv = 1.0;
                    m_clickPhase = 0;
                }
            }
            if (m_clickEnv > 1e-4) {
                const float v = (float)(std::sin(m_clickPhase) * m_clickEnv) * level;
                m_clickPhase += 2.0 * juce::MathConstants<double>::pi * m_clickHz / m_rate;
                m_clickEnv *= decay;
                for (int ch = 0; ch < chans; ++ch)
                    buffer.addSample(ch, i, v);
            }
        }
    }
    m_clickWasOn = click;
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
    root.setProperty("hideInactive", hideInactivePedals, nullptr);
    root.setProperty("classicView", classicView, nullptr);
    root.setProperty("tab", tab, nullptr);
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

    root.setProperty("stripOn", stripOn.load(), nullptr);
    root.setProperty("masterOn", masterOn.load(), nullptr);
    for (auto [name, proc] : {std::pair<const char *, YsfxProcessor *>{"strip", &m_strip}, {"master", &m_master}}) {
        juce::MemoryBlock mb;
        proc->getStateInformation(mb);
        juce::ValueTree t(name);
        t.setProperty("data", mb.toBase64Encoding(), nullptr);
        root.addChild(t, -1, nullptr);
    }

    juce::ValueTree amps("amps");
    for (auto &[path, mb] : m_ampStates) {
        juce::ValueTree a("amp");
        a.setProperty("file", juce::File(path).getFileName(), nullptr);
        a.setProperty("data", mb.toBase64Encoding(), nullptr);
        amps.addChild(a, -1, nullptr);
    }
    root.addChild(amps, -1, nullptr);
    if (m_surface)
        root.addChild(m_surface->getState(), -1, nullptr);

    juce::MemoryOutputStream stream(dest, false);
    root.writeToStream(stream);
}

void RigProcessor::setStateInformation(const void *data, int size)
{
    juce::ValueTree root = juce::ValueTree::readFromData(data, (size_t)size);
    if (!root.hasType("lmsrig"))
        return;
    hideInactivePedals = root.getProperty("hideInactive", false);
    classicView = root.getProperty("classicView", false);
    if (m_surface)
        m_surface->setState(root.getChildWithName("surface"));
    tab = juce::jlimit(0, 2, (int)root.getProperty("tab", 0));

    juce::MemoryBlock pedalState;
    if (pedalState.fromBase64Encoding(root.getChildWithName("pedals").getProperty("data").toString()))
        m_pedals.setStateInformation(pedalState.getData(), (int)pedalState.getSize());

    // pre-amp-only rigs have neither; the stages then keep their defaults, switched out
    stripOn = (bool)root.getProperty("stripOn", false);
    masterOn = (bool)root.getProperty("masterOn", false);
    for (auto [name, proc, file] : {std::tuple<const char *, YsfxProcessor *, const char *>{"strip", &m_strip, "lms_rtw.jsfx"},
                                    {"master", &m_master, "lms_mega_increasinator.jsfx"}}) {
        juce::MemoryBlock mb;
        if (!mb.fromBase64Encoding(root.getChildWithName(name).getProperty("data").toString()))
            continue;
        juce::ValueTree t = juce::ValueTree::readFromData(mb.getData(), mb.getSize());
        if (t.isValid()) {  // point the saved state at this machine's copy
            t.setProperty("path", m_lmsDir.getChildFile(file).getFullPathName(), nullptr);
            mb.reset();
            juce::MemoryOutputStream st(mb, false);
            t.writeToStream(st);
        }
        proc->setStateInformation(mb.getData(), (int)mb.getSize());
    }

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

using rigui::kBg;
using rigui::kPanel;
using rigui::kAccent;
using rigui::kText;

constexpr int kWidth = 1500;
constexpr int kPedalHeight = 580;
constexpr int kPedalHeader = 122;             // JSFX title + tuner strip, kept above the native board
constexpr int kBottomHeight = 800;
constexpr int kSideWidth = 340;               // head picker + cab, right of the amp
constexpr int kAmpArea = kWidth - kSideWidth;  // amp is scaled to fill this
constexpr int kHeight = kPedalHeight + kBottomHeight;
constexpr int kTabH = 46;                     // AMP / CHANNEL STRIP / MASTER, above the bottom panels
constexpr int kStageY = kPedalHeight + kTabH;
constexpr int kStageH = kBottomHeight - kTabH;

const juce::Colour kStripAccent{0xff2fa4a8};
const juce::Colour kMasterAccent{0xffe0a020};

// RTW, bound by slider name
const std::vector<rigui::RackSectionDef> kStripSections = {
    {"PREAMP", nullptr, nullptr, nullptr, {"Preamp Drive (dB)|Drive", "Saturation Mode|Saturation"}},
    {"FILTERS", nullptr, nullptr, nullptr, {"HPF", "HPF Freq (Hz)|HPF Freq", "LPF", "LPF Freq (Hz)|LPF Freq"}},
    {"EQ", nullptr, nullptr, nullptr,
     {"Low Shelf (dB)|Low", "Low Freq (Hz)|Low Freq", "Low-Mid (dB)|Low-Mid", "Low-Mid Freq (Hz)|LM Freq", "Low-Mid Q|LM Q",
      "Hi-Mid (dB)|Hi-Mid", "Hi-Mid Freq (Hz)|HM Freq", "Hi-Mid Q|HM Q", "High Shelf (dB)|High", "High Freq (Hz)|High Freq"}},
    {"COMPRESSOR", "Compressor", "comp_gr_db", "Comp ",
     {"Comp Threshold (dB)", "Comp Ratio", "Comp Attack (ms)", "Comp Release (ms)", "Auto Release", "Sidechain HPF|SC HPF",
      "SC HPF Freq (Hz)|SC Freq", "Comp Makeup (dB)", "Comp Mix (%)"}},
    {"REVERB", nullptr, nullptr, "Reverb ", {"Reverb Type", "Reverb Mix (%)", "Reverb Tone", "Reverb Position"}},
    {"GATE", "Gate", nullptr, "Gate ", {"Gate Threshold (dB)", "Gate Hold (ms)", "Gate Release (ms)"}},
    {"OUTPUT", nullptr, nullptr, nullptr, {"Output Gain (dB)|Output"}},
};

// Mega Increasinator, in signal order
const std::vector<rigui::RackSectionDef> kMasterSections = {
    {"INPUT", nullptr, nullptr, nullptr, {"Input Gain (dB)|Input", "Lo Crossover (Hz) [0=Off]|Lo Xover"}},
    {"FET", "FET", "m_fet_gr", "FET ",
     {"FET Threshold (dB)", "FET Ratio", "FET Attack (ms)", "FET Release (ms)", "FET Output (dB)", "FET Drive", "FET Mix (%)"}},
    {"OPTO", "OPTO", "m_opto_gr", "Opto ",
     {"Opto Peak Reduction (%)|Peak Red", "Opto Gain (dB)", "Opto Mode", "Opto Emphasis (%)", "Opto Warmth (%)", "Opto Mix (%)"}},
    {"VCA", "VCA", "m_vca_gr", "VCA ",
     {"VCA Threshold (dB)", "VCA Ratio", "VCA Attack (ms)", "VCA Release (ms)", "VCA Auto Release|Auto Rel",
      "VCA SC HPF (Hz) [0=Off]|SC HPF", "VCA Makeup (dB)", "VCA Mix (%)"}},
    {"REACTIVE EQ", nullptr, nullptr, nullptr, {"EQ Profile|Profile", "EQ Intensity (%)|Intensity"}},
    {"LIMITER", nullptr, "m_lim_gr", nullptr, {"Target Loudness (dB)|Target", "Ceiling (dBFS)|Ceiling", "Release (ms)|Release"}},
};

const std::vector<rigui::RackReadout> kMasterReadouts = {
    {"LUFS S", "lufs_s", false},
    {"LUFS I", "lufs_i", false},
    {"PEAK", "out_peak", true},
};

// Cab load vs the amp's impedance tap, read from the head's own variables
struct MatchLight : juce::Component {
    double load = 0, tap = 0;

    void set(double newLoad, double newTap)
    {
        if (newLoad != load || newTap != tap) {
            load = newLoad;
            tap = newTap;
            repaint();
        }
    }

    void paint(juce::Graphics &g) override
    {
        const bool known = load > 0 && tap > 0;
        const bool matched = known && std::abs(load - tap) < 0.01;
        const juce::Colour c = !known ? rigui::kDim : matched ? juce::Colour(0xff3fbf5f) : juce::Colour(0xffe0a020);
        auto r = getLocalBounds().toFloat();
        auto led = r.removeFromLeft(r.getHeight()).reduced(r.getHeight() * 0.28f);
        g.setColour(c.withAlpha(0.35f));
        g.fillEllipse(led.expanded(led.getWidth() * 0.45f));
        g.setColour(c);
        g.fillEllipse(led);
        r.removeFromLeft(8);

        auto ohms = [](double v) { return juce::String(v, v == std::floor(v) ? 0 : 1) + juce::String(juce::CharPointer_UTF8(" \u03a9")); };
        juce::String text = !known ? juce::String("No cab load")
                          : (matched ? "MATCHED  " : "MISMATCH  ") + ohms(load) + " cab on " + ohms(tap) + " tap";
        g.setColour(matched ? rigui::kText : c);
        g.setFont(juce::FontOptions(14.0f, juce::Font::bold));
        g.drawText(text, r, juce::Justification::centredLeft);
    }
};

// Pedalboard out -> amp in: peak meter with a latching CLIP light (click to clear)
struct InputMeter : juce::Component {
    float level = 0.0f;  // displayed peak, linear
    int clipTicks = 0;   // timer ticks the CLIP light stays lit

    void push(float peak)
    {
        level = juce::jmax(peak, level * 0.82f);  // ~15 Hz ticks: falls ~20 dB/s
        if (peak >= 1.0f)
            clipTicks = 45;  // 3 s
        else if (clipTicks > 0)
            --clipTicks;
        repaint();
    }

    void mouseUp(const juce::MouseEvent &) override
    {
        clipTicks = 0;
        repaint();
    }

    void paint(juce::Graphics &g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour(rigui::kDim);
        g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
        g.drawText("AMP IN", r.removeFromLeft(60.0f), juce::Justification::centredLeft);

        // CLIP light on the right
        auto clip = r.removeFromRight(64.0f).reduced(2.0f, 4.0f);
        const bool lit = clipTicks > 0;
        g.setColour(lit ? juce::Colour(0xffe02020) : juce::Colour(0xff2a2222));
        g.fillRoundedRectangle(clip, 4.0f);
        g.setColour(lit ? juce::Colours::white : rigui::kDim);
        g.drawText("CLIP", clip, juce::Justification::centred);
        r.removeFromRight(8.0f);

        // dB readout
        const float db = level > 1e-6f ? 20.0f * std::log10(level) : -100.0f;
        g.setColour(db >= 0.0f ? juce::Colour(0xffe02020) : rigui::kText);
        g.drawText(db > -60.0f ? juce::String(db, 1) + " dB" : juce::String("-inf"), r.removeFromRight(66.0f),
                   juce::Justification::centredRight);
        r.removeFromRight(8.0f);

        // bar: -48..+6 dBFS, green to -6, amber to 0, red above
        auto bar = r.reduced(0.0f, 9.0f);
        auto x = [&](float d) { return bar.getX() + bar.getWidth() * (d + 48.0f) / 54.0f; };
        g.setColour(juce::Colour(0xff141010));
        g.fillRoundedRectangle(bar, 3.0f);
        const float fill = juce::jlimit(-48.0f, 6.0f, db);
        auto seg = [&](float from, float to, juce::Colour c) {
            if (fill > from)
                g.setColour(c), g.fillRect(juce::Rectangle<float>(x(from), bar.getY(), x(juce::jmin(fill, to)) - x(from), bar.getHeight()));
        };
        seg(-48.0f, -6.0f, juce::Colour(0xff3fbf5f));
        seg(-6.0f, 0.0f, juce::Colour(0xffe0a020));
        seg(0.0f, 6.0f, juce::Colour(0xffe02020));
        g.setColour(rigui::kText.withAlpha(0.6f));
        g.fillRect(x(0.0f), bar.getY() - 3.0f, 1.5f, bar.getHeight() + 6.0f);  // 0 dBFS mark
        g.setColour(juce::Colours::black.withAlpha(0.6f));
        g.drawRoundedRectangle(bar, 3.0f, 1.0f);
    }
};

class RigEditor final : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit RigEditor(RigProcessor &p)
        : AudioProcessorEditor(p), m_proc(p),
          m_stripPanel(kStripSections, {}, kStripAccent),
          m_masterPanel(kMasterSections, kMasterReadouts, kMasterAccent)
    {
        setLookAndFeel(&m_lnf);

        addAndMakeVisible(m_content);
        m_content.setSize(kWidth, kHeight);
        m_content.addAndMakeVisible(m_pedalClip);
        m_pedalClip.addAndMakeVisible(m_pedalView);
        m_content.addChildComponent(m_pedalPanel);
        m_content.addChildComponent(m_ampView);
        m_content.addChildComponent(m_ampPanel);
        m_content.addChildComponent(m_stripView);
        m_content.addChildComponent(m_stripPanel);
        m_content.addChildComponent(m_masterView);
        m_content.addChildComponent(m_masterPanel);

        // AMP / CHANNEL STRIP / MASTER
        for (int i = 0; i < 3; ++i) {
            m_tabs[i].setClickingTogglesState(true);
            m_tabs[i].setRadioGroupId(1);
            m_tabs[i].setColour(juce::TextButton::buttonOnColourId, i == 0 ? kAccent : i == 1 ? kStripAccent : kMasterAccent);
            m_tabs[i].onClick = [this, i] {
                if (m_tabs[i].getToggleState()) {
                    m_proc.tab = i;
                    applyView();
                }
            };
            m_content.addAndMakeVisible(m_tabs[i]);
        }
        m_tabs[m_proc.tab].setToggleState(true, juce::dontSendNotification);
        m_stripPanel.inChain().onClick = [this] { m_proc.stripOn = m_stripPanel.inChain().getToggleState(); updateTabs(); };
        m_masterPanel.inChain().onClick = [this] { m_proc.masterOn = m_masterPanel.inChain().getToggleState(); updateTabs(); };
        m_pedalPanel.onHideInactiveChanged = [this](bool hide) { m_proc.hideInactivePedals = hide; };

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
        setupLabel(m_headTitle, "AMP HEAD", 20.0f);
        setupLabel(m_cabTitle, "CABINET", 20.0f);
        setupLabel(m_aLabel, "A", 15.0f);
        setupLabel(m_bLabel, "B", 15.0f);
        setupLabel(m_micALabel, "Mic A", 13.0f);
        setupLabel(m_micBLabel, "Mic B", 13.0f);
        setupLabel(m_breakupLabel, "Breakup", 13.0f);

        m_typeA.onChange = [this] { m_proc.setCabValue(cabType, m_typeA.getSelectedItemIndex()); };
        m_typeB.onChange = [this] { m_proc.setCabValue(cabTypeB, m_typeB.getSelectedItemIndex()); };
        m_cabOn.setButtonText("Cab sim");
        m_cabOn.onClick = [this] { m_proc.setCabValue(cabOn, m_cabOn.getToggleState() ? 1.0 : 0.0); };
        setupKnob(m_micA, cabMic);
        setupKnob(m_micB, cabMicB);
        setupKnob(m_breakup, cabBreakup);
        m_classic.setButtonText("Classic view");
        m_classic.setToggleState(m_proc.classicView, juce::dontSendNotification);
        m_classic.onClick = [this] { m_proc.classicView = m_classic.getToggleState(); applyView(); };

        // record an idea
        setupLabel(m_recTitle, "IDEAS", 20.0f);
        m_rec.setClickingTogglesState(true);
        m_rec.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffe02020));
        m_rec.setToggleState(m_proc.isRecording(), juce::dontSendNotification);
        m_rec.onClick = [this] {
            if (m_rec.getToggleState()) {
                if (!m_proc.startRecording())
                    m_rec.setToggleState(false, juce::dontSendNotification);
            } else {
                m_proc.stopRecording();
            }
            updateRec();
        };
        m_recFolder.setButtonText("FOLDER");
        m_recFolder.onClick = [] { RigProcessor::revealDir(RigProcessor::recordingsDir()); };
        setupLabel(m_recInfo, "", 13.0f);
        m_recInfo.setColour(juce::Label::textColourId, rigui::kDim);
        m_recInfo.setMinimumHorizontalScale(0.6f);

        // the ideas played on this head, newest first
        m_ideaBox.setJustificationType(juce::Justification::centredLeft);
        m_ideaBox.setTextWhenNoChoicesAvailable("no ideas on this head yet");
        m_ideaBox.setTextWhenNothingSelected("pick an idea");
        m_play.setClickingTogglesState(true);
        m_play.setColour(juce::TextButton::buttonOnColourId, kStripAccent);
        m_play.onClick = [this] {
            if (m_play.getToggleState()) {
                int i = m_ideaBox.getSelectedItemIndex();
                if (i < 0 || i >= (int)m_ideas.size() || !m_proc.playIdea(m_ideas[(size_t)i]))
                    m_play.setToggleState(false, juce::dontSendNotification);
            } else {
                m_proc.stopIdea();
            }
            updateRec();
        };
        m_reaper.setButtonText("REAPER");
        m_reaper.onClick = [this] {
            int i = m_ideaBox.getSelectedItemIndex();
            if (i < 0 || i >= (int)m_ideas.size())
                return;
            juce::String err;
            if (!m_proc.openInReaper(m_ideas[(size_t)i], err))
                m_recInfo.setText(err, juce::dontSendNotification);
            else
                m_recInfo.setText("opened in REAPER: Idea " + m_ideas[(size_t)i].stamp, juce::dontSendNotification);
            m_recInfoHold = juce::Time::currentTimeMillis() + 4000;
        };

        // the click
        m_clickOn.setButtonText("CLICK");
        m_clickOn.setColour(juce::ToggleButton::tickColourId, kMasterAccent);
        m_clickOn.setToggleState(m_proc.clickOn, juce::dontSendNotification);
        m_clickOn.onClick = [this] { m_proc.clickOn = m_clickOn.getToggleState(); };
        m_bpm.setSliderStyle(juce::Slider::LinearBar);
        m_bpm.setRange(40.0, 240.0, 1.0);
        m_bpm.setValue(m_proc.clickBpm, juce::dontSendNotification);
        m_bpm.setTextValueSuffix(" bpm");
        m_bpm.setColour(juce::Slider::trackColourId, kMasterAccent.withAlpha(0.6f));
        m_bpm.onValueChange = [this] { m_proc.clickBpm = m_bpm.getValue(); };
        m_clickLevel.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        m_clickLevel.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        m_clickLevel.setRange(0.0, 1.0, 0.01);
        m_clickLevel.setValue(m_proc.clickLevel, juce::dontSendNotification);
        m_clickLevel.setColour(juce::Slider::rotarySliderFillColourId, kMasterAccent);
        m_clickLevel.onValueChange = [this] { m_proc.clickLevel = (float)m_clickLevel.getValue(); };

        refreshIdeas();
        updateRec();

        m_content.addAndMakeVisible(m_inMeter);
        for (auto *c : std::initializer_list<juce::Component *>{&m_typeA, &m_typeB, &m_match, &m_cabOn, &m_classic,
                                                                  &m_rec, &m_recFolder, &m_ideaBox, &m_play, &m_reaper,
                                                                  &m_clickOn, &m_bpm, &m_clickLevel})
            m_content.addAndMakeVisible(c);

        layoutContent();
        refreshAll();
        applyView();
        m_content.sendLookAndFeelChange();

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
        m_stripView.setEffect(nullptr);
        m_masterView.setEffect(nullptr);
        setLookAndFeel(nullptr);
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
            g.fillRect(kAmpArea, kPedalHeight, kSideWidth, kBottomHeight);
            g.setColour(kAccent);
            g.fillRect(0, kPedalHeight, kWidth, 2);
            g.fillRect(kAmpArea, kPedalHeight, 2, kBottomHeight);
            g.setColour(kAccent.withAlpha(0.4f));
            g.fillRect(kAmpArea + 20, kPedalHeight + 120, kSideWidth - 40, 1);
            g.fillRect(kAmpArea + 20, kHeight - 70, kSideWidth - 40, 1);
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
        s.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 60, 18);
        s.setRange(0.0, 100.0, 1.0);
        s.setTextValueSuffix(" %");
        s.onValueChange = [this, &s, k] { m_proc.setCabValue(k, s.getValue()); };
        m_content.addAndMakeVisible(s);
    }

    // native panels, or the JSFX's own UIs (the pedalboard header + tuner stay JSFX either way)
    void applyView()
    {
        const bool classic = m_proc.classicView;
        const int tab = m_proc.tab;
        m_pedalClip.setBounds(0, 0, kWidth, classic ? kPedalHeight : kPedalHeader);
        m_pedalPanel.setVisible(!classic);
        m_ampPanel.setVisible(!classic && tab == 0);
        m_ampView.setVisible(classic && tab == 0);
        m_stripPanel.setVisible(!classic && tab == 1);
        m_stripView.setVisible(classic && tab == 1);
        m_masterPanel.setVisible(!classic && tab == 2);
        m_masterView.setVisible(classic && tab == 2);
        updateTabs();
    }

    void updateTabs()
    {
        // a dot on the tab when that stage is in the chain
        const juce::String dot = juce::CharPointer_UTF8("  \xe2\x97\x8f");
        m_tabs[0].setButtonText("AMP");
        m_tabs[1].setButtonText("CHANNEL STRIP" + (m_proc.stripOn ? dot : juce::String()));
        m_tabs[2].setButtonText("MASTER" + (m_proc.masterOn ? dot : juce::String()));
        m_stripPanel.inChain().setToggleState(m_proc.stripOn, juce::dontSendNotification);
        m_masterPanel.inChain().setToggleState(m_proc.masterOn, juce::dontSendNotification);
    }

    void layoutContent()
    {
        m_pedalView.setBounds(0, 0, kWidth, kPedalHeight);
        m_pedalPanel.setBounds(0, kPedalHeader, kWidth, kPedalHeight - kPedalHeader);
        m_ampPanel.setBounds(0, kStageY, kAmpArea, kStageH);
        m_stripPanel.setBounds(0, kStageY, kAmpArea, kStageH);
        m_masterPanel.setBounds(0, kStageY, kAmpArea, kStageH);
        const int tabW = 220;
        for (int i = 0; i < 3; ++i)
            m_tabs[i].setBounds(20 + i * (tabW + 8), kPedalHeight + 10, tabW, kTabH - 12);
        const int meterX = 20 + 3 * (tabW + 8) + 16;
        m_inMeter.setBounds(meterX, kPedalHeight + 8, kAmpArea - 20 - meterX, kTabH - 8);
        layoutView(m_ampView, m_proc.amp());
        layoutView(m_stripView, m_proc.strip());
        layoutView(m_masterView, m_proc.master());

        const int x0 = kAmpArea + 20, w = kSideWidth - 40;
        int y = kPedalHeight + 20;
        m_headTitle.setBounds(x0, y, w, 28);
        y += 40;
        m_prev.setBounds(x0, y, 36, 34);
        m_ampBox.setBounds(x0 + 42, y, w - 84, 34);
        m_next.setBounds(x0 + w - 36, y, 36, 34);

        y = kPedalHeight + 140;
        m_cabTitle.setBounds(x0, y, w, 28);
        y += 44;
        m_aLabel.setBounds(x0, y, 24, 30);
        m_typeA.setBounds(x0 + 30, y, w - 30, 30);
        y += 40;
        m_bLabel.setBounds(x0, y, 24, 30);
        m_typeB.setBounds(x0 + 30, y, w - 30, 30);
        y += 38;
        m_match.setBounds(x0 + 30, y, w - 30, 24);
        y += 40;

        const int colW = w / 3, knob = 90;
        juce::Label *labels[] = {&m_micALabel, &m_micBLabel, &m_breakupLabel};
        juce::Slider *knobs[] = {&m_micA, &m_micB, &m_breakup};
        for (int i = 0; i < 3; ++i) {
            int cx = x0 + colW * i + colW / 2;
            labels[i]->setBounds(cx - colW / 2, y, colW, 20);
            knobs[i]->setBounds(cx - knob / 2, y + 22, knob, knob + 18);
        }
        y += 22 + knob + 18 + 30;
        m_cabOn.setBounds(x0 + w / 2 - 55, y, 110, 30);

        m_recTitle.setBounds(x0, kHeight - 250, w, 28);
        m_rec.setBounds(x0, kHeight - 214, w - 96, 40);
        m_recFolder.setBounds(x0 + w - 88, kHeight - 214, 88, 40);
        m_recInfo.setBounds(x0, kHeight - 170, w, 20);
        m_ideaBox.setBounds(x0, kHeight - 146, w - 150, 32);
        m_play.setBounds(x0 + w - 144, kHeight - 146, 44, 32);
        m_reaper.setBounds(x0 + w - 96, kHeight - 146, 96, 32);
        m_clickOn.setBounds(x0, kHeight - 108, 84, 30);
        m_bpm.setBounds(x0 + 90, kHeight - 108, w - 90 - 50, 30);
        m_clickLevel.setBounds(x0 + w - 44, kHeight - 112, 40, 40);

        m_classic.setBounds(x0 + w / 2 - 70, kHeight - 50, 140, 30);
    }

    static juce::String clock(double seconds)
    {
        int s = (int)seconds;
        return juce::String(s / 60) + ":" + juce::String(s % 60).paddedLeft('0', 2);
    }

    void refreshIdeas()
    {
        const juce::String head = m_proc.ampIndex() >= 0 ? juce::File(m_proc.ampNames()[m_proc.ampIndex()]).getFileName() : juce::String();
        m_ideas = RigProcessor::ideas(m_ampFileNow());
        const int keep = m_ideaBox.getSelectedItemIndex();
        m_ideaBox.clear(juce::dontSendNotification);
        for (size_t i = 0; i < m_ideas.size(); ++i) {
            const auto &idea = m_ideas[i];
            juce::String label = juce::Time(idea.time).formatted("%b %d  %H:%M") + "   " + clock(idea.seconds);
            if (idea.bpm > 0)
                label += "   " + juce::String((int)idea.bpm) + " bpm";
            m_ideaBox.addItem(label, (int)i + 1);
        }
        if (!m_ideas.empty())
            m_ideaBox.setSelectedItemIndex(juce::jlimit(0, (int)m_ideas.size() - 1, keep < 0 ? 0 : keep), juce::dontSendNotification);
        juce::ignoreUnused(head);
    }

    juce::String m_ampFileNow() const
    {
        // RigProcessor keeps the full paths privately; the JSON stores the file name
        return m_proc.ampFileName();
    }

    void updateRec()
    {
        const bool on = m_proc.isRecording();
        if (m_rec.getToggleState() != on)
            m_rec.setToggleState(on, juce::dontSendNotification);
        m_rec.setButtonText(on ? "STOP  " + clock(m_proc.recordedSeconds()) : "RECORD IDEA");
        if (m_wasRecording && !on)
            refreshIdeas();  // the take just landed
        m_wasRecording = on;

        const bool playing = m_proc.isPlaying();
        if (m_play.getToggleState() != playing)
            m_play.setToggleState(playing, juce::dontSendNotification);
        m_play.setButtonText(playing ? juce::String(juce::CharPointer_UTF8("\xe2\x96\xa0")) : juce::String(juce::CharPointer_UTF8("\xe2\x96\xb6")));

        if (juce::Time::currentTimeMillis() < m_recInfoHold)
            return;
        auto f = m_proc.lastRecording();
        m_recInfo.setText(on ? f.getFileName()
                          : playing ? "playing  " + clock(m_proc.playSeconds()) + " / " + clock(m_proc.playLength())
                          : f.existsAsFile() ? "Saved " + f.getFileName() + "  (" + clock(m_proc.recordedSeconds()) + ")  + DI"
                                             : juce::String(m_ideas.size()) + " idea" + (m_ideas.size() == 1 ? "" : "s") + " on this head",
                          juce::dontSendNotification);
    }

    void layoutView(YsfxGraphicsView &view, YsfxProcessor &proc)
    {
        // scale the JSFX canvas to fill the stage area; the JSFX scale themselves off gfx_w
        int w = 600, h = 564;
        if (auto info = proc.getCurrentInfo()) {
            uint32_t dim[2] = {};
            if (info->effect && ysfx_get_gfx_dim(info->effect.get(), dim) && dim[0] && dim[1]) {
                w = (int)dim[0];
                h = (int)dim[1];
            }
        }
        const int pad = 16;
        double s = juce::jmin((kAmpArea - 2.0 * pad) / w, (kStageH - 2.0 * pad) / h);
        int sw = juce::roundToInt(w * s), sh = juce::roundToInt(h * s);
        view.setBounds((kAmpArea - sw) / 2, kStageY + (kStageH - sh) / 2, sw, sh);
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
        updateRec();
        if (auto info = m_proc.pedals().getCurrentInfo(); info != m_pedalInfo) {
            m_pedalInfo = info;
            m_pedalView.setEffect(info ? info->effect.get() : nullptr);
            m_pedalPanel.build(m_proc.pedals());
            m_pedalPanel.setHideInactive(m_proc.hideInactivePedals);
            m_pedalPanel.sendLookAndFeelChange();  // controls were made before they had our L&F
        }
        if (auto info = m_proc.amp().getCurrentInfo(); info != m_ampInfo) {
            m_ampInfo = info;
            m_ampBox.setSelectedItemIndex(m_proc.ampIndex(), juce::dontSendNotification);  // head may change under us (state load)
            m_ampView.setEffect(info ? info->effect.get() : nullptr);
            layoutView(m_ampView, m_proc.amp());
            m_ampPanel.build(m_proc.amp(), juce::StringArray(kCabSliderNames, numCabKeys));
            m_ampPanel.sendLookAndFeelChange();
            fillCabTypes(m_typeA, cabType);
            fillCabTypes(m_typeB, cabTypeB);
            refreshCab();
            ysfx_t *fx = info ? info->effect.get() : nullptr;
            m_loadVar = fx ? ysfx_find_var(fx, "cab_load_ohms") : nullptr;
            m_tapVar = fx ? ysfx_find_var(fx, "amp_designed_ohms") : nullptr;
            refreshIdeas();  // a different head shows its own ideas
        }
        if (auto info = m_proc.strip().getCurrentInfo(); info != m_stripInfo) {
            m_stripInfo = info;
            m_stripView.setEffect(info ? info->effect.get() : nullptr);
            layoutView(m_stripView, m_proc.strip());
            m_stripPanel.build(m_proc.strip());
            m_stripPanel.sendLookAndFeelChange();
            updateTabs();  // a state load may have switched the stages
        }
        if (auto info = m_proc.master().getCurrentInfo(); info != m_masterInfo) {
            m_masterInfo = info;
            m_masterView.setEffect(info ? info->effect.get() : nullptr);
            layoutView(m_masterView, m_proc.master());
            m_masterPanel.build(m_proc.master());
            m_masterPanel.sendLookAndFeelChange();
            updateTabs();
        }
        m_inMeter.push(m_proc.ampInPeak.exchange(0.0f));
        m_match.set(m_loadVar ? *m_loadVar : 0.0, m_tapVar ? *m_tapVar : 0.0);
        if (m_proc.pollCabFromAmp())
            refreshCab();
        if (!m_proc.classicView) {
            m_pedalPanel.sync();
            if (m_proc.tab == 0)
                m_ampPanel.sync();
            else if (m_proc.tab == 1)
                m_stripPanel.sync();
            else
                m_masterPanel.sync();
        }
    }

    RigProcessor &m_proc;
    rigui::RigLookAndFeel m_lnf;
    YsfxInfo::Ptr m_pedalInfo, m_ampInfo, m_stripInfo, m_masterInfo;
    ysfx_real *m_loadVar = nullptr, *m_tapVar = nullptr;  // live in the head's VM

    Content m_content;
    juce::Component m_pedalClip;  // shows the whole JSFX board, or just its header + tuner
    YsfxGraphicsView m_pedalView, m_ampView, m_stripView, m_masterView;
    rigui::PedalboardPanel m_pedalPanel;
    rigui::AmpPanel m_ampPanel;
    rigui::RackPanel m_stripPanel, m_masterPanel;
    juce::TextButton m_tabs[3];
    juce::TextButton m_prev, m_next;
    rigui::ClickCombo m_ampBox;

    juce::Label m_headTitle, m_cabTitle, m_aLabel, m_bLabel, m_micALabel, m_micBLabel, m_breakupLabel;
    rigui::ClickCombo m_typeA, m_typeB;
    MatchLight m_match;
    InputMeter m_inMeter;
    juce::ToggleButton m_cabOn, m_classic;
    juce::Slider m_micA, m_micB, m_breakup;
    juce::Label m_recTitle, m_recInfo;
    juce::TextButton m_rec, m_recFolder, m_play, m_reaper;
    rigui::ClickCombo m_ideaBox;
    juce::ToggleButton m_clickOn;
    juce::Slider m_bpm, m_clickLevel;
    std::vector<RigProcessor::Idea> m_ideas;
    bool m_wasRecording = false;
    int64_t m_recInfoHold = 0;
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
