// LMS Rig — see rig_ui.h

#include "rig_ui.h"
#include "processor.h"
#include "parameter.h"
#include "ysfx.h"

namespace rigui {

//==============================================================================
// slider names: "-DOA Drive (%)" -> name "DOA Drive (%)", unit "%", bare "Drive"

static juce::String unitOf(const juce::String &name)
{
    juce::String u = name.fromLastOccurrenceOf("(", false, false).upToFirstOccurrenceOf(")", false, false);
    if (!name.trimEnd().endsWith(")") || u.isEmpty())
        return {};
    return u == "%" ? u : " " + u;
}

static juce::String bareName(juce::String name, const juce::String &prefix = {})
{
    if (prefix.isNotEmpty() && name.startsWith(prefix))
        name = name.substring(prefix.length());
    if (name.trimEnd().endsWith(")"))
        name = name.upToLastOccurrenceOf("(", false, false);
    return name.trim();
}

// existing sliders of an effect in index order
static std::vector<std::pair<int, juce::String>> listSliders(YsfxProcessor &proc)
{
    std::vector<std::pair<int, juce::String>> out;
    auto info = proc.getCurrentInfo();
    if (!info || !info->effect)
        return out;
    ysfx_t *fx = info->effect.get();
    for (uint32_t i = 0; i < ysfx_max_sliders; ++i)
        if (ysfx_slider_exists(fx, i))
            out.emplace_back((int)i, juce::String(juce::CharPointer_UTF8(ysfx_slider_get_name(fx, i))).trimCharactersAtStart("-").trim());
    return out;
}

static void drawLed(juce::Graphics &g, juce::Rectangle<float> r, bool on, juce::Colour c)
{
    if (on) {
        g.setColour(c.withAlpha(0.35f));
        g.fillEllipse(r.expanded(r.getWidth() * 0.45f));
        g.setColour(c.brighter(0.3f));
    } else {
        g.setColour(juce::Colour(0xff2a2222));
    }
    g.fillEllipse(r);
    g.setColour(juce::Colours::black.withAlpha(0.7f));
    g.drawEllipse(r, 1.0f);
}

//==============================================================================
RigLookAndFeel::RigLookAndFeel()
{
    setColour(juce::ComboBox::backgroundColourId, kPanel.brighter(0.08f));
    setColour(juce::ComboBox::outlineColourId, kAccent.withAlpha(0.6f));
    setColour(juce::ComboBox::textColourId, kText);
    setColour(juce::ComboBox::arrowColourId, kText);
    setColour(juce::PopupMenu::backgroundColourId, kPanel);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, kAccent);
    setColour(juce::TextButton::buttonColourId, kPanel.brighter(0.08f));
    setColour(juce::TextButton::buttonOnColourId, kAccent);
    setColour(juce::TextButton::textColourOffId, kText);
    setColour(juce::TextButton::textColourOnId, kText);
    setColour(juce::Slider::thumbColourId, kAccent);
    setColour(juce::Slider::rotarySliderFillColourId, kAccent);
    setColour(juce::Slider::trackColourId, kAccent.withAlpha(0.7f));
    setColour(juce::Slider::textBoxTextColourId, kText);
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::Label::textColourId, kText);
    setColour(juce::ToggleButton::textColourId, kText);
    setColour(juce::ToggleButton::tickColourId, kAccent);
}

