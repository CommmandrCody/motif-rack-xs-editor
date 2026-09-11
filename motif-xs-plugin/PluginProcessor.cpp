#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace motifxs;

namespace {

/// Writes what the plugin handed the host and what the host handed back.
///
/// A restore that goes wrong leaves no evidence otherwise: the rack reports
/// "illegal bulk data" on its own display and the blocks are gone. Having both
/// sides on disk makes it possible to tell a bad capture from a bad restore,
/// and lets a project's rack state be recovered by hand if the plugin is at
/// fault.
void writeDiagnostic(const juce::String& name, const juce::String& contents) {
    auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                   .getChildFile("Logs")
                   .getChildFile("MotifRackXS");
    if (!dir.exists() && !dir.createDirectory().wasOk()) return;
    dir.getChildFile(name).replaceWithText(contents);
}


/// The automatable set is deliberately small. The rack exposes 918 usable
/// parameters; publishing all of them makes a host's automation list unusable
/// and Live's device view unreadable. These are the PERFORM macros -- the same
/// controls the rack's own knobs drive -- for the part the plugin is pointed
/// at. Everything deeper stays plugin state, editable in the UI but not
/// automatable, which matches how the hardware behaves.
constexpr int kNumMacros = int(macros::kCount);

/// How long the rack must be untouched before an automatic capture runs.
/// Long enough not to fire between two knob turns, short enough that a project
/// saved shortly after editing is current.
constexpr juce::int64 kAutoCaptureQuietMs = 4000;

}  // namespace

juce::AudioProcessorValueTreeState::ParameterLayout MotifXsProcessor::makeLayout() {
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // Which part the macros address. Automatable so a track can switch parts,
    // though in practice one instance per part is the sane arrangement.
    layout.add(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"part", 1}, "Part", 1, 16, 1));

    for (const auto& m : macros::kAll) {
        const auto* p = findParameterById(m.parameter);
        const int lo = p ? p->min : 0;
        const int hi = p && p->max > p->min ? p->max : 127;
        layout.add(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{m.id, 1}, m.label,
            juce::NormalisableRange<float>(float(lo), float(hi), 1.0f),
            float(p && p->description.find("-64") != std::string_view::npos ? 64 : hi / 2)));
    }
    return layout;
}

MotifXsProcessor::MotifXsProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("In", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Out", juce::AudioChannelSet::stereo(), true)),
      worker_(sharedWorker()),
      apvts_(*this, nullptr, "motifxs", makeLayout()),
      lastPushed_(std::size_t(kNumMacros)) {
    for (auto& v : lastPushed_) v.store(-1.0f);

    // One rack, one port: open it as soon as the plugin is instantiated so the
    // UI is useful the moment it is opened.
    // Another instance may already have opened it; opening again is harmless
    // and re-confirms the device number.
    if (!worker_->isOpen())
        openDevice();
    else
        portName_ = juce::String(worker_->info().portName);

    // 30 Hz is fast enough to feel continuous and slow enough that a knob
    // sweep cannot outrun the rack's input buffer.
    startTimerHz(30);
}

bool MotifXsProcessor::midiOutEnabled() const { return relayOn_.load(); }

void MotifXsProcessor::setMidiOutEnabled(bool on) {
    relayOn_.store(on);
    if (!on) {
        worker_->post([](Device& d) { d.setChannelListener({}); });
        return;
    }
    // Only one listener slot exists on the shared connection, so with several
    // instances the most recently enabled one wins. One instance relaying is
    // the sane arrangement anyway -- the rack sends a single merged stream.
    worker_->post([this](Device& d) {
        d.setChannelListener([this](const Bytes& m) {
            if (!relayOn_.load() || m.empty() || m.size() > 3) return;
            int start1, size1, start2, size2;
            relayFifo_.prepareToWrite(1, start1, size1, start2, size2);
            if (size1 + size2 < 1) {          // the host is not draining
                relayDropped_.fetch_add(1);
                return;
            }
            auto& slot = relayQueue_[std::size_t(size1 > 0 ? start1 : start2)];
            slot.size = std::uint8_t(m.size());
            for (std::size_t i = 0; i < m.size(); ++i) slot.bytes[i] = m[i];
            relayFifo_.finishedWrite(1);
        });
    });
}

MotifXsProcessor::~MotifXsProcessor() { stopTimer(); }

void MotifXsProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    // Audio passes through untouched. This plugin generates none, but it may
    // well sit on the track the rack's audio returns on -- clearing the buffer
    // there would silently mute the instrument.
    juce::ScopedNoDenormals noDenormals;
    midi.clear();

    // Tap a mono sum for the scope. Cheap, and never allocates or locks.
    const int inCh = getTotalNumInputChannels();
    inputChannels_.store(inCh);
    if (const int numSamples = buffer.getNumSamples(); numSamples > 0 && inCh > 0) {
        const int channels = juce::jmin(inCh, buffer.getNumChannels());
        int w = scopeWrite_.load(std::memory_order_relaxed);
        float blockPeak = 0.0f;
        for (int i = 0; i < numSamples; ++i) {
            float sum = 0.0f;
            for (int ch = 0; ch < channels; ++ch) sum += buffer.getReadPointer(ch)[i];
            const float v = channels > 0 ? sum / float(channels) : 0.0f;
            scopeRing_[std::size_t(w)] = v;
            blockPeak = juce::jmax(blockPeak, std::abs(v));
            w = (w + 1) & (kScopeSize - 1);
        }
        scopeWrite_.store(w, std::memory_order_release);
        if (blockPeak > 1.0e-5f) everSawSignal_.store(true);
    }

    // Relay whatever the rack's arpeggiator sent since the last block. These
    // arrive asynchronously, so they are stamped at the start of the block --
    // up to one buffer of jitter, which is fine for capture but is not
    // sample-accurate, and worth knowing before quantising hard against it.
    if (relayOn_.load()) {
        int start1, size1, start2, size2;
        relayFifo_.prepareToRead(kRelayCapacity, start1, size1, start2, size2);
        const auto emit = [&](int from, int count) {
            for (int i = 0; i < count; ++i) {
                const auto& s = relayQueue_[std::size_t(from + i)];
                if (s.size >= 1)
                    midi.addEvent(juce::MidiMessage(s.bytes, int(s.size)), 0);
            }
        };
        emit(start1, size1);
        emit(start2, size2);
        relayFifo_.finishedRead(size1 + size2);
    }

    // Detect automation movement without allocating or sending. The worker
    // thread does the actual MIDI; a host sweeping a parameter at block rate
    // must not produce one SysEx per block.
    bool changed = false;
    for (int i = 0; i < kNumMacros; ++i) {
        if (auto* p = apvts_.getRawParameterValue(macros::kAll[std::size_t(i)].id)) {
            const float v = p->load();
            if (lastPushed_[std::size_t(i)].exchange(v) != v) changed = true;
        }
    }
    if (changed) automationDirty_.store(true);

    // Auto-capture must not run against a rolling transport.
    if (auto* ph = getPlayHead()) {
        if (const auto pos = ph->getPosition())
            transportPlaying_.store(pos->getIsPlaying() || pos->getIsRecording());
    }
}

