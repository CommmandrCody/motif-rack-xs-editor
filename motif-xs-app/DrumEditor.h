#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <span>

#include "motifxs/catalog.hpp"
#include "motifxs/parameters.hpp"
#include "Theme.h"

/// A MOTIF-RACK XS drum kit spreads 73 instruments across C0-C6, one per key,
/// each with its own waveform, tuning, filter and routing. That is a different
/// instrument from a Normal Voice's 8 elements, so it gets its own editor
/// rather than the element UI with most controls greyed out.
class DrumKeyMap : public juce::Component {
public:
    static constexpr int kKeys = 73;      // ee = 0..72
    static constexpr int kFirstNote = 24; // ee 0 == C0 == MIDI note 24

    std::function<void(int)> onKeySelected;   // takes ee

    DrumKeyMap() { assigned_.fill(false); }

    void setAssigned(int ee, bool on) {
        if (ee >= 0 && ee < kKeys && assigned_[size_t(ee)] != on) {
            assigned_[size_t(ee)] = on;
            repaint();
        }
    }
    void setSelected(int ee) { selected_ = ee; repaint(); }
    [[nodiscard]] int selected() const { return selected_; }

    static bool isBlack(int ee) {
        static constexpr bool black[12] = {false, true, false, true, false, false,
                                           true, false, true, false, true, false};
        return black[(ee + kFirstNote) % 12];
    }
    static juce::String noteName(int ee) {
        static const char* n[12] = {"C", "C#", "D", "D#", "E", "F",
                                    "F#", "G", "G#", "A", "A#", "B"};
        const int midi = ee + kFirstNote;
        return juce::String(n[midi % 12]) + juce::String(midi / 12 - 2);
    }

    void paint(juce::Graphics& g) override {
        auto r = getLocalBounds().toFloat();
        g.setColour(theme::panel);
        g.fillRoundedRectangle(r, 4.0f);

        const int whites = countWhites();
        if (whites == 0) return;
        const float w = r.getWidth() / float(whites);

        // white keys first, then black on top, so the black keys overlap
        int wi = 0;
        for (int ee = 0; ee < kKeys; ++ee) {
            if (isBlack(ee)) continue;
            drawKey(g, ee, r.getX() + wi * w, r.getY(), w, r.getHeight(), false);
            ++wi;
        }
        wi = 0;
        for (int ee = 0; ee < kKeys; ++ee) {
            if (isBlack(ee)) { 
                drawKey(g, ee, r.getX() + wi * w - w * 0.3f, r.getY(),
                        w * 0.6f, r.getHeight() * 0.62f, true);
            } else {
                ++wi;
            }
        }
    }

    void mouseDown(const juce::MouseEvent& e) override {
        const int ee = keyAt(e.position);
        if (ee < 0) return;
        selected_ = ee;
        repaint();
        if (onKeySelected) onKeySelected(ee);
    }

private:
    static int countWhites() {
        int n = 0;
        for (int ee = 0; ee < kKeys; ++ee)
            if (!isBlack(ee)) ++n;
        return n;
    }

    void drawKey(juce::Graphics& g, int ee, float x, float y, float w, float h, bool black) {
        const bool sel = ee == selected_;
        const bool on = assigned_[size_t(ee)];
        juce::Colour c;
        if (sel) c = theme::accent;
        else if (black) c = on ? theme::panelHi.brighter(0.10f) : juce::Colour(0xff121316);
        else c = on ? theme::line.brighter(0.28f) : theme::panelHi.darker(0.25f);

        const auto rect = juce::Rectangle<float>(x, y, w - 1.0f, h);
        g.setColour(c);
        g.fillRoundedRectangle(rect, 2.0f);
        g.setColour(juce::Colour(0xff0d0e10));
        g.drawRoundedRectangle(rect, 2.0f, 0.8f);

        // label C of each octave, so the map can be read at a glance
        if (!black && (ee + kFirstNote) % 12 == 0 && w > 9.0f) {
            g.setColour(sel ? juce::Colours::black : theme::dim);
            g.setFont(juce::FontOptions(9.0f));
            g.drawText(noteName(ee), rect.reduced(1.0f).withTrimmedTop(rect.getHeight() - 14.0f),
                       juce::Justification::centred);
        }
    }

    int keyAt(juce::Point<float> p) {
        auto r = getLocalBounds().toFloat();
        const int whites = countWhites();
        if (whites == 0) return -1;
        const float w = r.getWidth() / float(whites);

        // black keys sit on top, so hit-test them first
        int wi = 0;
        for (int ee = 0; ee < kKeys; ++ee) {
            if (isBlack(ee)) {
                const juce::Rectangle<float> b(r.getX() + wi * w - w * 0.3f, r.getY(),
                                               w * 0.6f, r.getHeight() * 0.62f);
                if (b.contains(p)) return ee;
            } else {
                ++wi;
            }
        }
        wi = 0;
        for (int ee = 0; ee < kKeys; ++ee) {
            if (isBlack(ee)) continue;
            const juce::Rectangle<float> b(r.getX() + wi * w, r.getY(), w, r.getHeight());
            if (b.contains(p)) return ee;
            ++wi;
        }
        return -1;
    }

