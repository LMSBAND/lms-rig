// LMS Rig — native controls for the pedalboard and amp heads.
// Every control is bound to a JSFX slider by name and polls it, so edits made
// in the classic JSFX view (or by the host) show up here too.

#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
#include <vector>

class YsfxParameter;
class YsfxProcessor;

namespace rigui {

const juce::Colour kBg{0xff0e0b0b};
const juce::Colour kPanel{0xff1a1414};
const juce::Colour kAccent{0xffc0392b};
const juce::Colour kText{0xffe8dcdc};
const juce::Colour kDim{0xff8a7c7c};

class RigLookAndFeel final : public juce::LookAndFeel_V4 {
public:
    RigLookAndFeel();
    void drawRotarySlider(juce::Graphics &, int x, int y, int w, int h, float pos, float start, float end,
                          juce::Slider &) override;
    void drawToggleButton(juce::Graphics &, juce::ToggleButton &, bool highlighted, bool down) override;
};

// One JSFX slider shown as a knob, an on/off switch, or a selector
class ParamControl final : public juce::Component {
public:
    ParamControl(YsfxParameter *p, const juce::String &label, juce::Colour accent);
    void makeFootswitch();
    void sync();
    double value() const;
    bool isSwitch() const { return m_kind == toggle; }
    bool isChoice() const { return m_kind == choice; }
    void resized() override;

private:
    enum Kind { knob, toggle, choice };
    void set(double v);

    YsfxParameter *m_param;
    Kind m_kind = knob;
    juce::Label m_label;
    juce::Slider m_slider;
    juce::ToggleButton m_button;
    juce::ComboBox m_combo;
};

// Satan's Little Pedal Board below its header: comp strips, flow order, stompboxes
class PedalboardPanel final : public juce::Component {
public:
    PedalboardPanel();
    ~PedalboardPanel() override;
    void build(YsfxProcessor &board);
    void setHideInactive(bool hide);
    std::function<void(bool)> onHideInactiveChanged;
    void sync();
    void paint(juce::Graphics &) override;
    void resized() override;

    struct Card;
private:
    void layoutCards();

    std::vector<std::unique_ptr<Card>> m_cards;
    std::unique_ptr<ParamControl> m_inComp, m_outComp, m_order;
    std::vector<std::unique_ptr<ParamControl>> m_inKnobs, m_outKnobs;
    juce::Label m_orderLabel;
    juce::TextButton m_hide;
    juce::Rectangle<int> m_cardArea;
    bool m_hideInactive = false;
    juce::uint32 m_activeMask = 0;
};

// The loaded head as a faceplate of sections; cab sliders are left to the rig's sidebar
class AmpPanel final : public juce::Component {
public:
    AmpPanel();
    ~AmpPanel() override;
    void build(YsfxProcessor &amp, const juce::StringArray &skipNames);
    void sync();
    void paint(juce::Graphics &) override;
    void resized() override;

    struct Section;
private:
    std::vector<std::unique_ptr<Section>> m_sections;
    juce::String m_title, m_subtitle;
};

} // namespace rigui