void RigLookAndFeel::drawRotarySlider(juce::Graphics &g, int x, int y, int w, int h, float pos, float start,
                                      float end, juce::Slider &s)
{
    const float r = juce::jmin(w, h) * 0.5f - 3.0f;
    if (r < 4.0f)
        return;
    const float cx = x + w * 0.5f, cy = y + h * 0.5f;
    const float angle = start + pos * (end - start);
    const float arcW = juce::jmax(2.0f, r * 0.13f);
    const juce::Colour accent = s.findColour(juce::Slider::rotarySliderFillColourId);

    juce::Path track;
    track.addCentredArc(cx, cy, r, r, 0.0f, start, end, true);
    g.setColour(juce::Colour(0xff2c2424));
    g.strokePath(track, juce::PathStrokeType(arcW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    juce::Path value;
    value.addCentredArc(cx, cy, r, r, 0.0f, start, angle, true);
    g.setColour(accent);
    g.strokePath(value, juce::PathStrokeType(arcW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const float kr = r * 0.74f;
    juce::Rectangle<float> body(cx - kr, cy - kr, kr * 2, kr * 2);
    g.setColour(juce::Colours::black.withAlpha(0.5f));
    g.fillEllipse(body.translated(0, kr * 0.08f).expanded(1.5f));
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff4a4040), cx, cy - kr, juce::Colour(0xff161111), cx, cy + kr, false));
    g.fillEllipse(body);
    g.setColour(juce::Colour(0xff5c5050));
    g.drawEllipse(body.reduced(0.5f), 1.0f);

    const float lw = juce::jmax(2.0f, kr * 0.14f);
    juce::Path pointer;
    pointer.addRoundedRectangle(-lw * 0.5f, -kr * 0.92f, lw, kr * 0.5f, lw * 0.5f);
    g.setColour(kText);
    g.fillPath(pointer, juce::AffineTransform::rotation(angle).translated(cx, cy));
}

void RigLookAndFeel::drawToggleButton(juce::Graphics &g, juce::ToggleButton &b, bool highlighted, bool down)
{
    const bool on = b.getToggleState();
    const juce::Colour accent = b.findColour(juce::ToggleButton::tickColourId);
    auto area = b.getLocalBounds().toFloat();

    if (b.getComponentID() == "footswitch") {
        const float led = juce::jmin(12.0f, area.getHeight() * 0.15f);
        drawLed(g, juce::Rectangle<float>(led, led).withCentre({area.getCentreX(), area.getY() + led}), on, accent);
        area.removeFromTop(led * 2.4f);
        const float d = juce::jmin(area.getWidth(), area.getHeight()) * 0.85f;
        auto c = juce::Rectangle<float>(d, d).withCentre(area.getCentre());
        g.setColour(juce::Colours::black.withAlpha(0.6f));
        g.fillEllipse(c.translated(0, 2.0f));
        g.setGradientFill(juce::ColourGradient(juce::Colour(down ? 0xff9a9a9a : 0xffdcdcdc), c.getX(), c.getY(),
                                               juce::Colour(0xff4a4a4a), c.getRight(), c.getBottom(), false));
        g.fillEllipse(c);
        auto inner = c.reduced(d * 0.18f);
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff5a5a5a), inner.getX(), inner.getY(),
                                               juce::Colour(highlighted ? 0xffcfcfcf : 0xffb0b0b0), inner.getRight(), inner.getBottom(), false));
        g.fillEllipse(inner);
        return;
    }

    const float d = juce::jmin(14.0f, area.getHeight() - 4.0f);
    if (b.getButtonText().isEmpty()) {
        drawLed(g, juce::Rectangle<float>(d, d).withCentre(area.getCentre()), on, accent);
        return;
    }
    drawLed(g, juce::Rectangle<float>(area.getX() + 4, area.getCentreY() - d * 0.5f, d, d), on, accent);
    g.setColour(b.findColour(juce::ToggleButton::textColourId).withAlpha(b.isEnabled() ? 1.0f : 0.5f));
    g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
    g.drawFittedText(b.getButtonText(), area.withTrimmedLeft(d + 10).toNearestInt(), juce::Justification::centredLeft, 1);
}

