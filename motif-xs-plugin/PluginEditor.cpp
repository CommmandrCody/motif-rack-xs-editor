#include "PluginEditor.h"

MotifXsEditor::MotifXsEditor(MotifXsProcessor& p)
    : AudioProcessorEditor(&p), processor_(p), ui_(p.worker()) {
    setLookAndFeel(&look_);
    addAndMakeVisible(ui_);

    stateLabel_.setFont(juce::FontOptions(11.0f));
    stateLabel_.setColour(juce::Label::textColourId, theme::dim);
    stateLabel_.setText("project state: " + processor_.stateSummary(),
                        juce::dontSendNotification);
    addAndMakeVisible(stateLabel_);

    hint_.setFont(juce::FontOptions(11.0f));
    hint_.setColour(juce::Label::textColourId, theme::dim);
    hint_.setJustificationType(juce::Justification::centredRight);
    hint_.setText("capture before saving the project, or the rack's setup is not stored",
                  juce::dontSendNotification);
    addAndMakeVisible(hint_);

    // The host saves whatever was last captured. Doing this automatically on
    // every host save is not possible -- getStateInformation must not block on
    // a 2.7 kB round trip to the rack -- so it is an explicit action.
    captureButton_.setColour(juce::TextButton::buttonColourId, theme::accentDim);
    captureButton_.onClick = [this] {
        stateLabel_.setText("capturing...", juce::dontSendNotification);
        processor_.captureNow([this](bool ok, juce::String message) {
            juce::MessageManager::callAsync([this, ok, message] {
                stateLabel_.setText((ok ? "project state: " : "capture failed: ") + message,
                                    juce::dontSendNotification);
                stateLabel_.setColour(juce::Label::textColourId,
                                      ok ? theme::good : theme::bad);
                if (ok) processor_.updateHostDisplay();
            });
        });
    };
    addAndMakeVisible(captureButton_);

    // The part strip and the host's "part" parameter are one thing seen two
    // ways: picking a part in the UI moves the parameter that automation
    // targets, and host automation moves the UI.
    ui_.onPartChanged = [this](int part) {
        if (syncing_) return;
        if (auto* p = processor_.apvts().getParameter("part")) {
            const juce::ScopedValueSetter<bool> guard(syncing_, true);
            p->setValueNotifyingHost(p->convertTo0to1(float(part + 1)));
        }
    };
    processor_.apvts().addParameterListener("part", this);
    if (auto* raw = processor_.apvts().getRawParameterValue("part")) {
        const juce::ScopedValueSetter<bool> guard(syncing_, true);
        ui_.setPart(int(raw->load()) - 1);
    }

    setResizable(true, true);
    setResizeLimits(980, 620, 3000, 2000);
    setSize(1240, 800);
}

MotifXsEditor::~MotifXsEditor() {
    processor_.apvts().removeParameterListener("part", this);
    ui_.onPartChanged = nullptr;
    setLookAndFeel(nullptr);
}

void MotifXsEditor::parameterChanged(const juce::String& id, float value) {
    if (id != "part" || syncing_) return;
    // Automation arrives on the host's thread; touching the UI needs the
    // message thread.
    juce::MessageManager::callAsync([this, value] {
        const juce::ScopedValueSetter<bool> guard(syncing_, true);
        ui_.setPart(int(value) - 1);
    });
}

void MotifXsEditor::paint(juce::Graphics& g) { g.fillAll(theme::bg); }

void MotifXsEditor::resized() {
    auto r = getLocalBounds();
    auto strip = r.removeFromBottom(30).reduced(10, 4);
    captureButton_.setBounds(strip.removeFromLeft(190));
    strip.removeFromLeft(10);
    stateLabel_.setBounds(strip.removeFromLeft(320));
    hint_.setBounds(strip);
    ui_.setBounds(r);
}
