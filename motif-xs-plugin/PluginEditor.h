#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

#include "MainComponent.h"
#include "PluginProcessor.h"

/// The plugin window is the standalone app's UI plus a strip for project
/// state, so there is one editor to maintain rather than two that drift.
class MotifXsEditor : public juce::AudioProcessorEditor,
                      private juce::AudioProcessorValueTreeState::Listener,
                      private juce::Timer {
public:
    explicit MotifXsEditor(MotifXsProcessor&);
    ~MotifXsEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    /// Keeps the part strip and the automatable "part" parameter in step,
    /// in both directions, without letting them chase each other.
    void parameterChanged(const juce::String& id, float value) override;
    /// Keeps the state readout honest about whether a save right now would
    /// bring the rack back.
    void timerCallback() override;
    bool syncing_{false};

    MotifXsProcessor& processor_;
    MainComponent ui_;

    juce::Label stateLabel_;
    juce::TextButton captureButton_{"CAPTURE NOW"};
    juce::TextButton midiOutButton_{"ARP -> DAW"};
    juce::Label hint_;
    theme::Look look_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MotifXsEditor)
};