//==============================================================================
ParamControl::ParamControl(YsfxParameter *p, const juce::String &label, juce::Colour accent)
    : m_param(p)
{
    const juce::String name = juce::String(p->getSliderName()).trimCharactersAtStart("-").trim();
    if (p->isEnumSlider()) {
        bool onOff = p->getSliderEnumSize() == 2 && juce::String(p->getSliderEnumName(0)) == "Off" &&
                     juce::String(p->getSliderEnumName(1)) == "On";
        m_kind = onOff ? toggle : choice;
    }

    m_label.setText(label, juce::dontSendNotification);
    m_label.setFont(juce::FontOptions(12.0f, juce::Font::bold));
    m_label.setJustificationType(juce::Justification::centred);
    m_label.setColour(juce::Label::textColourId, kText.withAlpha(0.85f));
    m_label.setInterceptsMouseClicks(false, false);
    m_label.setMinimumHorizontalScale(0.7f);
    addAndMakeVisible(m_label);

    switch (m_kind) {
    case knob: {
        auto r = p->getSliderRange();
        const juce::String unit = unitOf(name);
        const int decimals = (r.inc > 0 && r.inc < 1) ? 1 : 0;
        m_slider.textFromValueFunction = [unit, decimals](double v) {
            return (decimals ? juce::String(v, decimals) : juce::String(juce::roundToInt(v))) + unit;
        };
        m_slider.valueFromTextFunction = [](const juce::String &t) {
            return t.retainCharacters("-0123456789.").getDoubleValue();
        };
        m_slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        m_slider.setRange(r.min, r.max, r.inc > 0 ? r.inc : 0.0);
        m_slider.setDoubleClickReturnValue(true, r.def);
        m_slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 64, 16);
        m_slider.setColour(juce::Slider::rotarySliderFillColourId, accent);
        m_slider.updateText();
        m_slider.onDragStart = [this] { m_param->beginChangeGesture(); };
        m_slider.onDragEnd = [this] { m_param->endChangeGesture(); };
        m_slider.onValueChange = [this] { set(m_slider.getValue()); };
        addAndMakeVisible(m_slider);
        break;
    }
    case toggle:
        m_button.setColour(juce::ToggleButton::tickColourId, accent);
        m_button.onClick = [this] {
            m_param->beginChangeGesture();
            set(m_button.getToggleState() ? 1.0 : 0.0);
            m_param->endChangeGesture();
        };
        addAndMakeVisible(m_button);
        break;
    case choice:
        for (int i = 0; i < p->getSliderEnumSize(); ++i)
            m_combo.addItem(juce::String(p->getSliderEnumName(i)), i + 1);
        m_combo.setJustificationType(juce::Justification::centred);
        m_combo.onChange = [this] {
            m_param->beginChangeGesture();
            set(m_combo.getSelectedItemIndex());
            m_param->endChangeGesture();
        };
        addAndMakeVisible(m_combo);
        break;
    }
    sync();
}

void ParamControl::makeFootswitch()
{
    m_button.setComponentID("footswitch");
    m_label.setVisible(false);
}

double ParamControl::value() const
{
    return m_param->convertToYsfxValue(m_param->getValue());
}

void ParamControl::set(double v)
{
    m_param->setValueNotifyingHost(m_param->convertFromYsfxValue(v));
}

void ParamControl::sync()
{
    const double v = value();
    switch (m_kind) {
    case knob:
        if (!m_slider.isMouseButtonDown() && std::abs(m_slider.getValue() - v) > 1e-6)
            m_slider.setValue(v, juce::dontSendNotification);
        break;
    case toggle:
        m_button.setToggleState(v > 0.5, juce::dontSendNotification);
        break;
    case choice:
        if (int i = (int)std::lround(v); m_combo.getSelectedItemIndex() != i)
            m_combo.setSelectedItemIndex(i, juce::dontSendNotification);
        break;
    }
}

