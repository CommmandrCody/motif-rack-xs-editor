// Plugin tests. No DAW, no hardware.
//
// Every group here is banked from a failure that actually happened, and the
// comment above each says which one. A test that cannot name the bug it
// prevents is usually testing that the code is the code.
//
// Method note, borrowed from the Deluge companion suite: where exercising the
// real path would need a rack plugged in, the assertion is made against the
// SOURCE instead. That pins the exact shape of the bug, which is what
// regressed, and keeps the suite from depending on what is connected. Tests
// that genuinely need hardware live at the bottom and say out loud when they
// are skipped -- a missing rack must LOOK missing.

#include <juce_audio_processors/juce_audio_processors.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "Macros.h"
#include "motifxs/parameters.hpp"
#include "motifxs/state.hpp"
#include "motifxs/sysex.hpp"

using namespace motifxs;

namespace {

int gFailures = 0;
int gChecks = 0;

void check(bool ok, const std::string& what) {
    ++gChecks;
    if (!ok) {
        std::printf("  FAIL  %s\n", what.c_str());
        ++gFailures;
    }
}

void group(const char* name) { std::printf("\n%s\n", name); }

std::string readSource(const std::string& relative) {
    std::ifstream in(std::string(MOTIFXS_SOURCE_DIR) + "/" + relative);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// Strips // and /* */ comments, so a rule is not satisfied by the sentence
/// describing it.
std::string withoutComments(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n') ++i;
            out += '\n';
        } else if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) ++i;
            ++i;
        } else {
            out += s[i];
        }
    }
    return out;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------- fixtures

/// A structurally complete Multi: the header, all sixteen part blocks and the
/// footer. Enough to satisfy State::problem(), which is what decides whether a
/// capture may be saved or restored.
State validCapture() {
    State s;
    s.deviceNumber = 0;
    s.capturedAt = "2026-01-01T00:00:00Z";
    const std::vector<std::uint8_t> empty;
    s.messages.push_back(bulkDump(0, {0x0E, 0x5F, 0x00}, empty));
    for (int part = 0; part < kParts; ++part) {
        const std::vector<std::uint8_t> partData(20, 0x40);
        s.messages.push_back(bulkDump(0, {0x37, std::uint8_t(part), 0x00}, partData));
    }
    s.messages.push_back(bulkDump(0, {0x0F, 0x5F, 0x00}, empty));
    return s;
}

/// What the old capture bug produced: framing with the contents missing. This
/// is the shape of the file that sat in a Live template for weeks.
State truncatedCapture() {
    State s;
    s.deviceNumber = 0;
    s.capturedAt = "2026-01-01T00:00:00Z";
    const std::vector<std::uint8_t> empty;
    s.messages.push_back(bulkDump(0, {0x0E, 0x5F, 0x00}, empty));
    const std::vector<std::uint8_t> crumb{0x01, 0x49, 0x40, 0x40};
    s.messages.push_back(bulkDump(0, {0x40, 0x07, 0x00}, crumb));
    s.messages.push_back(bulkDump(0, {0x0F, 0x5F, 0x00}, empty));
    return s;
}

/// Builds the plugin's own state blob with a given capture inside it, the way
/// a saved project carries one.
juce::MemoryBlock projectStateCarrying(const State& s) {
    juce::ValueTree tree("motifxs");
    tree.setProperty("multi", juce::String(toText(s)), nullptr);
    juce::MemoryBlock block;
    if (auto xml = tree.createXml()) juce::AudioProcessor::copyXmlToBinary(*xml, block);
    return block;
}

bool blobMentionsMulti(const juce::MemoryBlock& block) {
    if (auto xml = juce::AudioProcessor::getXmlFromBinary(block.getData(), int(block.getSize())))
        return juce::ValueTree::fromXml(*xml).hasProperty("multi");
    return false;
}

}  // namespace

