#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <span>

#include "motifxs/catalog.hpp"
#include "motifxs/parameters.hpp"
#include "Theme.h"

/// A Normal Voice is Common plus up to 8 Elements, each a complete layer with
/// its own wave, pitch, filter, amplitude and LFO. The elements are what makes
/// a MOTIF voice a MOTIF voice -- a drawbar organ patch is literally eight
/// elements, one per footage, and their levels are the registration -- so they
/// are shown side by side rather than one at a time.
///
/// Each element spans two address blocks: 41 ee (oscillator, amplitude, pitch)
/// and 42 ee (filter, EQ, LFO). That split is a protocol detail and is hidden
/// here; the parameter model resolves it.
class ElementEditor : public juce::Component {
public:
    static constexpr int kElements = 8;

    /// (element, parameter, raw value) when a control moves.
    std::function<void(int, const motifxs::Parameter&, int)> onEdit;
    /// (element) when a strip is selected, so the host can read its detail.
    std::function<void(int)> onElementSelected;

    struct Detail {
        const char* label;
        const char* id;
        bool bipolar;
    };

    ElementEditor() {
        header_.setFont(theme::panelFont(15.5f));
        header_.setColour(juce::Label::textColourId, theme::text);
        addAndMakeVisible(header_);

        hint_.setFont(juce::FontOptions(11.0f));
        hint_.setColour(juce::Label::textColourId, theme::dim);
        hint_.setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(hint_);

        assignParam_ = motifxs::findParameterById("normal_voice_element_element_assign");
        levelParam_ = motifxs::findParameterById("normal_voice_element_element_level");
        panParam_ = motifxs::findParameterById("normal_voice_element_pan");

        for (int e = 0; e < kElements; ++e) {
            auto& s = strips_[size_t(e)];

            s.select.setButtonText("EL" + juce::String(e + 1));
            s.select.onClick = [this, e] { select(e); };
            addAndMakeVisible(s.select);

            s.wave.setFont(juce::FontOptions(10.0f));
            s.wave.setColour(juce::Label::textColourId, theme::dim);
            s.wave.setJustificationType(juce::Justification::centred);
            addAndMakeVisible(s.wave);

            s.on.setButtonText("ON");
            s.on.setClickingTogglesState(true);
            s.on.setColour(juce::TextButton::buttonOnColourId, theme::good);
            s.on.setColour(juce::TextButton::textColourOnId, juce::Colours::black);
            s.on.setColour(juce::TextButton::textColourOffId, theme::dim);
            s.on.onClick = [this, e] {
                if (suppress_ || !assignParam_ || !onEdit) return;
                onEdit(e, *assignParam_, strips_[size_t(e)].on.getToggleState() ? 1 : 0);
                refreshEnabled(e);
            };
            addAndMakeVisible(s.on);

            // A vertical fader, because for an organ patch this column *is*
            // the drawbar and reads that way at a glance.
            s.level.setSliderStyle(juce::Slider::LinearVertical);
            s.level.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 46, 15);
            s.level.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            s.level.setColour(juce::Slider::textBoxTextColourId, theme::text);
            s.level.setColour(juce::Slider::thumbColourId, theme::accent);
            s.level.setColour(juce::Slider::trackColourId, theme::line);
            s.level.setRange(0.0, 127.0, 1.0);
            s.level.onValueChange = [this, e] {
                if (suppress_ || !levelParam_ || !onEdit) return;
                onEdit(e, *levelParam_, int(strips_[size_t(e)].level.getValue()));
            };
            addAndMakeVisible(s.level);

            s.pan.setSliderStyle(juce::Slider::RotaryVerticalDrag);
            s.pan.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 46, 14);
            s.pan.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            s.pan.setColour(juce::Slider::textBoxTextColourId, theme::dim);
            s.pan.setRange(1.0, 127.0, 1.0);
            s.pan.textFromValueFunction = [](double v) {
                const int c = int(v) - 64;
                return c == 0 ? juce::String("C")
                              : (c < 0 ? "L" : "R") + juce::String(std::abs(c));
            };
            s.pan.onValueChange = [this, e] {
                if (suppress_ || !panParam_ || !onEdit) return;
                onEdit(e, *panParam_, int(strips_[size_t(e)].pan.getValue()));
            };
            addAndMakeVisible(s.pan);
        }

        for (size_t i = 0; i < kDetails.size(); ++i) {
            detailParams_[i] = motifxs::findParameterById(kDetails[i].id);
            auto& l = detailLabels_[i];
            auto& k = detailKnobs_[i];
            l.setText(kDetails[i].label, juce::dontSendNotification);
            l.setJustificationType(juce::Justification::centred);
            l.setFont(theme::panelFont(11.0f));
            l.setColour(juce::Label::textColourId, theme::dim);
            addAndMakeVisible(l);

            k.setSliderStyle(juce::Slider::RotaryVerticalDrag);
            k.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 52, 15);
            k.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            k.setColour(juce::Slider::textBoxTextColourId, theme::text);
            if (kDetails[i].bipolar) k.setRange(-64.0, 63.0, 1.0);
            else if (detailParams_[i]) k.setRange(detailParams_[i]->min, detailParams_[i]->max, 1.0);
            k.onValueChange = [this, i] {
                if (suppress_ || !detailParams_[i] || !onEdit) return;
                const int raw = kDetails[i].bipolar ? int(detailKnobs_[i].getValue()) + 64
                                                    : int(detailKnobs_[i].getValue());
                onEdit(selected_, *detailParams_[i], raw);
            };
            addAndMakeVisible(k);
        }

        filterTypeParam_ = motifxs::findParameterById("normal_voice_element_filter_type");
        xaParam_ = motifxs::findParameterById("normal_voice_element_xa_control");

        filterLabel_.setText("FILTER", juce::dontSendNotification);
        xaLabel_.setText("XA CONTROL", juce::dontSendNotification);
        for (auto* l : {&filterLabel_, &xaLabel_}) {
            l->setFont(theme::panelFont(11.0f));
            l->setColour(juce::Label::textColourId, theme::dim);
            l->setJustificationType(juce::Justification::centredRight);
            addAndMakeVisible(*l);
        }

        int id = 1;
        for (const auto* n : kFilterTypes) filterType_.addItem(n, id++);
        filterType_.onChange = [this] {
            if (suppress_ || !filterTypeParam_ || !onEdit) return;
            onEdit(selected_, *filterTypeParam_, filterType_.getSelectedId() - 1);
        };
        addAndMakeVisible(filterType_);

        id = 1;
        for (const auto* n : kXaModes) xa_.addItem(n, id++);
        xa_.onChange = [this] {
            if (suppress_ || !xaParam_ || !onEdit) return;
            onEdit(selected_, *xaParam_, xa_.getSelectedId() - 1);
        };
        addAndMakeVisible(xa_);

        select(0);
    }

    void setAvailable(bool yes, const juce::String& why) {
        available_ = yes;
        hint_.setText(why, juce::dontSendNotification);
        for (auto& s : strips_) {
            s.select.setEnabled(yes);
            s.on.setEnabled(yes);
        }
        for (auto& k : detailKnobs_) k.setEnabled(yes);
        filterType_.setEnabled(yes);
        xa_.setEnabled(yes);
        for (int e = 0; e < kElements; ++e) refreshEnabled(e);
        repaint();
    }

    void setAssigned(int e, bool on) {
        if (e < 0 || e >= kElements) return;
        const juce::ScopedValueSetter<bool> guard(suppress_, true);
        strips_[size_t(e)].on.setToggleState(on, juce::dontSendNotification);
        refreshEnabled(e);
    }

    void setWaveform(int e, int number) {
        if (e < 0 || e >= kElements) return;
        const auto* w = motifxs::findWaveform(number);
        strips_[size_t(e)].wave.setText(w ? juce::String(std::string(w->name))
                                          : juce::String(number),
                                        juce::dontSendNotification);
        strips_[size_t(e)].wave.setTooltip(w ? juce::String(std::string(w->name)) + "  #" +
                                                   juce::String(number)
                                             : juce::String(number));
    }

    /// Applies a value read from the rack without echoing it back as an edit.
    void setValue(int e, const motifxs::Parameter& p, int raw) {
        const juce::ScopedValueSetter<bool> guard(suppress_, true);
        if (&p == levelParam_ && e >= 0 && e < kElements) {
            strips_[size_t(e)].level.setValue(raw, juce::dontSendNotification);
            return;
        }
        if (&p == panParam_ && e >= 0 && e < kElements) {
            strips_[size_t(e)].pan.setValue(raw, juce::dontSendNotification);
            return;
        }
        if (e != selected_) return;            // detail shows the selected element only
        if (&p == filterTypeParam_) {
            filterType_.setSelectedId(raw + 1, juce::dontSendNotification);
            return;
        }
        if (&p == xaParam_) {
            xa_.setSelectedId(raw + 1, juce::dontSendNotification);
            return;
        }
        for (size_t i = 0; i < kDetails.size(); ++i)
            if (detailParams_[i] == &p) {
                detailKnobs_[i].setValue(kDetails[i].bipolar ? raw - 64 : raw,
                                         juce::dontSendNotification);
                return;
            }
    }

    [[nodiscard]] int selected() const { return selected_; }
    static std::span<const Detail> details() { return kDetails; }
    const motifxs::Parameter* detailParameter(size_t i) const { return detailParams_[i]; }
    const motifxs::Parameter* assignParameter() const { return assignParam_; }
    const motifxs::Parameter* levelParameter() const { return levelParam_; }
    const motifxs::Parameter* panParameter() const { return panParam_; }
    const motifxs::Parameter* filterTypeParameter() const { return filterTypeParam_; }
    const motifxs::Parameter* xaParameter() const { return xaParam_; }

    void resized() override {
        auto r = getLocalBounds().reduced(2);
        auto top = r.removeFromTop(24);
        header_.setBounds(top.removeFromLeft(220));
        hint_.setBounds(top);
        r.removeFromTop(4);

        auto strips = r.removeFromTop(juce::jmax(210, r.getHeight() - 130));
        const int w = strips.getWidth() / kElements;
        for (int e = 0; e < kElements; ++e) {
            auto cell = strips.removeFromLeft(w).reduced(3);
            strips_[size_t(e)].select.setBounds(cell.removeFromTop(20));
            strips_[size_t(e)].wave.setBounds(cell.removeFromTop(14));
            strips_[size_t(e)].on.setBounds(cell.removeFromTop(18).reduced(cell.getWidth() / 4, 1));
            strips_[size_t(e)].pan.setBounds(cell.removeFromBottom(52));
            strips_[size_t(e)].level.setBounds(cell.reduced(cell.getWidth() / 3, 2));
        }

        r.removeFromTop(6);
        auto combos = r.removeFromTop(24);
        combos.removeFromLeft(8);
        filterLabel_.setBounds(combos.removeFromLeft(56));
        combos.removeFromLeft(6);
        filterType_.setBounds(combos.removeFromLeft(120));
        combos.removeFromLeft(24);
        xaLabel_.setBounds(combos.removeFromLeft(84));
        combos.removeFromLeft(6);
        xa_.setBounds(combos.removeFromLeft(150));

        r.removeFromTop(6);
        auto row = r.removeFromTop(88);
        const int dw = row.getWidth() / int(kDetails.size());
        for (size_t i = 0; i < kDetails.size(); ++i) {
            auto cell = row.removeFromLeft(dw).reduced(3);
            detailLabels_[i].setBounds(cell.removeFromTop(13));
            detailKnobs_[i].setBounds(cell);
        }
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(theme::bg);
        if (!available_) {
            g.setColour(theme::dim);
            g.setFont(juce::FontOptions(13.0f));
            g.drawText("No Normal Voice in the edit buffer", getLocalBounds(),
                       juce::Justification::centred);
        }
    }