void ParamControl::resized()
{
    auto b = getLocalBounds();
    if (m_button.getComponentID() == "footswitch") {
        m_button.setBounds(b);
        return;
    }
    m_label.setBounds(b.removeFromTop(16));
    switch (m_kind) {
    case knob:
        m_slider.setBounds(b);
        break;
    case toggle:
        m_button.setBounds(b.withSizeKeepingCentre(juce::jmin(b.getWidth(), 32), juce::jmin(b.getHeight(), 32)));
        break;
    case choice:
        m_combo.setBounds(b.withSizeKeepingCentre(b.getWidth(), juce::jmin(b.getHeight(), 26)));
        break;
    }
}

//==============================================================================
// Pedalboard

namespace {
struct PedalDef {
    const char *name, *prefix, *category;
    juce::uint32 colour;
};

// left-to-right as on the JSFX board; matched by slider name
const PedalDef kPedals[] = {
    {"12 STEPPER", "12S ", "PITCH", 0xff9b59b6},
    {"GOLDEN CALF", "GC ", "DRIVE", 0xffe67e22},
    {"GREEN SIN", "GS ", "DRIVE", 0xffe67e22},
    {"HARDWARE INCOMPATIBLE", "HWI ", "DRIVE", 0xffe67e22},
    {"DOA666", "DOA ", "DRIVE", 0xffe67e22},
    {"MARMOT", "MA ", "DRIVE", 0xffe67e22},
    {"BLACK MASS", "BM ", "MOD", 0xffe84393},
    {"PIPEWORKS", "PW ", "MOD", 0xffe84393},
    {"THE ELEPHANT", "EL ", "DELAY", 0xff3498db},
    {"UNHOLY BOWL", "UB ", "REVERB", 0xff2ecc71},
    {"JUDGEMENT DAY", "JD ", "EQ", 0xfff1c40f},
};

constexpr int kCompRow = 88;
constexpr int kCompSwitchW = 76, kCompKnobW = 62;
constexpr int kCardBand = 40, kCardFoot = 58, kCardChoice = 40;
} // namespace

struct PedalboardPanel::Card final : juce::Component {
    juce::String name, category;
    juce::Colour colour;
    std::unique_ptr<ParamControl> sw;
    std::vector<std::unique_ptr<ParamControl>> params;
    bool lastOn = false;

    bool on() const { return sw && sw->value() > 0.5; }

    void paint(juce::Graphics &g) override
    {
        const bool lit = on();
        auto b = getLocalBounds().toFloat().reduced(1.0f);
        g.setGradientFill(juce::ColourGradient(kPanel.brighter(lit ? 0.12f : 0.04f), 0, b.getY(),
                                               kPanel.darker(0.3f), 0, b.getBottom(), false));
        g.fillRoundedRectangle(b, 7.0f);
        g.setColour(colour.withAlpha(lit ? 0.9f : 0.3f));
        g.drawRoundedRectangle(b, 7.0f, 1.5f);

        auto band = b.removeFromTop((float)kCardBand);
        juce::Path p;
        p.addRoundedRectangle(band.getX(), band.getY(), band.getWidth(), band.getHeight(), 7.0f, 7.0f, true, true, false, false);
        g.setColour(colour.withAlpha(lit ? 0.85f : 0.22f));
        g.fillPath(p);
        g.setColour(lit ? juce::Colours::black.withAlpha(0.6f) : colour.withAlpha(0.7f));
        g.setFont(juce::FontOptions(9.0f, juce::Font::bold));
        g.drawText(category, band.removeFromTop(13.0f).withTrimmedTop(2.0f), juce::Justification::centred);
        g.setColour(lit ? juce::Colours::black : kText.withAlpha(0.7f));
        g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
        g.drawFittedText(name, band.reduced(4.0f, 0.0f).toNearestInt(), juce::Justification::centred, 2, 0.75f);
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced(6);
        b.removeFromTop(kCardBand);
        sw->setBounds(b.removeFromBottom(kCardFoot).withSizeKeepingCentre(juce::jmin(b.getWidth(), 70), kCardFoot));

        std::vector<ParamControl *> knobs;
        for (auto &p : params) {
            if (p->isChoice())
                p->setBounds(b.removeFromTop(kCardChoice).reduced(2, 0));
            else
                knobs.push_back(p.get());
        }
        if (knobs.empty())
            return;
        const int cols = b.getWidth() >= 210 ? 3 : 2;
        const int rows = ((int)knobs.size() + cols - 1) / cols;
        const int cellW = b.getWidth() / cols;
        const int cellH = juce::jmin(84, b.getHeight() / rows);
        for (size_t i = 0; i < knobs.size(); ++i) {
            const int row = (int)i / cols, col = (int)i % cols;
            const int inRow = juce::jmin(cols, (int)knobs.size() - row * cols);
            const int x0 = b.getX() + (b.getWidth() - inRow * cellW) / 2;
            knobs[i]->setBounds(x0 + col * cellW, b.getY() + row * cellH, cellW, cellH);
        }
    }
};

