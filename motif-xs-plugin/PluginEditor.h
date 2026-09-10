#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

#include "MainComponent.h"
#include "PluginProcessor.h"

/// The plugin window is the standalone app's UI plus a strip for project
/// state, so there is one editor to maintain rather than two that drift.
class MotifXsEditor : public juce::AudioProcessorEditor {
public:
    explicit MotifXsEditor(MotifXsProcessor&);
    ~MotifXsEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    MotifXsProcessor& processor_;
    MainComponent ui_;

    juce::Label stateLabel_;
    juce::TextButton captureButton_{"CAPTURE FOR PROJECT"};
    juce::Label hint_;
    theme::Look look_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MotifXsEditor)
};
