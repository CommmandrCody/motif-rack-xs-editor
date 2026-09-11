#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>

#include "motifxs/state.hpp"
#include "motifxs/worker.hpp"

#include "AudioScope.h"
#include "Macros.h"

/// The rack is the sound engine; this plugin is a controller that happens to
/// live in a DAW track.
///
/// It produces no audio and sends no MIDI through the host. All device traffic
/// goes out its own CoreMIDI connection, because VST3 SysEx output is not
/// dependable -- Steinberg treats VST3 as an audio API, and JUCE's VST3 wrapper
/// strips the F0/F7 framing so SysEx fails in most third-party hosts. Since
/// essentially everything this device needs is SysEx, routing control traffic
/// through the VST3 event path would depend on the least reliable part of the
/// API. See docs/vst-architecture.md.
class MotifXsProcessor : public juce::AudioProcessor,
                         private juce::Timer,
                         public AudioTap {
public:
    MotifXsProcessor();
    ~MotifXsProcessor() override;

    void prepareToPlay(double sampleRate, int) override { sampleRate_.store(sampleRate); }
    void releaseResources() override {}
    /// Accepting any layout at all lets a host instantiate this with no input
    /// bus, after which no audio ever reaches processBlock and the scope has
    /// nothing to draw with no way to say why.
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override {
        const auto out = layouts.getMainOutputChannelSet();
        if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
            return false;
        const auto in = layouts.getMainInputChannelSet();
        return in.isDisabled() || in == out;
    }

    /// Silent passthrough. Nothing here may allocate, lock or send: automation
    /// is pushed into a lock-free queue and the worker thread does the MIDI.
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Motif Rack XS"; }
    bool acceptsMidi() const override { return true; }
    /// The rack's arpeggiator output is relayed to the host as ordinary note
    /// and controller events, so a DAW track can record the phrase. Note events
    /// through the VST3 output bus are reliable; it is SysEx that is not, and
    /// none of the device's SysEx goes this way.
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    /// Project state: the captured Multi, stored as the same text blob the
    /// standalone app writes to a .motifxs file.
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    motifxs::DeviceWorker& worker() { return *worker_; }

    /// AudioTap: recent mono samples for the scope.
    int readRecent(float* dest, int count) override;
    double tapSampleRate() const override { return sampleRate_.load(); }
    juce::String tapStatus() override;
    juce::AudioProcessorValueTreeState& apvts() { return apvts_; }

    /// True while the rack's arpeggiator is being relayed to the host.
    [[nodiscard]] bool midiOutEnabled() const;
    void setMidiOutEnabled(bool);

    /// Captures the rack now, so the next host save carries it.
    void captureNow(std::function<void(bool, juce::String)> done = {});
    [[nodiscard]] juce::String stateSummary() const;

    /// True when the rack has been changed since the last capture, so a project
    /// saved right now would not bring back what you are hearing.
    [[nodiscard]] bool stateIsStale() const;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout();
    /// Drains automation movement on the message thread at a musical rate, and
    /// keeps the saved state current. processBlock only flags that something
    /// moved; sending happens here and then on the worker, so the audio thread
    /// never touches CoreMIDI.
    void timerCallback() override {
        pushAutomationToDevice();
        maybeAutoCapture();
    }
    void pushAutomationToDevice();

    /// Shared with every other instance in this host process.
    std::shared_ptr<motifxs::DeviceWorker> worker_;
    juce::AudioProcessorValueTreeState apvts_;

    mutable juce::CriticalSection stateLock_;
    motifxs::State captured_;
    juce::String portName_;

    /// Last value pushed per automatable parameter, so processBlock only
    /// enqueues genuine changes rather than a message per block.
    std::vector<std::atomic<float>> lastPushed_;
    std::atomic<bool> automationDirty_{false};

    /// Channel messages arriving from the rack, handed from the CoreMIDI read
    /// thread to the audio thread without a lock.
    struct RelayedMessage {
        std::uint8_t bytes[3]{};
        std::uint8_t size{};
    };
    static constexpr int kRelayCapacity = 2048;
    juce::AbstractFifo relayFifo_{kRelayCapacity};
    std::array<RelayedMessage, kRelayCapacity> relayQueue_{};
    std::atomic<bool> relayOn_{false};
    std::atomic<int> relayDropped_{0};

    /// Automatic capture. getStateInformation cannot block on a five-second
    /// round trip to the rack, so the state has to already be there when the
    /// host asks -- which means capturing ahead of time, not on demand.
    void maybeAutoCapture();
    std::atomic<bool> transportPlaying_{false};

    /// A ring of recent mono samples, written on the audio thread and read by
    /// the editor. Approximate by design: the scope is for looking at, so a
    /// torn read costs a slightly ragged frame and nothing else.
    static constexpr int kScopeSize = 1 << 13;
    std::array<float, kScopeSize> scopeRing_{};
    std::atomic<int> scopeWrite_{0};
    std::atomic<double> sampleRate_{48000.0};
    std::atomic<int> inputChannels_{0};
    std::atomic<bool> everSawSignal_{false};
    std::atomic<bool> capturing_{false};
    std::uint64_t lastSeenChange_{0};
    std::uint64_t capturedAtChange_{0};
    juce::int64 lastChangeMs_{0};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MotifXsProcessor)
};
