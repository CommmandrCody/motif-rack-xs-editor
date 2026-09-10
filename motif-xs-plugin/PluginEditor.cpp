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

    setResizable(true, true);
    setResizeLimits(980, 620, 3000, 2000);
    setSize(1240, 800);
}

MotifXsEditor::~MotifXsEditor() { setLookAndFeel(nullptr); }

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