    std::array<bool, kKeys> assigned_{};
    int selected_{12};   // C1, where the kick usually lives
};

/// The drum page: a 73-key map plus the selected key's parameters.
///
/// Owns no MIDI itself -- it reports what the user did and is told what the
/// rack says, so the component stays testable and the device work stays on the
/// worker thread.
class DrumEditor : public juce::Component {
public:
    /// (ee, parameter, raw value) when the user changes a control.
    std::function<void(int, const motifxs::Parameter&, int)> onEdit;
    /// (ee) when a key is picked, so the host can audition and read its state.
    std::function<void(int)> onKeySelected;

    struct Control {
        const char* label;
        const char* id;
        bool bipolar;
    };

    DrumEditor() {
        addAndMakeVisible(keys_);
        keys_.onKeySelected = [this](int ee) {
            updateHeader();
            if (onKeySelected) onKeySelected(ee);
        };

        header_.setFont(theme::panelFont(15.5f));
        header_.setColour(juce::Label::textColourId, theme::text);
        addAndMakeVisible(header_);

        waveLabel_.setFont(juce::FontOptions(12.0f));
        waveLabel_.setColour(juce::Label::textColourId, theme::dim);
        addAndMakeVisible(waveLabel_);

        hint_.setFont(juce::FontOptions(11.0f));
        hint_.setColour(juce::Label::textColourId, theme::dim);
        hint_.setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(hint_);

        for (size_t i = 0; i < kControls.size(); ++i) {
            const auto& spec = kControls[i];
            auto& s = sliders_[i];
            auto& l = labels_[i];
            params_[i] = motifxs::findParameterById(spec.id);

            l.setText(spec.label, juce::dontSendNotification);
            l.setJustificationType(juce::Justification::centred);
            l.setFont(theme::panelFont(11.0f));
            l.setColour(juce::Label::textColourId, theme::dim);
            addAndMakeVisible(l);

            s.setSliderStyle(juce::Slider::RotaryVerticalDrag);
            s.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 52, 15);
            s.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            s.setColour(juce::Slider::textBoxTextColourId, theme::text);
            if (spec.bipolar) s.setRange(-64.0, 63.0, 1.0);
            else if (params_[i]) s.setRange(params_[i]->min, params_[i]->max, 1.0);
            s.onValueChange = [this, i] {
                if (suppress_ || !params_[i] || !onEdit) return;
                const int raw = kControls[i].bipolar ? int(sliders_[i].getValue()) + 64
                                                     : int(sliders_[i].getValue());
                onEdit(keys_.selected(), *params_[i], raw);
            };
            addAndMakeVisible(s);
        }
        // The two that make a kit behave like a kit: Alternate Group is how a
        // closed hi-hat cuts off an open one, and Receive Note Off decides
        // whether a cymbal rings or chokes on key release.
        altGroupLabel_.setText("ALT GROUP", juce::dontSendNotification);
        rcvOffLabel_.setText("KEY OFF", juce::dontSendNotification);
        for (auto* l : {&altGroupLabel_, &rcvOffLabel_}) {
            l->setFont(theme::panelFont(11.0f));
            l->setColour(juce::Label::textColourId, theme::dim);
            l->setJustificationType(juce::Justification::centredRight);
            addAndMakeVisible(*l);
        }

        altGroup_.addItem("off", 1);
        for (int i = 1; i <= 127; ++i) altGroup_.addItem(juce::String(i), i + 1);
        altGroupParam_ = motifxs::findParameterById("drum_voice_key_alternate_group");
        altGroup_.onChange = [this] {
            if (suppress_ || !altGroupParam_ || !onEdit) return;
            onEdit(keys_.selected(), *altGroupParam_, altGroup_.getSelectedId() - 1);
        };
        addAndMakeVisible(altGroup_);

        rcvOff_.addItem("ignore (ring)", 1);
        rcvOff_.addItem("receive (choke)", 2);
        rcvOffParam_ = motifxs::findParameterById("drum_voice_key_receive_note_off");
        rcvOff_.onChange = [this] {
            if (suppress_ || !rcvOffParam_ || !onEdit) return;
            onEdit(keys_.selected(), *rcvOffParam_, rcvOff_.getSelectedId() - 1);
        };
        addAndMakeVisible(rcvOff_);