bool MotifXsProcessor::stateIsStale() const {
    return worker_->changeCount() != capturedAtChange_;
}

/// Captures once the user has stopped fiddling and the transport is idle.
///
/// Two conditions, both necessary. Capturing while the transport rolls would
/// put five seconds of bulk traffic against the notes being played. Capturing
/// when nothing changed would be pointless traffic -- so the worker counts
/// changes *to* the rack, and reads never bump it.
void MotifXsProcessor::maybeAutoCapture() {
    if (capturing_.load() || restoring_.load() || transportPlaying_.load() || !worker_->isOpen())
        return;

    const auto changes = worker_->changeCount();
    const auto now = juce::Time::currentTimeMillis();
    if (changes != lastSeenChange_) {
        lastSeenChange_ = changes;
        lastChangeMs_ = now;
        return;                                   // still moving; wait for quiet
    }
    if (changes == capturedAtChange_) return;     // nothing new since last capture
    if (now - lastChangeMs_ < kAutoCaptureQuietMs) return;

    capturing_.store(true);
    const auto at = changes;
    captureNow([this, at](bool ok, juce::String) {
        if (ok) capturedAtChange_ = at;
        capturing_.store(false);
    });
}

void MotifXsProcessor::pushAutomationToDevice() {
    if (!automationDirty_.exchange(false)) return;
    const int part = int(apvts_.getRawParameterValue("part")->load()) - 1;
    for (int i = 0; i < kNumMacros; ++i) {
        const auto* p = findParameterById(macros::kAll[std::size_t(i)].parameter);
        if (!p) continue;
        if (auto* raw = apvts_.getRawParameterValue(macros::kAll[std::size_t(i)].id))
            worker_->setParameter(*p, juce::jlimit(0, 15, part), int(raw->load()));
    }
}

int MotifXsProcessor::readRecent(float* dest, int count) {
    if (count <= 0) return 0;
    const int n = juce::jmin(count, kScopeSize);
    const int w = scopeWrite_.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        dest[i] = scopeRing_[std::size_t((w - n + i + kScopeSize) & (kScopeSize - 1))];
    // pad the front if fewer were asked for than the caller's buffer
    for (int i = n; i < count; ++i) dest[i] = 0.0f;
    return n;
}

juce::String MotifXsProcessor::tapStatus() {
    if (inputChannels_.load() <= 0)
        return "This track sends no audio to the plugin.\n"
               "Put it after an External Instrument device, or on the audio "
               "track the rack returns on.";
    if (!everSawSignal_.load())
        return "Audio bus connected, but nothing has come through it yet.";
    return {};
}

