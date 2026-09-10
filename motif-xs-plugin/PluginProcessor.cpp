#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace motifxs;

namespace {

/// The automatable set is deliberately small. The rack exposes 918 usable
/// parameters; publishing all of them makes a host's automation list unusable
/// and Live's device view unreadable. These are the PERFORM macros -- the same
/// controls the rack's own knobs drive -- for the part the plugin is pointed
/// at. Everything deeper stays plugin state, editable in the UI but not
/// automatable, which matches how the hardware behaves.
struct Macro {
    const char* id;
    const char* label;
    const char* parameterId;   // motifxs parameter id
};

constexpr Macro kMacros[] = {
    {"volume",  "Volume",      "multi_part_volume"},
    {"pan",     "Pan",         "multi_part_pan"},
    {"cutoff",  "Cutoff",      "multi_part_filter_cutoff_frequency"},
    {"reso",    "Resonance",   "multi_part_filter_resonance_width"},
    {"attack",  "Attack",      "multi_part_aeg_attack_time"},
    {"decay",   "Decay",       "multi_part_aeg_decay_time"},
    {"release", "Release",     "multi_part_aeg_release_time"},
    {"reverb",  "Reverb Send", "multi_part_reverb_send"},
    {"chorus",  "Chorus Send", "multi_part_chorus_send"},
};

constexpr int kNumMacros = int(std::size(kMacros));

}  // namespace

juce::AudioProcessorValueTreeState::ParameterLayout MotifXsProcessor::makeLayout() {
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // Which part the macros address. Automatable so a track can switch parts,
    // though in practice one instance per part is the sane arrangement.
    layout.add(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"part", 1}, "Part", 1, 16, 1));

    for (const auto& m : kMacros) {
        const auto* p = findParameterById(m.parameterId);
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
        worker_->open({}, [this](bool ok, std::string, DeviceInfo info) {
            if (ok) {
                const juce::ScopedLock lock(stateLock_);
                portName_ = juce::String(info.portName);
            }
        });
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
    // No audio, no host MIDI. Clear rather than pass through: a controller
    // that leaks its input to the track output surprises people.
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();
    midi.clear();

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
        if (auto* p = apvts_.getRawParameterValue(kMacros[std::size_t(i)].id)) {
            const float v = p->load();
            if (lastPushed_[std::size_t(i)].exchange(v) != v) changed = true;
        }
    }
    if (changed) automationDirty_.store(true);
}

void MotifXsProcessor::pushAutomationToDevice() {
    if (!automationDirty_.exchange(false)) return;
    const int part = int(apvts_.getRawParameterValue("part")->load()) - 1;
    for (int i = 0; i < kNumMacros; ++i) {
        const auto* p = findParameterById(kMacros[std::size_t(i)].parameterId);
        if (!p) continue;
        if (auto* raw = apvts_.getRawParameterValue(kMacros[std::size_t(i)].id))
            worker_->setParameter(*p, juce::jlimit(0, 15, part), int(raw->load()));
    }
}

juce::AudioProcessorEditor* MotifXsProcessor::createEditor() {
    return new MotifXsEditor(*this);
}

void MotifXsProcessor::captureNow(std::function<void(bool, juce::String)> done) {
    worker_->post([this, done](Device& d) {
        auto st = captureState(d);
        if (!st) {
            if (done) done(false, "the rack sent no bulk data - is it in Multi mode?");
            return;
        }
        {
            const juce::ScopedLock lock(stateLock_);
            captured_ = std::move(*st);
        }
        if (done) done(true, juce::String(stateSummary()));
    });
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
}

void MotifXsProcessor::setStateInformation(const void* data, int size) {
    auto xml = getXmlFromBinary(data, size);
    if (!xml) return;
    auto tree = juce::ValueTree::fromXml(*xml);
    if (!tree.isValid()) return;

    const auto multi = tree.getProperty("multi").toString();
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
    // Never send MIDI from setStateInformation: the host may call it before
    // the device is open, and on its own thread. Hand it to the worker.
    worker_->post([captured = *st](Device& d) {
        if (d.isOpen()) restoreState(d, captured);
    });
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new MotifXsProcessor(); }