PedalboardPanel::PedalboardPanel()
{
    m_orderLabel.setText("SIGNAL FLOW", juce::dontSendNotification);
    m_orderLabel.setFont(juce::FontOptions(12.0f, juce::Font::bold));
    m_orderLabel.setJustificationType(juce::Justification::centred);
    m_orderLabel.setColour(juce::Label::textColourId, kDim);
    addAndMakeVisible(m_orderLabel);

    m_hide.setClickingTogglesState(true);
    m_hide.setButtonText("HIDE INACTIVE");
    m_hide.onClick = [this] {
        setHideInactive(m_hide.getToggleState());
        if (onHideInactiveChanged)
            onHideInactiveChanged(m_hideInactive);
    };
    addAndMakeVisible(m_hide);
}

PedalboardPanel::~PedalboardPanel() = default;

void PedalboardPanel::build(YsfxProcessor &board)
{
    m_cards.clear();
    m_inKnobs.clear();
    m_outKnobs.clear();
    m_inComp.reset();
    m_outComp.reset();
    m_order.reset();

    const auto sliders = listSliders(board);
    auto find = [&](const juce::String &name) -> YsfxParameter * {
        for (auto &[i, n] : sliders)
            if (n == name)
                return board.getYsfxParameter(i);
        return nullptr;
    };
    auto add = [this](std::unique_ptr<ParamControl> &slot, YsfxParameter *p, const juce::String &label) {
        if (p) {
            slot = std::make_unique<ParamControl>(p, label, kAccent);
            addAndMakeVisible(*slot);
        }
    };
    auto addPrefixed = [&](std::vector<std::unique_ptr<ParamControl>> &out, const juce::String &prefix,
                           const juce::String &except, juce::Colour colour, juce::Component &parent) {
        for (auto &[i, n] : sliders) {
            if (!n.startsWith(prefix) || n == except)
                continue;
            out.push_back(std::make_unique<ParamControl>(board.getYsfxParameter(i), bareName(n, prefix), colour));
            parent.addAndMakeVisible(*out.back());
        }
    };

    add(m_inComp, find("In Comp"), "IN COMP");
    add(m_outComp, find("Out Comp"), "OUT COMP");
    add(m_order, find("Section Order"), {});
    addPrefixed(m_inKnobs, "In ", "In Comp", kAccent, *this);
    addPrefixed(m_outKnobs, "Out ", "Out Comp", kAccent, *this);

    for (auto &def : kPedals) {
        YsfxParameter *sw = find(def.name);
        if (!sw)
            continue;
        auto card = std::make_unique<Card>();
        card->name = juce::String(def.name);
        card->category = def.category;
        card->colour = juce::Colour(def.colour);
        card->sw = std::make_unique<ParamControl>(sw, juce::String(), card->colour);
        card->sw->makeFootswitch();
        card->addAndMakeVisible(*card->sw);
        addPrefixed(card->params, def.prefix, {}, card->colour, *card);
        card->lastOn = card->on();
        addChildComponent(*card);
        m_cards.push_back(std::move(card));
    }
    m_activeMask = ~0u;  // force a layout
    resized();
}

