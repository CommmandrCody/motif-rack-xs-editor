#pragma once
#include <juce_dsp/juce_dsp.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <array>
#include <cmath>
#include <vector>

#include "Theme.h"

/// Somewhere recent audio can be read from for display.
///
/// The plugin implements this over whatever audio reaches its track -- the
/// rack's own output, if it is returned through an External Instrument device
/// or an audio track. The standalone app opens no audio device, so it has none
/// and the page says so rather than drawing a flat line and implying silence.
struct AudioTap {
    virtual ~AudioTap() = default;
    /// Copies up to `count` of the most recent mono samples, newest last.
    /// Returns how many were written.
    virtual int readRecent(float* dest, int count) = 0;
    virtual double tapSampleRate() const = 0;

    /// Optional: lets the page choose which input to listen to. Without this a
    /// tap is stuck on whatever the system default is, which is usually the
    /// built-in microphone rather than the interface the rack is plugged into.
    /// Why the scope is showing nothing, in words. "No signal" and "this track
    /// never sends me any audio" look identical on a flat line, and they have
    /// completely different fixes.
    virtual juce::String tapStatus() { return {}; }

    virtual juce::StringArray inputDevices() { return {}; }
    virtual juce::String currentInputDevice() { return {}; }
    virtual void setInputDevice(const juce::String&) {}

    /// Which stereo pair on the interface to listen to. A rack on an 18-input
    /// desk is rarely on channels 1/2, and picking the device alone leaves you
    /// staring at whatever happens to be on the first pair.
    virtual juce::StringArray inputPairs() { return {}; }
    virtual int currentInputPair() { return 0; }
    virtual void setInputPair(int) {}
};

/// A waveform and a spectrum of whatever the tap is hearing.
class AudioScope : public juce::Component, private juce::Timer {
public:
    explicit AudioScope(AudioTap* tap) : tap_(tap) {
        if (tap_ != nullptr) {
            const auto devices = tap_->inputDevices();
            if (!devices.isEmpty()) {
                inputLabel_.setText("INPUT", juce::dontSendNotification);
                inputLabel_.setFont(theme::panelFont(11.0f));
                inputLabel_.setColour(juce::Label::textColourId, theme::dim);
                inputLabel_.setJustificationType(juce::Justification::centredRight);
                addAndMakeVisible(inputLabel_);

                int id = 1;
                for (const auto& d : devices) input_.addItem(d, id++);
                input_.setText(tap_->currentInputDevice(), juce::dontSendNotification);
                input_.onChange = [this] {
                    if (tap_ != nullptr) tap_->setInputDevice(input_.getText());
                };
                addAndMakeVisible(input_);
                hasSelector_ = true;

                pair_.onChange = [this] {
                    if (tap_ != nullptr) tap_->setInputPair(pair_.getSelectedId() - 1);
                };
                addAndMakeVisible(pair_);
                refreshPairs();

                // The pair list only exists once a device is open, so rebuild
                // it whenever the device changes.
                input_.onChange = [this] {
                    if (tap_ == nullptr) return;
                    tap_->setInputDevice(input_.getText());
                    refreshPairs();
                };
            }
        }
        window_.resize(size_t(kFftSize));
        juce::dsp::WindowingFunction<float> w(size_t(kFftSize),
                                              juce::dsp::WindowingFunction<float>::hann);
        std::fill(window_.begin(), window_.end(), 1.0f);
        w.multiplyWithWindowingTable(window_.data(), size_t(kFftSize));

        samples_.resize(size_t(kFftSize), 0.0f);
        fftBuffer_.resize(size_t(kFftSize) * 2, 0.0f);
        magnitudes_.fill(0.0f);
        startTimerHz(30);
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(theme::bg);
        auto r = getLocalBounds().reduced(10);
        if (hasSelector_) r.removeFromTop(30);

        if (tap_ == nullptr) {
            g.setColour(theme::dim);
            g.setFont(juce::FontOptions(13.0f));
            g.drawText("No audio reaching this plugin.\n\n"
                       "Return the rack's audio to this track -- an External Instrument "
                       "device, or an audio track fed from your interface -- and it will "
                       "be drawn here.",
                       r.reduced(60), juce::Justification::centred, true);
            return;
        }

        auto wave = r.removeFromTop(r.getHeight() / 2 - 6);
        r.removeFromTop(12);
        drawWaveform(g, wave);
        drawSpectrum(g, r);
    }

    void resized() override {
        if (!hasSelector_) return;
        auto top = getLocalBounds().reduced(10).removeFromTop(24);
        pair_.setBounds(top.removeFromRight(190));
        top.removeFromRight(8);
        input_.setBounds(top.removeFromRight(260));
        inputLabel_.setBounds(top.removeFromRight(50));
    }

private:
    static constexpr int kFftOrder = 11;               // 2048
    static constexpr int kFftSize = 1 << kFftOrder;
    static constexpr int kBands = 128;