        updateHeader();
    }

    DrumKeyMap& keyMap() { return keys_; }
    const motifxs::Parameter* altGroupParameter() const { return altGroupParam_; }
    const motifxs::Parameter* rcvOffParameter() const { return rcvOffParam_; }

    void setWaveform(int number) {
        const auto* w = motifxs::findWaveform(number);
        waveLabel_.setText(w ? juce::String(std::string(w->name)) + "   [" +
                                  juce::String(std::string(w->category)) + "  #" +
                                  juce::String(number) + "]"
                             : "waveform " + juce::String(number),
                           juce::dontSendNotification);
    }

    /// Applies a value read back from the rack without echoing it as an edit.
    void setValue(const motifxs::Parameter& p, int raw) {
        const juce::ScopedValueSetter<bool> guard(suppress_, true);
        if (altGroupParam_ == &p) {
            altGroup_.setSelectedId(juce::jlimit(0, 127, raw) + 1, juce::dontSendNotification);
            return;
        }
        if (rcvOffParam_ == &p) {
            rcvOff_.setSelectedId(raw ? 2 : 1, juce::dontSendNotification);
            return;
        }
        for (size_t i = 0; i < kControls.size(); ++i) {
            if (params_[i] != &p) continue;
            sliders_[i].setValue(kControls[i].bipolar ? raw - 64 : raw,
                                 juce::dontSendNotification);
            return;
        }
    }

    void setAvailable(bool yes, const juce::String& why) {
        available_ = yes;
        hint_.setText(why, juce::dontSendNotification);
        for (auto& s : sliders_) s.setEnabled(yes);
        altGroup_.setEnabled(yes);
        rcvOff_.setEnabled(yes);
        keys_.setEnabled(yes);
        repaint();
    }

    static std::span<const Control> controls() { return kControls; }
    const motifxs::Parameter* parameterAt(size_t i) const { return params_[i]; }

    void resized() override {
        auto r = getLocalBounds().reduced(2);
        auto top = r.removeFromTop(24);
        header_.setBounds(top.removeFromLeft(150));
        hint_.setBounds(top.removeFromRight(320));
        waveLabel_.setBounds(top);
        r.removeFromTop(4);
        keys_.setBounds(r.removeFromTop(juce::jmin(150, r.getHeight() / 2)));
        r.removeFromTop(10);

        auto row = r.removeFromTop(96);
        const int w = row.getWidth() / int(kControls.size());
        for (size_t i = 0; i < kControls.size(); ++i) {
            auto cell = row.removeFromLeft(w).reduced(3);
            labels_[i].setBounds(cell.removeFromTop(13));
            sliders_[i].setBounds(cell);
        }

        r.removeFromTop(14);
        auto behaviour = r.removeFromTop(26);
        behaviour.removeFromLeft(behaviour.getWidth() / 6);
        altGroupLabel_.setBounds(behaviour.removeFromLeft(90));
        behaviour.removeFromLeft(8);
        altGroup_.setBounds(behaviour.removeFromLeft(110));
        behaviour.removeFromLeft(28);
        rcvOffLabel_.setBounds(behaviour.removeFromLeft(80));
        behaviour.removeFromLeft(8);
        rcvOff_.setBounds(behaviour.removeFromLeft(150));
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(theme::bg);
        if (!available_) {
            g.setColour(theme::dim);
            g.setFont(juce::FontOptions(13.0f));
            g.drawText("No drum kit in the edit buffer", getLocalBounds(),
                       juce::Justification::centred);
        }
    }

private:
    void updateHeader() {
        header_.setText("KEY  " + DrumKeyMap::noteName(keys_.selected()),
                        juce::dontSendNotification);
    }

    // Deliberately the drum-specific set: no filter envelope, no LFO, no PEG --
    // a drum key has none of those. Alternate Group and Receive Note Off are
    // what make a kit behave like a kit (hi-hat choke, cymbal ring).
    static constexpr std::array<Control, 9> kControls{{
        {"LEVEL",    "drum_voice_key_element_level",         false},
        {"PAN",      "drum_voice_key_pan",                   false},
        {"TUNE",     "drum_voice_key_coarse_tune",           true},
        {"CUTOFF",   "drum_voice_key_lpf_cutoff_frequency",  false},
        {"RESO",     "drum_voice_key_lpf_resonance",         false},
        {"ATTACK",   "drum_voice_key_aeg_attack_time",       false},
        {"DECAY",    "drum_voice_key_aeg_decay_1_time",      false},
        {"REVERB",   "drum_voice_key_reverb_send_level",     false},
        {"CHORUS",   "drum_voice_key_chorus_send_level",     false},
    }};

    DrumKeyMap keys_;
    juce::Label header_, waveLabel_, hint_;
    std::array<juce::Slider, 9> sliders_;
    std::array<juce::Label, 9> labels_;
    std::array<const motifxs::Parameter*, 9> params_{};
    juce::ComboBox altGroup_, rcvOff_;
    juce::Label altGroupLabel_, rcvOffLabel_;
    const motifxs::Parameter* altGroupParam_{};
    const motifxs::Parameter* rcvOffParam_{};
    bool suppress_{false};
    bool available_{false};
};