void PedalboardPanel::setHideInactive(bool hide)
{
    m_hideInactive = hide;
    m_hide.setToggleState(hide, juce::dontSendNotification);
    m_hide.setButtonText(hide ? "SHOW ALL PEDALS" : "HIDE INACTIVE");
    layoutCards();
    repaint();
}

void PedalboardPanel::sync()
{
    for (auto *c : {m_inComp.get(), m_outComp.get(), m_order.get()})
        if (c)
            c->sync();
    for (auto &k : m_inKnobs)
        k->sync();
    for (auto &k : m_outKnobs)
        k->sync();

    juce::uint32 mask = 0;
    for (size_t i = 0; i < m_cards.size(); ++i) {
        auto &card = *m_cards[i];
        card.sw->sync();
        for (auto &p : card.params)
            p->sync();
        const bool on = card.on();
        if (on != card.lastOn) {
            card.lastOn = on;
            card.repaint();
        }
        if (on)
            mask |= 1u << i;
    }
    if (m_hideInactive && mask != m_activeMask)
        layoutCards();
}

void PedalboardPanel::paint(juce::Graphics &g)
{
    auto strip = [&](ParamControl *sw) {
        if (!sw)
            return;
        auto r = sw->getBounds().getUnion(sw == m_inComp.get() ? m_inKnobs.back()->getBounds() : m_outKnobs.back()->getBounds());
        g.setColour(juce::Colours::black.withAlpha(0.3f));
        g.fillRoundedRectangle(r.toFloat().expanded(4.0f, 2.0f), 6.0f);
        g.setColour(kAccent.withAlpha(sw->value() > 0.5 ? 0.6f : 0.2f));
        g.drawRoundedRectangle(r.toFloat().expanded(4.0f, 2.0f), 6.0f, 1.0f);
    };
    if (!m_inKnobs.empty())
        strip(m_inComp.get());
    if (!m_outKnobs.empty())
        strip(m_outComp.get());

    if (m_hideInactive && m_activeMask == 0) {
        g.setColour(kDim);
        g.setFont(juce::FontOptions(16.0f));
        g.drawText("No pedals engaged - SHOW ALL PEDALS to pick some", getLocalBounds().withTrimmedTop(kCompRow),
                   juce::Justification::centred);
    }
}

void PedalboardPanel::resized()
{
    auto b = getLocalBounds().reduced(10, 6);
    auto row = b.removeFromTop(kCompRow).reduced(4, 2);

    auto strip = [](ParamControl *sw, std::vector<std::unique_ptr<ParamControl>> &knobs, juce::Rectangle<int> r) {
        if (sw)
            sw->setBounds(r.removeFromLeft(kCompSwitchW));
        for (auto &k : knobs)
            k->setBounds(r.removeFromLeft(kCompKnobW));
    };
    const int stripW = kCompSwitchW + kCompKnobW * 5;
    strip(m_inComp.get(), m_inKnobs, row.removeFromLeft(stripW));
    m_hide.setBounds(row.removeFromRight(170).withSizeKeepingCentre(170, 34));
    row.removeFromRight(16);
    strip(m_outComp.get(), m_outKnobs, row.removeFromRight(stripW));
    auto mid = row.reduced(20, 0).withSizeKeepingCentre(juce::jmin(row.getWidth() - 40, 240), 56);
    m_orderLabel.setBounds(mid.removeFromTop(20));
    if (m_order)
        m_order->setBounds(mid.withTrimmedTop(-16));  // ParamControl reserves 16px for its (empty) label

    b.removeFromTop(6);
    m_cardArea = b;
    layoutCards();
}