juce::AudioProcessorEditor* MotifXsProcessor::createEditor() {
    return new MotifXsEditor(*this);
}

void MotifXsProcessor::captureNow(std::function<void(bool, juce::String)> done) {
    worker_->post([this, done](Device& d) {
        std::string report;
        auto st = captureState(d, {}, std::chrono::milliseconds{700},
                               CaptureScope::WithVoices, &report);
        writeDiagnostic("last-capture-report.txt", report);
        if (!st) {
            if (done)
                done(false, "the rack did not send a complete Multi - project state NOT updated");
            return;
        }
        {
            const juce::ScopedLock lock(stateLock_);
            captured_ = std::move(*st);
        }
        if (done) done(true, juce::String(stateSummary()));
    });
}

void MotifXsProcessor::openDevice() {
    worker_->open({}, [this](bool ok, std::string err, DeviceInfo info) {
        const juce::ScopedLock lock(stateLock_);
        if (ok) {
            portName_ = juce::String(info.portName);
            deviceError_.clear();
        } else {
            deviceError_ = juce::String(err);
        }
    }, "the Motif Rack XS plugin");
}

void MotifXsProcessor::retryOpenIfBlocked() {
    if (worker_->isOpen()) return;
    {
        const juce::ScopedLock lock(stateLock_);
        if (deviceError_.isEmpty()) return;
    }
    // The timer runs at 30 Hz; every couple of seconds is often enough to feel
    // immediate without hammering CoreMIDI while a DAW is loading.
    if (++retryTicks_ < 60) return;
    retryTicks_ = 0;
    openDevice();
}

juce::String MotifXsProcessor::deviceError() const {
    const juce::ScopedLock lock(stateLock_);
    return deviceError_;
}

juce::String MotifXsProcessor::restoreStatus() const {
    const juce::ScopedLock lock(stateLock_);
    return restoreStatus_;
}

juce::String MotifXsProcessor::stateSummary() const {
    const juce::ScopedLock lock(stateLock_);
    return captured_.empty() ? juce::String("nothing captured yet")
                             : juce::String(captured_.summary());
}

void MotifXsProcessor::getStateInformation(juce::MemoryBlock& dest) {
    auto tree = apvts_.copyState();
    // Store the captured Multi alongside the automation values. Addresses and
    // raw bytes, never UI state: the address map is stable across firmware, a
    // UI layout is not.
    {
        const juce::ScopedLock lock(stateLock_);
        if (!captured_.empty())
            tree.setProperty("multi", juce::String(toText(captured_)), nullptr);
        tree.setProperty("port", portName_, nullptr);
    }
    if (auto xml = tree.createXml()) copyXmlToBinary(*xml, dest);

    {
        const juce::ScopedLock lock(stateLock_);
        if (!captured_.empty()) writeDiagnostic("last-saved.motifxs", toText(captured_));
    }
}

void MotifXsProcessor::setStateInformation(const void* data, int size) {
    auto xml = getXmlFromBinary(data, size);
    if (!xml) return;
    auto tree = juce::ValueTree::fromXml(*xml);
    if (!tree.isValid()) return;

    const auto multi = tree.getProperty("multi").toString();
    writeDiagnostic("last-restored.motifxs", multi.isEmpty() ? "(the project carried no rack state)"
                                                             : multi);
    tree.removeProperty("multi", nullptr);
    tree.removeProperty("port", nullptr);
    apvts_.replaceState(tree);

    if (multi.isEmpty()) return;
    auto st = fromText(multi.toStdString());
    if (!st) return;

    {
        const juce::ScopedLock lock(stateLock_);
        captured_ = *st;
    }
    restoring_.store(true);
    {
        const juce::ScopedLock lock(stateLock_);
        restoreStatus_ = "restoring the rack...";
        capturedAtChange_ = worker_->changeCount();   // a restore is not an edit
    }

    // Never send MIDI from setStateInformation: the host may call it before the
    // device is open, and on its own thread. Hand it to the worker, which runs
    // this after the open job queued in the constructor.
    worker_->post([this, captured = *st](Device& d) {
        if (!d.isOpen()) {
            // Silently doing nothing here is how a project could open with the
            // rack on entirely the wrong sounds and no indication why.
            restoring_.store(false);
            const juce::ScopedLock lock(stateLock_);
            restoreStatus_ = "no rack found - the project's sounds were not restored";
            return;
        }
        const bool ok = restoreState(d, captured);
        restoring_.store(false);
        const juce::ScopedLock lock(stateLock_);
        restoreStatus_ = ok ? juce::String("restored from the project")
                            : juce::String("restore failed");
        // The restore itself moved the rack; that is not an edit to capture.
        capturedAtChange_ = worker_->changeCount();
    });
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new MotifXsProcessor(); }