private:
    struct Strip {
        juce::TextButton select, on;
        juce::Label wave;
        juce::Slider level, pan;
    };

    void select(int e) {
        selected_ = juce::jlimit(0, kElements - 1, e);
        for (int i = 0; i < kElements; ++i) {
            auto& b = strips_[size_t(i)].select;
            b.setColour(juce::TextButton::buttonColourId,
                        i == selected_ ? theme::accent : theme::panelHi);
            b.setColour(juce::TextButton::textColourOffId,
                        i == selected_ ? juce::Colours::black : theme::text);
        }
        header_.setText("ELEMENT " + juce::String(selected_ + 1), juce::dontSendNotification);
        if (onElementSelected) onElementSelected(selected_);
        repaint();
    }

    /// An unassigned element's controls do nothing on the rack, so they should
    /// not look live.
    void refreshEnabled(int e) {
        const bool on = available_ && strips_[size_t(e)].on.getToggleState();
        strips_[size_t(e)].level.setEnabled(on);
        strips_[size_t(e)].pan.setEnabled(on);
    }

    static constexpr const char* kFilterTypes[] = {
        "LPF24D", "LPF24A", "LPF18", "LPF18s", "LPF12", "LPF6", "HPF24D", "HPF12",
        "BPF12D", "BPFw", "BPF6", "BEF12", "BEF6", "Dual LPF", "Dual HPF", "Dual BPF",
        "Dual BEF", "LPF12+BPF6", "THRU"};

    // Expanded Articulation: the MOTIF XS's signature per-element behaviour.
    // Nothing in a generic subtractive UI corresponds to it, so it is given a
    // control of its own rather than buried.
    static constexpr const char* kXaModes[] = {
        "normal", "legato", "key off sound", "wave cycle", "wave random",
        "all AF off", "AF 1 on", "AF 2 on"};

    static constexpr std::array<Detail, 6> kDetails{{
        {"CUTOFF",  "normal_voice_element_filter_cutoff_frequency",     false},
        {"RESO",    "normal_voice_element_filter_resonance_width_band", false},
        {"ATTACK",  "normal_voice_element_aeg_attack_time",             false},
        {"DECAY",   "normal_voice_element_aeg_decay_1_time",            false},
        {"RELEASE", "normal_voice_element_aeg_release_time",            false},
        {"TUNE",    "normal_voice_element_coarse_tune",                 true},
    }};

    std::array<Strip, kElements> strips_;
    juce::Label header_, hint_, filterLabel_, xaLabel_;
    juce::ComboBox filterType_, xa_;
    std::array<juce::Slider, 6> detailKnobs_;
    std::array<juce::Label, 6> detailLabels_;
    std::array<const motifxs::Parameter*, 6> detailParams_{};
    const motifxs::Parameter* assignParam_{};
    const motifxs::Parameter* levelParam_{};
    const motifxs::Parameter* panParam_{};
    const motifxs::Parameter* filterTypeParam_{};
    const motifxs::Parameter* xaParam_{};
    int selected_{0};
    bool suppress_{false};
    bool available_{false};
};