void PedalboardPanel::layoutCards()
{
    std::vector<Card *> shown;
    juce::uint32 mask = 0;
    for (size_t i = 0; i < m_cards.size(); ++i) {
        const bool on = m_cards[i]->on();
        if (on)
            mask |= 1u << i;
        const bool vis = !m_hideInactive || on;
        m_cards[i]->setVisible(vis);
        if (vis)
            shown.push_back(m_cards[i].get());
    }
    m_activeMask = mask;
    repaint();
    if (shown.empty())
        return;

    const int gap = 8, n = (int)shown.size();
    const int w = juce::jmin(220, (m_cardArea.getWidth() - gap * (n - 1)) / n);
    int x = m_cardArea.getX() + (m_cardArea.getWidth() - (n * w + gap * (n - 1))) / 2;
    for (auto *c : shown) {
        c->setBounds(x, m_cardArea.getY(), w, m_cardArea.getHeight());
        x += w + gap;
    }
}

//==============================================================================
// Amp

namespace {
const char *const kAmpSections[] = {"PREAMP", "TONE", "POWER", "EFFECTS", "ODB-3", "CRUSHINATOR", "COMPRESSOR"};
const char *const kAmpPrefixes[] = {"", "", "", "", "ODB ", "Crush ", "Comp "};

int ampSection(const juce::String &n)
{
    if (n.startsWith("ODB"))
        return 4;
    if (n.startsWith("Crush"))
        return 5;
    if (n == "Compressor" || n.startsWith("Comp ") || n.startsWith("Peak Reduction") || n.startsWith("Emphasis"))
        return 6;
    if (n == "FX Bypass" || n.contains("Echo") || n.contains("Reverb") || n.startsWith("Tremolo"))
        return 3;
    if (n.startsWith("PSU") || n.startsWith("Master") || n.startsWith("Output Level") || n == "Impedance Tap" || n == "Noise Gate")
        return 2;
    for (auto *t : {"Bass", "Mid", "Treble", "Presence", "Tone", "Resonance", "Low ", "High ", "Ultra", "Jump Mix"})
        if (n.startsWith(t))
            return 1;
    return 0;
}

constexpr int kAmpHeader = 84;
constexpr int kSectionH = 150, kSectionTitle = 26, kSectionPad = 12, kSectionGap = 12;
} // namespace

struct AmpPanel::Section {
    juce::String title;
    std::vector<std::unique_ptr<ParamControl>> controls;
    juce::Rectangle<int> bounds;

    int width() const
    {
        int w = kSectionPad * 2;
        for (auto &c : controls)
            w += c->isChoice() ? 124 : c->isSwitch() ? 78 : 88;
        return w;
    }
};

AmpPanel::AmpPanel() = default;
AmpPanel::~AmpPanel() = default;

void AmpPanel::build(YsfxProcessor &amp, const juce::StringArray &skipNames)
{
    m_sections.clear();
    m_title.clear();
    m_subtitle.clear();

    // "LMS - The TRSOB v2 (Density-Aware High-Gain Rectifier)"
    if (auto info = amp.getCurrentInfo(); info && info->effect) {
        juce::String desc = juce::CharPointer_UTF8(ysfx_get_name(info->effect.get()));
        juce::String head = desc.fromFirstOccurrenceOf("LMS - ", false, false).upToFirstOccurrenceOf("(", false, false).trim();
        if (head.endsWith(" v2"))
            head = head.dropLastCharacters(3);
        m_title = (head.isNotEmpty() ? head : desc).toUpperCase();
        if (desc.contains("("))
            m_subtitle = desc.fromFirstOccurrenceOf("(", false, false).upToLastOccurrenceOf(")", false, false);
    }

    for (size_t s = 0; s < std::size(kAmpSections); ++s) {
        auto sec = std::make_unique<Section>();
        sec->title = kAmpSections[s];
        m_sections.push_back(std::move(sec));
    }
    for (auto &[i, n] : listSliders(amp)) {
        if (skipNames.contains(n))
            continue;
        const int s = ampSection(n);
        juce::String label = bareName(n, kAmpPrefixes[s]);
        if (s >= 4 && n == bareName(n) && !n.startsWith(kAmpPrefixes[s]))
            label = "ON";  // the built-in pedal's own on/off switch
        auto c = std::make_unique<ParamControl>(amp.getYsfxParameter(i), label, kAccent);
        addAndMakeVisible(*c);
        m_sections[(size_t)s]->controls.push_back(std::move(c));
    }
    m_sections.erase(std::remove_if(m_sections.begin(), m_sections.end(), [](auto &s) { return s->controls.empty(); }),
                     m_sections.end());
    resized();
    repaint();
}

