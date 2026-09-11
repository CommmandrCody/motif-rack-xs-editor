#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "motifxs/worker.hpp"

#include "AudioScope.h"
#include "MainComponent.h"

/// Opens an audio input so the standalone app can show the rack's output.
///
/// Input only -- the app makes no sound of its own, and opening an output would
/// put it in the way of whatever else is using the interface. Feed it the input
/// the rack is plugged into.
class StandaloneAudioTap : public AudioTap, private juce::AudioIODeviceCallback {
public:
    StandaloneAudioTap() {
        ring_.fill(0.0f);
        manager_.initialiseWithDefaultDevices(2, 0);
        enableAllInputs();
        manager_.addAudioCallback(this);
    }
    ~StandaloneAudioTap() override { manager_.removeAudioCallback(this); }

    juce::AudioDeviceManager& manager() { return manager_; }

    int readRecent(float* dest, int count) override {
        const int n = juce::jmin(count, kSize);
        const int w = write_.load(std::memory_order_acquire);
        for (int i = 0; i < n; ++i)
            dest[i] = ring_[std::size_t((w - n + i + kSize) & (kSize - 1))];
        for (int i = n; i < count; ++i) dest[i] = 0.0f;
        return active_.load() ? n : 0;
    }
    double tapSampleRate() const override { return rate_.load(); }

    juce::String tapStatus() override {
        if (!active_.load())
            return "No audio input is open.\nChoose the interface input the rack "
                   "is plugged into, and allow microphone access when macOS asks.";
        if (!everSaw_.load())
            return "Input open, but nothing on this pair yet.\n"
                   "Choose the pair the rack is plugged into.";
        return {};
    }

    juce::StringArray inputDevices() override {
        juce::StringArray names;
        for (auto* type : manager_.getAvailableDeviceTypes()) {
            type->scanForDevices();
            names.addArray(type->getDeviceNames(true));   // inputs
        }
        names.removeDuplicates(false);
        return names;
    }
    juce::String currentInputDevice() override {
        if (auto* d = manager_.getCurrentAudioDevice()) return d->getName();
        return {};
    }
    void setInputDevice(const juce::String& name) override {
        auto setup = manager_.getAudioDeviceSetup();
        setup.inputDeviceName = name;
        setup.useDefaultInputChannels = true;
        manager_.setAudioDeviceSetup(setup, true);
        enableAllInputs();
        pairBase_.store(0);
        everSaw_.store(false);
    }

    juce::StringArray inputPairs() override {
        juce::StringArray pairs;
        if (auto* d = manager_.getCurrentAudioDevice()) {
            const auto names = d->getInputChannelNames();
            for (int i = 0; i + 1 < names.size(); i += 2)
                pairs.add(juce::String(i + 1) + "/" + juce::String(i + 2) +
                          "   " + names[i].trim());
        }
        return pairs;
    }
    int currentInputPair() override { return pairBase_.load() / 2; }
    void setInputPair(int index) override {
        pairBase_.store(juce::jmax(0, index) * 2);
        everSaw_.store(false);
    }

private:
    /// Open every input the interface has, so any pair can be chosen in
    /// software without reconfiguring the device each time.
    void enableAllInputs() {
        auto setup = manager_.getAudioDeviceSetup();
        if (auto* d = manager_.getCurrentAudioDevice()) {
            setup.useDefaultInputChannels = false;
            setup.inputChannels.clear();
            setup.inputChannels.setRange(0, d->getInputChannelNames().size(), true);
            manager_.setAudioDeviceSetup(setup, true);
        }
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* d) override {
        rate_.store(d != nullptr ? d->getCurrentSampleRate() : 48000.0);
        active_.store(true);
    }
    void audioDeviceStopped() override { active_.store(false); }

    void audioDeviceIOCallbackWithContext(const float* const* in, int numIn,
                                          float* const* out, int numOut,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext&) override {
        for (int ch = 0; ch < numOut; ++ch)
            if (out[ch] != nullptr) juce::FloatVectorOperations::clear(out[ch], numSamples);
        if (numIn <= 0 || in == nullptr) return;

        // Only the selected pair, not a sum of every input on the desk.
        const int base = juce::jlimit(0, juce::jmax(0, numIn - 1), pairBase_.load());
        const int last = juce::jmin(numIn - 1, base + 1);

        int w = write_.load(std::memory_order_relaxed);
        for (int i = 0; i < numSamples; ++i) {
            float sum = 0.0f;
            int used = 0;
            for (int ch = base; ch <= last; ++ch)
                if (in[ch] != nullptr) { sum += in[ch][i]; ++used; }
            const float v = used ? sum / float(used) : 0.0f;
            if (std::abs(v) > 1.0e-5f) everSaw_.store(true);
            ring_[std::size_t(w)] = v;
            w = (w + 1) & (kSize - 1);
        }
        write_.store(w, std::memory_order_release);
    }

    static constexpr int kSize = 1 << 13;
    juce::AudioDeviceManager manager_;
    std::array<float, kSize> ring_{};
    std::atomic<int> write_{0};
    std::atomic<double> rate_{48000.0};
    std::atomic<bool> active_{false};
    std::atomic<bool> everSaw_{false};
    std::atomic<int> pairBase_{0};
};

class MotifXsApplication : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "Motif Rack XS"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise(const juce::String&) override {
        window_ = std::make_unique<Window>(getApplicationName());
    }
    void shutdown() override { window_.reset(); }
    void systemRequestedQuit() override { quit(); }

private:
    class Window : public juce::DocumentWindow {
    public:
        explicit Window(const juce::String& name)
            : DocumentWindow(name, theme::bg, DocumentWindow::allButtons) {
            setUsingNativeTitleBar(true);
            setContentOwned(new MainComponent(worker_, &tap_), true);
            setResizable(true, true);
            setResizeLimits(980, 620, 3000, 2000);
            centreWithSize(getWidth(), getHeight());
            setVisible(true);
        }
        ~Window() override {
            // Members are destroyed before base classes, so worker_ would go
            // first and DocumentWindow would then destroy MainComponent, whose
            // destructor uses it. Drop the content while the worker is alive.
            clearContentComponent();
        }

        void closeButtonPressed() override {
            JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        motifxs::DeviceWorker worker_;
        StandaloneAudioTap tap_;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Window)
    };

    std::unique_ptr<Window> window_;
};

START_JUCE_APPLICATION(MotifXsApplication)