    void timerCallback() override {
        if (tap_ == nullptr) return;
        const int got = tap_->readRecent(samples_.data(), kFftSize);
        if (got <= 0) {
            // decay toward silence so a stopped source fades rather than freezing
            peak_ *= 0.85f;
            for (auto& m : magnitudes_) m *= 0.85f;
            repaint();
            return;
        }

        peak_ = 0.0f;
        for (int i = 0; i < got; ++i) peak_ = juce::jmax(peak_, std::abs(samples_[size_t(i)]));

        std::fill(fftBuffer_.begin(), fftBuffer_.end(), 0.0f);
        for (int i = 0; i < kFftSize; ++i)
            fftBuffer_[size_t(i)] = samples_[size_t(i)] * window_[size_t(i)];
        fft_.performFrequencyOnlyForwardTransform(fftBuffer_.data());

        // Log-spaced bands: a linear FFT axis spends most of its width on
        // frequencies an ear barely distinguishes.
        const double sr = tap_->tapSampleRate() > 0 ? tap_->tapSampleRate() : 48000.0;
        const double nyquist = sr * 0.5;
        for (int b = 0; b < kBands; ++b) {
            const double f0 = 20.0 * std::pow(nyquist / 20.0, double(b) / kBands);
            const double f1 = 20.0 * std::pow(nyquist / 20.0, double(b + 1) / kBands);
            const int i0 = juce::jlimit(1, kFftSize / 2 - 1, int(f0 / nyquist * (kFftSize / 2)));
            const int i1 = juce::jlimit(i0 + 1, kFftSize / 2, int(f1 / nyquist * (kFftSize / 2)));
            float peak = 0.0f;
            for (int i = i0; i < i1; ++i) peak = juce::jmax(peak, fftBuffer_[size_t(i)]);
            const float db = juce::Decibels::gainToDecibels(peak / float(kFftSize / 4) + 1.0e-9f);
            const float norm = juce::jlimit(0.0f, 1.0f, (db + 90.0f) / 90.0f);
            auto& m = magnitudes_[size_t(b)];
            m = norm > m ? norm : m * 0.80f + norm * 0.20f;   // fast attack, slow release
        }
        repaint();
    }

    void drawWaveform(juce::Graphics& g, juce::Rectangle<int> area) {
        g.setColour(theme::panel);
        g.fillRoundedRectangle(area.toFloat(), 4.0f);
        g.setColour(theme::line);
        g.drawHorizontalLine(area.getCentreY(), float(area.getX()), float(area.getRight()));

        juce::Path p;
        const int n = kFftSize;
        const float halfH = area.getHeight() * 0.46f;
        for (int x = 0; x < area.getWidth(); ++x) {
            const int i = juce::jlimit(0, n - 1, int(double(x) / area.getWidth() * n));
            const float v = juce::jlimit(-1.0f, 1.0f, samples_[size_t(i)]);
            const float y = area.getCentreY() - v * halfH;
            if (x == 0) p.startNewSubPath(float(area.getX() + x), y);
            else p.lineTo(float(area.getX() + x), y);
        }
        g.setColour(theme::accent);
        g.strokePath(p, juce::PathStrokeType(1.4f));

        g.setColour(theme::dim);
        g.setFont(theme::panelFont(10.5f));
        g.drawText("WAVEFORM", area.reduced(6), juce::Justification::topLeft);
        const juce::String status = tap_ != nullptr ? tap_->tapStatus() : juce::String();
        g.drawText(peak_ > 0.0001f
                       ? juce::String(juce::Decibels::gainToDecibels(peak_), 1) + " dB peak"
                       : (status.isNotEmpty() ? status : juce::String("silent")),
                   area.reduced(6), juce::Justification::topRight);

        if (peak_ <= 0.0001f && status.isNotEmpty()) {
            g.setColour(theme::warn);
            g.setFont(juce::FontOptions(12.0f));
            g.drawText(status, area, juce::Justification::centred, true);
        }
    }

    void drawSpectrum(juce::Graphics& g, juce::Rectangle<int> area) {
        g.setColour(theme::panel);
        g.fillRoundedRectangle(area.toFloat(), 4.0f);

        const float w = area.getWidth() / float(kBands);
        for (int b = 0; b < kBands; ++b) {
            const float h = magnitudes_[size_t(b)] * area.getHeight() * 0.92f;
            if (h < 1.0f) continue;
            const juce::Rectangle<float> bar(area.getX() + b * w, area.getBottom() - h,
                                             juce::jmax(1.0f, w - 1.0f), h);
            g.setColour(theme::accent.withBrightness(0.55f + 0.45f * magnitudes_[size_t(b)]));
            g.fillRect(bar);
        }

        g.setColour(theme::dim);
        g.setFont(theme::panelFont(10.5f));
        g.drawText("SPECTRUM   20 Hz - Nyquist, log", area.reduced(6),
                   juce::Justification::topLeft);
    }

    AudioTap* tap_{};
    void refreshPairs() {
        if (tap_ == nullptr) return;
        pair_.clear(juce::dontSendNotification);
        int id = 1;
        for (const auto& p : tap_->inputPairs()) pair_.addItem(p, id++);
        pair_.setSelectedId(tap_->currentInputPair() + 1, juce::dontSendNotification);
    }

    juce::ComboBox input_, pair_;
    juce::Label inputLabel_;
    bool hasSelector_{false};
    juce::dsp::FFT fft_{kFftOrder};
    std::vector<float> window_, samples_, fftBuffer_;
    std::array<float, kBands> magnitudes_{};
    float peak_{0.0f};
};