int main() {
    juce::ScopedJuceInitialiser_GUI juceInit;

    // ---------------------------------------------------------------------
    group("a capture has to be restorable to count as one");
    // Banked from: a half-arrived capture reported success, was written into
    // the project, replaced the last good state, and only failed weeks later
    // when the session was reopened and the rack showed "illegal bulk data".
    {
        check(validCapture().problem().empty(),
              "a complete Multi is accepted");
        check(!truncatedCapture().problem().empty(),
              "a Multi with no part blocks is refused");
        check(contains(truncatedCapture().problem(), "part"),
              "the refusal names what is missing rather than failing mutely");
        check(State{}.problem() == "nothing was captured",
              "an empty state is refused");
    }

    // ---------------------------------------------------------------------
    group("a bad state in a project is ignored, not adopted");
    // Banked from: the worst bug in the project. A project carrying a broken
    // capture had it adopted as the current state, so the next save wrote it
    // straight back out. It outlived every fix, and a Live TEMPLATE carrying
    // one reseeded it into every new set made from that template.
    {
        auto processor = std::unique_ptr<juce::AudioProcessor>(createPluginFilter());
        const auto bad = projectStateCarrying(truncatedCapture());
        processor->setStateInformation(bad.getData(), int(bad.getSize()));

        juce::MemoryBlock saved;
        processor->getStateInformation(saved);
        check(!blobMentionsMulti(saved),
              "a broken capture loaded from a project is NOT written back out");
    }
    {
        auto processor = std::unique_ptr<juce::AudioProcessor>(createPluginFilter());
        const auto good = projectStateCarrying(validCapture());
        processor->setStateInformation(good.getData(), int(good.getSize()));

        juce::MemoryBlock saved;
        processor->getStateInformation(saved);
        check(blobMentionsMulti(saved),
              "a complete capture survives the project round trip");
    }

    // ---------------------------------------------------------------------
    group("restoring refuses what it cannot put back");
    // Banked from: restore pushed incomplete bulk at the rack, which rejected
    // the stream and left it half applied -- twice destroying a working Multi.
    {
        Device offline;
        check(!restoreState(offline, truncatedCapture()),
              "an incomplete state is refused rather than sent");
        check(!restoreState(offline, State{}),
              "an empty state is refused");
    }

    // ---------------------------------------------------------------------
    group("the separation between core and UI holds");
    // The core must stay free of JUCE: it is what keeps the licensing bound to
    // the shells, lets the core be tested headless, and allows a second device
    // to reuse it. Enforced here rather than trusted.
    {
        for (const char* file : {"motif-xs-core/src/device.cpp",
                                 "motif-xs-core/src/state.cpp",
                                 "motif-xs-core/src/worker.cpp",
                                 "motif-xs-core/src/sysex.cpp",
                                 "motif-xs-core/src/racklock.cpp"}) {
            const auto src = withoutComments(readSource(file));
            check(!src.empty(), std::string("could read ") + file);
            check(!contains(src, "juce"),
                  std::string(file) + " is free of JUCE");
        }
    }

    // ---------------------------------------------------------------------
    group("the device is never opened twice");
    // Banked from: the bug that corrupted every capture made from the plugin.
    // Opening an already-open Device built a SECOND CoreMIDI client and
    // connected the source again while the first was live, so every packet
    // arrived twice and the copies spliced together inside the reassembler --
    // 209-byte messages where the largest real block is 107. The rack lock did
    // not catch it, because a lock already held by this process is granted
    // again. Asserted against the source: reproducing it needs a rack, and the
    // shape is what regressed.
    {
        const auto src = withoutComments(readSource("motif-xs-core/src/device.cpp"));
        check(contains(src, "if (isOpen())"),
              "Device::open returns early when it is already open");
        check(contains(src, "teardown()"),
              "every CoreMIDI object is released before another is made");
        const auto worker = withoutComments(readSource("motif-xs-core/src/worker.cpp"));
        check(contains(worker, "opening_"),
              "the worker refuses to queue a second open while one is in flight");
    }

    // ---------------------------------------------------------------------
    group("the arpeggiator relay cannot feed itself");
    // Banked from: on one track the relayed notes reached the rack again --
    // plugin MIDI out runs downstream into the External Instrument that feeds
    // the rack, which arpeggiates what it is sent. One key sustained itself
    // until PANIC.
    {
        const auto src = withoutComments(readSource("motif-xs-plugin/PluginProcessor.cpp"));
        check(contains(src, "wasJustSentByHost"),
              "notes the host just played are not relayed back out");
        check(contains(src, "feedbackTripped_"),
              "a runaway note-on rate switches the relay off");
        check(contains(src, "kRunawayNoteOnsPerSecond"),
              "the runaway threshold is a named constant, not a magic number");
    }

    // ---------------------------------------------------------------------
    group("every macro is bound to a real parameter");
    // Banked from: the UI knobs and the host parameters were two unconnected
    // paths over two duplicated lists, so a Push-mapped cutoff moved the rack
    // while the editor showed the old value.
    {
        check(macros::kCount == 9, "nine macros, as the UI lays out");
        for (std::size_t i = 0; i < macros::kCount; ++i) {
            const auto& m = macros::kAll[i];
            check(findParameterById(m.parameter) != nullptr,
                  std::string("macro '") + m.id + "' resolves to parameter '" + m.parameter + "'");
            check(m.id != nullptr && m.label != nullptr && m.shortLabel != nullptr,
                  std::string("macro '") + m.id + "' is fully labelled");
        }
        const auto editor = withoutComments(readSource("motif-xs-plugin/PluginEditor.cpp"));
        check(contains(editor, "macros::kAll"),
              "the editor drives the shared macro list rather than its own copy");
        const auto processor = withoutComments(readSource("motif-xs-plugin/PluginProcessor.cpp"));
        check(contains(processor, "macros::kAll"),
              "the processor drives the same list");
    }

    // ---------------------------------------------------------------------
    group("the plugin presents itself correctly to a host");
    // Banked from: the plugin cleared the audio buffer, which silently muted
    // the instrument when it sat on the track the rack returns on.
    {
        auto processor = std::unique_ptr<juce::AudioProcessor>(createPluginFilter());
        check(processor != nullptr, "the processor is constructible headless");
        check(processor->producesMidi(),
              "it declares MIDI output, or the arp relay has nowhere to go");
        check(processor->acceptsMidi(), "it accepts MIDI");
        check(!processor->isMidiEffect(),
              "it is not a MIDI effect: it has to see the returning audio for the scope");

        const auto src = withoutComments(readSource("motif-xs-plugin/PluginProcessor.cpp"));
        check(!contains(src, "buffer.clear()"),
              "audio passes through untouched -- clearing it mutes the instrument");
    }

    // ---------------------------------------------------------------------
    group("audio passes through unchanged");
    {
        auto processor = std::unique_ptr<juce::AudioProcessor>(createPluginFilter());
        processor->prepareToPlay(48000.0, 512);
        juce::AudioBuffer<float> buffer(2, 512);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 512; ++i)
                buffer.getWritePointer(ch)[i] = 0.25f * float((i % 7) - 3);
        juce::AudioBuffer<float> before;
        before.makeCopyOf(buffer);

        juce::MidiBuffer midi;
        processor->processBlock(buffer, midi);

        bool identical = true;
        for (int ch = 0; ch < 2 && identical; ++ch)
            for (int i = 0; i < 512; ++i)
                if (buffer.getReadPointer(ch)[i] != before.getReadPointer(ch)[i]) {
                    identical = false;
                    break;
                }
        check(identical, "the audio buffer comes out exactly as it went in");
        processor->releaseResources();
    }

    // ---------------------------------------------------------------------
    group("the editor survives being opened and closed");
    // Banked from: pluginval segfaulted in its Editor test at strictness 7.
    // The editor queued work on timers, on the message thread and on the device
    // worker, all capturing a raw `this`. In the standalone app that is
    // harmless -- the window lives as long as the process -- but a plugin
    // editor is destroyed every time its window closes, and the callbacks
    // outlived it.
    {
        const auto src = withoutComments(readSource("motif-xs-app/MainComponent.cpp"));
        check(!contains(src, "juce::MessageManager::callAsync"),
              "deferred message-thread work goes through onMessageThread, which checks "
              "the editor is still alive");
        check(!contains(src, "callAfterDelay(250, [this]") &&
              !contains(src, "callAfterDelay(300, [this]") &&
              !contains(src, "callAfterDelay(400, [this]") &&
              !contains(src, "callAfterDelay(700, [this"),
              "no timer captures a bare `this` and fires into a destroyed editor");
        check(contains(src, "alive_->store(false)"),
              "the destructor disarms queued work before anything else runs");

        const auto hdr = withoutComments(readSource("motif-xs-app/MainComponent.h"));
        check(contains(hdr, "std::shared_ptr<std::atomic<bool>> alive_"),
              "the liveness flag is shared, so a callback can outlive the object "
              "and still answer");

        // And the thing itself: open and close it repeatedly.
        auto processor = std::unique_ptr<juce::AudioProcessor>(createPluginFilter());
        for (int i = 0; i < 5; ++i) {
            std::unique_ptr<juce::AudioProcessorEditor> ed(processor->createEditorIfNeeded());
            check(ed != nullptr, "the editor is constructible");
            ed.reset();
            // Let any timer queued by the editor fire after it is gone --
            // which is precisely what used to crash.
            const auto until = juce::Time::getMillisecondCounter() + 60;
            while (juce::Time::getMillisecondCounter() < until)
                juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
        }
        check(true, "five open/close cycles with the message loop pumped between them");
    }

    // ---------------------------------------------------------------------
    group("hardware");
    // A missing rack must LOOK missing. These never pass by falling back to
    // something simulated -- they say they were skipped.
    {
        Device device;
        std::string error;
        if (!device.open({}, &error, "motif-xs plugin tests")) {
            std::printf("  SKIP  no rack: %s\n", error.c_str());
            std::printf("        (connect a MOTIF-RACK XS and close other clients "
                        "to run these)\n");
        } else {
            const auto id = device.identify();
            check(id.has_value(), "the rack identifies itself");
            auto shot = captureState(device);
            check(shot.has_value(), "a live capture completes");
            if (shot) {
                check(shot->problem().empty(), "the live capture is restorable");
                check(shot->messages.size() > 400,
                      "the capture is the expected size, not a fragment");
            }
            device.close();
        }
    }

    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
