#include "PluginEditor.h"

MotifXsEditor::MotifXsEditor(MotifXsProcessor& p)
    : AudioProcessorEditor(&p), processor_(p), ui_(p.worker(), &p) {
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
    hint_.setText("the rack is captured automatically a few seconds after you stop editing",
                  juce::dontSendNotification);
    addAndMakeVisible(hint_);

    // Capture runs by itself once editing stops and the transport is idle, so
    // saving a project normally just works. This button is for forcing it --
    // getStateInformation cannot block on a five-second round trip to the rack,
    // so the state has to already be there when the host asks.
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

    // The editor knobs and the host parameters address the same rack
    // parameter. Without this they were simply two unconnected paths: a knob
    // mapped to Push moved the rack while the editor sat still, and a knob
    // turned here wrote no automation.
    ui_.onMacroChanged = [this](int index, int raw) {
        if (syncing_ || index < 0 || index >= int(macros::kCount)) return;
        if (auto* p = processor_.apvts().getParameter(macros::kAll[std::size_t(index)].id)) {
            const juce::ScopedValueSetter<bool> guard(syncing_, true);
            p->setValueNotifyingHost(p->convertTo0to1(float(raw)));
        }
    };
    for (const auto& m : macros::kAll) processor_.apvts().addParameterListener(m.id, this);
    if (auto* raw = processor_.apvts().getRawParameterValue("part")) {
        const juce::ScopedValueSetter<bool> guard(syncing_, true);
        ui_.setPart(int(raw->load()) - 1);
    }
    syncFromHost();

    // Relays the rack's arpeggiator into the track as note events, so the
    // phrase can be recorded without an IAC bus in the middle.
    midiOutButton_.setClickingTogglesState(true);
    midiOutButton_.setToggleState(processor_.midiOutEnabled(), juce::dontSendNotification);
    midiOutButton_.setColour(juce::TextButton::buttonOnColourId, theme::good);
    midiOutButton_.setColour(juce::TextButton::textColourOnId, juce::Colours::black);
    midiOutButton_.setColour(juce::TextButton::textColourOffId, theme::dim);
    midiOutButton_.setTooltip("Send the rack's arpeggiator to this track as MIDI. "
                              "Arm ARP and OUT on the part as well.");
    midiOutButton_.onClick = [this] {
        const bool on = midiOutButton_.getToggleState();
        processor_.setMidiOutEnabled(on);
        hint_.setText(on ? "arp is being sent to this track - arm ARP and OUT on the part too"
                         : "the rack is captured automatically a few seconds after you stop editing",
                      juce::dontSendNotification);
    };
    addAndMakeVisible(midiOutButton_);

    setResizable(true, true);
    // A Live device shares a chain with others; it should not open the size of
    // a desktop app. Still resizable for real editing work.
    setResizeLimits(880, 520, 3000, 2000);
    setSize(1000, 640);
    startTimerHz(2);
}

void MotifXsEditor::timerCallback() {
    // Nothing else matters if the rack is not ours: no edit, capture or
    // restore can work, and the cause is almost always the standalone app.
    if (const auto blocked = processor_.deviceError(); blocked.isNotEmpty()) {
        stateLabel_.setText(blocked.contains("already in use")
                                ? blocked + " - close it and this will reconnect"
                                : blocked,
                            juce::dontSendNotification);
        stateLabel_.setColour(juce::Label::textColourId, theme::bad);
        return;
    }
    // A restore in progress is the more urgent thing to say; it takes long
    // enough that silence would look like nothing was happening.
    const auto restore = processor_.restoreStatus();
    if (restore.isNotEmpty() && restore.startsWith("restoring")) {
        stateLabel_.setText(restore, juce::dontSendNotification);
        stateLabel_.setColour(juce::Label::textColourId, theme::warn);
        return;
    }
    if (restore.startsWith("no rack") || restore.startsWith("restore failed")) {
        stateLabel_.setText(restore, juce::dontSendNotification);
        stateLabel_.setColour(juce::Label::textColourId, theme::bad);
        return;
    }
    const bool stale = processor_.stateIsStale();
    stateLabel_.setText(stale ? "project state: capturing changes..."
                              : "project state: " + processor_.stateSummary(),
                        juce::dontSendNotification);
    stateLabel_.setColour(juce::Label::textColourId, stale ? theme::warn : theme::dim);
}

MotifXsEditor::~MotifXsEditor() {
    stopTimer();
    processor_.apvts().removeParameterListener("part", this);
    for (const auto& m : macros::kAll) processor_.apvts().removeParameterListener(m.id, this);
    ui_.onPartChanged = nullptr;
    ui_.onMacroChanged = nullptr;
    setLookAndFeel(nullptr);
}

void MotifXsEditor::syncFromHost() {
    const juce::ScopedValueSetter<bool> guard(syncing_, true);
    for (std::size_t i = 0; i < macros::kCount; ++i)
        if (auto* raw = processor_.apvts().getRawParameterValue(macros::kAll[i].id))
            ui_.setMacroValue(int(i), int(raw->load()));
}

void MotifXsEditor::parameterChanged(const juce::String& id, float value) {
    if (syncing_) return;

    // A macro moved in the host -- Push, a control surface, or automation
    // playing back. Show it.
    for (std::size_t i = 0; i < macros::kCount; ++i) {
        if (id != macros::kAll[i].id) continue;
        const int index = int(i);
        const int raw = int(value);
        juce::MessageManager::callAsync([this, index, raw] {
            const juce::ScopedValueSetter<bool> guard(syncing_, true);
            ui_.setMacroValue(index, raw);
        });
        return;
    }

    if (id != "part") return;
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
    auto strip = r.removeFromBottom(26).reduced(8, 3);
    captureButton_.setBounds(strip.removeFromLeft(110));
    strip.removeFromLeft(6);
    midiOutButton_.setBounds(strip.removeFromLeft(96));
    strip.removeFromLeft(8);
    stateLabel_.setBounds(strip.removeFromLeft(280));
    hint_.setBounds(strip);
    ui_.setBounds(r);
}