void AmpPanel::sync()
{
    for (auto &s : m_sections)
        for (auto &c : s->controls)
            c->sync();
}

void AmpPanel::paint(juce::Graphics &g)
{
    auto b = getLocalBounds().toFloat().reduced(10.0f);
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff261c1c), 0, b.getY(), juce::Colour(0xff110c0c), 0, b.getBottom(), false));
    g.fillRoundedRectangle(b, 12.0f);
    g.setColour(kAccent.withAlpha(0.55f));
    g.drawRoundedRectangle(b, 12.0f, 2.0f);

    auto head = b.removeFromTop((float)kAmpHeader).reduced(28.0f, 14.0f);
    g.setColour(kAccent.brighter(0.25f));
    g.setFont(juce::FontOptions(34.0f, juce::Font::bold));
    g.drawText(m_title, head.removeFromTop(38.0f), juce::Justification::centredLeft);
    g.setColour(kDim);
    g.setFont(juce::FontOptions(15.0f, juce::Font::italic));
    g.drawText(m_subtitle, head, juce::Justification::centredLeft);
    g.setColour(kAccent.withAlpha(0.35f));
    g.fillRect(b.getX() + 20.0f, b.getY(), b.getWidth() - 40.0f, 1.0f);

    for (auto &s : m_sections) {
        auto r = s->bounds.toFloat();
        g.setColour(juce::Colours::black.withAlpha(0.28f));
        g.fillRoundedRectangle(r, 8.0f);
        g.setColour(kAccent.withAlpha(0.25f));
        g.drawRoundedRectangle(r, 8.0f, 1.0f);
        g.setColour(kAccent);
        g.fillRect(r.getX() + 12.0f, r.getY() + 9.0f, 3.0f, 12.0f);
        g.setColour(kText.withAlpha(0.75f));
        g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
        g.drawText(s->title, r.withHeight((float)kSectionTitle + 4).withTrimmedLeft(22.0f), juce::Justification::centredLeft);
    }
}

void AmpPanel::resized()
{
    auto area = getLocalBounds().reduced(30, 20).withTrimmedTop(kAmpHeader);

    // greedy rows, each centred
    std::vector<std::vector<Section *>> rows(1);
    int rowW = 0;
    for (auto &s : m_sections) {
        const int w = s->width();
        if (!rows.back().empty() && rowW + kSectionGap + w > area.getWidth()) {
            rows.emplace_back();
            rowW = 0;
        }
        rowW += (rows.back().empty() ? 0 : kSectionGap) + w;
        rows.back().push_back(s.get());
    }

    const int totalH = (int)rows.size() * kSectionH + ((int)rows.size() - 1) * kSectionGap;
    int y = area.getY() + juce::jmax(0, (area.getHeight() - totalH) / 2);
    for (auto &row : rows) {
        int w = -kSectionGap;
        for (auto *s : row)
            w += s->width() + kSectionGap;
        int x = area.getX() + (area.getWidth() - w) / 2;
        for (auto *s : row) {
            s->bounds = {x, y, s->width(), kSectionH};
            auto inner = s->bounds.reduced(kSectionPad, 8).withTrimmedTop(kSectionTitle);
            for (auto &c : s->controls)
                c->setBounds(inner.removeFromLeft(c->isChoice() ? 124 : c->isSwitch() ? 78 : 88));
            x += s->width() + kSectionGap;
        }
        y += kSectionH + kSectionGap;
    }
}

} // namespace rigui
