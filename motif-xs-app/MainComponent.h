#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <memory>

#include <juce_data_structures/juce_data_structures.h>

#include "motifxs/catalog.hpp"
#include "motifxs/parameters.hpp"
#include "motifxs/state.hpp"
#include "motifxs/worker.hpp"

#include "Browsers.h"
#include "DrumEditor.h"
#include "AudioScope.h"
#include "ElementEditor.h"
#include "Theme.h"

/// One labelled knob bound to a Multi Part parameter.
class ParamKnob : public juce::Component {
public:
    ParamKnob(const juce::String& label, const char* parameterId)
        : id_(parameterId) {
        param_ = motifxs::findParameterById(parameterId);
        jassert(param_ != nullptr);

        name_.setText(label, juce::dontSendNotification);
        name_.setJustificationType(juce::Justification::centred);
        name_.setFont(theme::panelFont(10.5f));
        name_.setColour(juce::Label::textColourId, theme::dim);
        addAndMakeVisible(name_);

        slider_.setSliderStyle(juce::Slider::RotaryVerticalDrag);
        slider_.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 50, 14);
        slider_.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        slider_.setColour(juce::Slider::textBoxTextColourId, theme::text);
        if (param_) {
            // Offset parameters are stored 0..127 with 64 as zero; show them
            // the way the hardware does, centred on 0.
            bipolar_ = param_->description.find("-64") != std::string_view::npos;
            if (bipolar_) slider_.setRange(-64.0, 63.0, 1.0);
            else slider_.setRange(param_->min, param_->max, 1.0);
        }
        slider_.textFromValueFunction = [this](double v) {
            return format(bipolar_ ? int(v) + 64 : int(v));
        };
        slider_.valueFromTextFunction = [](const juce::String& s) { return s.getDoubleValue(); };
        addAndMakeVisible(slider_);
    }

    void resized() override {
        auto r = getLocalBounds();
        name_.setBounds(r.removeFromTop(12));
        slider_.setBounds(r);
    }

    /// Raw device value (0..127) -> displayed value.
    void setRaw(int raw) {
        slider_.setValue(bipolar_ ? raw - 64 : raw, juce::dontSendNotification);
    }
    [[nodiscard]] int raw() const {
        const int v = int(slider_.getValue());
        return bipolar_ ? v + 64 : v;
    }

    [[nodiscard]] const motifxs::Parameter* parameter() const { return param_; }
    juce::Slider& slider() { return slider_; }

    /// Formats a raw device value the way the rack's own display would.
    [[nodiscard]] juce::String format(int raw) const;

private:
    const char* id_;
    const motifxs::Parameter* param_{};
    bool bipolar_{false};
    juce::Label name_;
    juce::Slider slider_;
};

class MainComponent : public juce::Component, private juce::Timer {
public:
    /// The connection is owned by the host (the standalone app, or the plugin
    /// processor), not by the UI. A plugin editor is created and destroyed
    /// every time the user opens the window; the connection must outlive that.
    /// `tap` is optional: the plugin passes whatever audio reaches its track,
    /// the standalone app has none.
    explicit MainComponent(motifxs::DeviceWorker&, AudioTap* tap = nullptr);
    ~MainComponent() override;

    /// Selects the part the UI is editing. In the plugin this is bound to the
    /// automatable "part" parameter, so the part strip and the parameter the
    /// host automates are the same thing rather than drifting apart.
    void setPart(int part) { selectPart(part); }
    [[nodiscard]] int currentPart() const { return part_; }

    /// Fired when the user picks a part in the strip. The plugin uses this to
    /// keep the host parameter in step.
    std::function<void(int)> onPartChanged;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void refreshPorts();
    void connect();
    void selectPart(int part);
    void pullPartState();
    void pushKnob(ParamKnob&);
    void updateArpWarning();
    void refreshThruDestinations();
    void saveState();
    void loadState();
    void refreshDrumPage();
    void pullDrumKey(int ee);
    void refreshElementPage();
    void pullElement(int ee);
    void setStatus(const juce::String&, juce::Colour);
    void timerCallback() override;

    motifxs::DeviceWorker& worker_;

    // header
    juce::ComboBox portBox_;
    juce::TextButton connectButton_{"Connect"};
    juce::Label statusLabel_, deviceLabel_;

    // parts
    std::array<std::unique_ptr<juce::TextButton>, 16> partButtons_;
    std::array<juce::String, 16> partVoiceNames_;
    int part_{0};

    VoiceBrowser voices_;
    ArpBrowser arps_;
    DrumEditor drums_;
    ElementEditor elements_;
    AudioScope scope_;

    // Voice and Arpeggio each get the full width instead of sharing it. The
    // common case is picking a voice; the arp browser was taking half the
    // window to answer a question the user was not asking.
    juce::TabbedComponent tabs_{juce::TabbedButtonBar::TabsAtTop};

    juce::TextButton panicButton_{"PANIC"};
    // A DAW project needs the rack's setup back, not just the notes played
    // into it. These capture and restore the whole Multi.
    juce::TextButton saveButton_{"SAVE"};
    juce::TextButton loadButton_{"LOAD"};
    std::unique_ptr<juce::FileChooser> chooser_;
    // Arp state stays visible on every tab, so "what is armed on this part"
    // never needs a tab switch -- it is also what latches and runs away.
    juce::TextButton arpSwitch_{"ARP"};
    juce::TextButton arpHold_{"HOLD"};
    juce::ComboBox arpSlot_;
    juce::Label arpNameLabel_;
    // "OUT" arms the rack's per-part ARP MIDI Out; the thru box chooses where
    // those notes go. Both are needed, so they sit together.
    juce::TextButton arpMidiOut_{"OUT"};
    juce::ComboBox thruBox_;
    juce::Label thruLabel_;

    // Clicking a voice should let you hear it; browsing 1217 voices silently
    // is not browsing, it is reading a list.
    juce::TextButton auditionButton_{"AUDITION"};
    // A voice edited on the rack exists only in the part's edit buffer. These
    // save it whole as a file and put it back on any part, which is the only
    // way an edit outlives a patch change.
    juce::TextButton saveVoiceButton_{"SAVE PATCH"};
    juce::TextButton loadVoiceButton_{"LOAD PATCH"};
    void saveCustomPatch();
    void loadCustomPatch();
    int auditionNote_{60};
    void audition();
    void stopAudition();
    bool auditionSounding_{false};

    bool retriedAuto_{false};
    std::unique_ptr<juce::PropertiesFile> settings_;
    void loadSettings();
    void saveSettings();

    std::array<std::unique_ptr<ParamKnob>, 9> knobs_;

    std::atomic<bool> dirty_{false};
    juce::String pendingStatus_;
    juce::Colour pendingStatusColour_{theme::dim};

    theme::Look look_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
