#include "MainComponent.h"

using namespace motifxs;

namespace {
/// PERFORM macros. All are Multi Part parameters, so they act as offsets on
/// whatever voice the part holds -- the same thing the rack's own knobs do.
struct KnobSpec { const char* label; const char* id; };
constexpr KnobSpec kKnobs[] = {
    {"VOLUME",    "multi_part_volume"},
    {"PAN",       "multi_part_pan"},
    {"CUTOFF",    "multi_part_filter_cutoff_frequency"},
    {"RESO",      "multi_part_filter_resonance_width"},
    {"ATTACK",    "multi_part_aeg_attack_time"},
    {"DECAY",     "multi_part_aeg_decay_time"},
    {"RELEASE",   "multi_part_aeg_release_time"},
    {"REVERB",    "multi_part_reverb_send"},
    {"CHORUS",    "multi_part_chorus_send"},
};

const Parameter* param(const char* id) { return findParameterById(id); }
}  // namespace

/// Formats a raw device value the way the rack's own display would.
juce::String ParamKnob::format(int raw) const {
    const juce::String name = juce::String(std::string(param_ ? param_->name : ""));
    if (name == "Pan") {
        const int v = raw - 64;                    // 1..127, 64 = centre
        if (v == 0) return "C";
        return (v < 0 ? "L" : "R") + juce::String(std::abs(v));
    }
    if (bipolar_) return (raw - 64 > 0 ? "+" : "") + juce::String(raw - 64);
    return juce::String(raw);
}

MainComponent::MainComponent() {
    setLookAndFeel(&look_);

    addAndMakeVisible(portBox_);
    refreshPorts();

    connectButton_.onClick = [this] { connect(); };
    addAndMakeVisible(connectButton_);

    // A held arpeggio keeps playing after the key is released, so "stop
    // everything" has to be permanently within reach, not buried in a menu.
    panicButton_.setColour(juce::TextButton::buttonColourId, theme::bad.withAlpha(0.85f));
    panicButton_.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    panicButton_.onClick = [this] {
        worker_.post([](Device& d) { d.panic(); });
        arpSwitch_.setToggleState(false, juce::dontSendNotification);
        arpHold_.setToggleState(false, juce::dontSendNotification);
        setStatus("panic: all notes off, arpeggiators stopped on all 16 parts", theme::warn);
    };
    addAndMakeVisible(panicButton_);

    statusLabel_.setFont(juce::FontOptions(12.0f));
    statusLabel_.setColour(juce::Label::textColourId, theme::dim);
    statusLabel_.setText("not connected", juce::dontSendNotification);
    addAndMakeVisible(statusLabel_);

    deviceLabel_.setFont(juce::FontOptions(11.0f));
    deviceLabel_.setColour(juce::Label::textColourId, theme::dim);
    deviceLabel_.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(deviceLabel_);

    for (int i = 0; i < 16; ++i) {
        auto b = std::make_unique<juce::TextButton>(juce::String(i + 1));
        b->setClickingTogglesState(false);
        b->onClick = [this, i] { selectPart(i); };
        addAndMakeVisible(*b);
        partButtons_[size_t(i)] = std::move(b);
    }

    voices_.onPick = [this](const Voice& v) {
        // Bank Select + Program Change on the part's own receive channel.
        worker_.post([this, v](Device& d) { d.selectVoice(v.msb, v.lsb, v.program, std::uint8_t(part_)); });
        if (const auto* msb = param("multi_part_bank_select"))
            worker_.setParameter(*msb, part_, v.msb);
        if (const auto* lsb = param("multi_part_bank_select_lsb"))
            worker_.setParameter(*lsb, part_, v.lsb);
        if (const auto* pgm = param("multi_part_program_number"))
            worker_.setParameter(*pgm, part_, v.program);
        partVoiceNames_[size_t(part_)] = juce::String(std::string(v.name));
        dirty_ = true;
    };

    // Arm the rack's per-part ARP MIDI Out (38 pp 01). Without this the
    // arpeggio is audible but transmits nothing, so routing looks broken.
    arpMidiOut_.setClickingTogglesState(true);
    arpMidiOut_.setColour(juce::TextButton::buttonOnColourId, theme::good);
    arpMidiOut_.setColour(juce::TextButton::textColourOnId, juce::Colours::black);
    arpMidiOut_.setTooltip("Transmit this part's arpeggio as MIDI");
    arpMidiOut_.onClick = [this] {
        if (const auto* p = findParameter(Scope::MultiPart, 0x38, 0x00, 0x01))
            worker_.setParameter(*p, part_, arpMidiOut_.getToggleState() ? 1 : 0);
    };
    addAndMakeVisible(arpMidiOut_);

    thruLabel_.setText("THRU", juce::dontSendNotification);
    thruLabel_.setFont(juce::FontOptions(11.0f));
    thruLabel_.setColour(juce::Label::textColourId, theme::dim);
    thruLabel_.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(thruLabel_);

    refreshThruDestinations();
    thruBox_.onChange = [this] {
        const std::string dest =
            thruBox_.getSelectedId() <= 1 ? std::string() : thruBox_.getText().toStdString();
        worker_.post([dest](Device& d) {
            d.silenceThru();             // release notes held on the old target
            d.setThru(dest);
        });
        setStatus(dest.empty() ? "thru off"
                               : "forwarding the rack's notes to " + juce::String(dest),
                  dest.empty() ? theme::dim : theme::good);
    };
    addAndMakeVisible(thruBox_);

    arpNameLabel_.setFont(juce::FontOptions(12.0f));
    arpNameLabel_.setColour(juce::Label::textColourId, theme::dim);
    addAndMakeVisible(arpNameLabel_);

    arps_.onPick = [this](const Arpeggio& a) {
        const int slot = juce::jmax(1, arpSlot_.getSelectedId());
        // ARP SF1..SF5 Assign Type: 38 pp 38/3A/3C/3E/40
        const std::uint8_t low = std::uint8_t(0x38 + (slot - 1) * 2);
        if (const auto* p = findParameter(Scope::MultiPart, 0x38, 0x00, low))
            worker_.setParameter(*p, part_, a.number);
        arpNameLabel_.setText(juce::String(std::string(a.name)), juce::dontSendNotification);
        setStatus("SF" + juce::String(slot) + ": " + juce::String(std::string(a.name)), theme::good);
    };

    tabs_.setOutline(0);
    tabs_.setTabBarDepth(28);
    tabs_.addTab("VOICE", theme::bg, &voices_, false);
    tabs_.addTab("ARPEGGIO", theme::bg, &arps_, false);
    tabs_.setColour(juce::TabbedComponent::outlineColourId, juce::Colours::transparentBlack);
    addAndMakeVisible(tabs_);

    arpSlot_.addItem("SF1", 1); arpSlot_.addItem("SF2", 2); arpSlot_.addItem("SF3", 3);
    arpSlot_.addItem("SF4", 4); arpSlot_.addItem("SF5", 5);
    arpSlot_.setSelectedId(1, juce::dontSendNotification);
    arpSlot_.onChange = [this] { pullPartState(); };
    addAndMakeVisible(arpSlot_);

    arpSwitch_.setClickingTogglesState(true);
    arpSwitch_.setColour(juce::TextButton::buttonOnColourId, theme::warn);
    arpSwitch_.setColour(juce::TextButton::textColourOnId, juce::Colours::black);
    arpSwitch_.onClick = [this] {
        const bool on = arpSwitch_.getToggleState();
        if (const auto* p = findParameter(Scope::MultiPart, 0x38, 0x00, 0x00))
            worker_.setParameter(*p, part_, on ? 1 : 0);
        updateArpWarning();
    };
    addAndMakeVisible(arpSwitch_);

    arpHold_.setClickingTogglesState(true);
    // Hold is the one that latches: the phrase keeps going after key release.
    arpHold_.setColour(juce::TextButton::buttonOnColourId, theme::bad);
    arpHold_.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    arpHold_.onClick = [this] {
        // ARP Hold: 0 sync-off, 1 off, 2 on
        if (const auto* p = findParameter(Scope::MultiPart, 0x38, 0x00, 0x07))
            worker_.setParameter(*p, part_, arpHold_.getToggleState() ? 2 : 1);
        updateArpWarning();
    };
    addAndMakeVisible(arpHold_);

    for (size_t i = 0; i < knobs_.size(); ++i) {
        auto k = std::make_unique<ParamKnob>(kKnobs[i].label, kKnobs[i].id);
        auto* raw = k.get();
        raw->slider().onValueChange = [this, raw] { pushKnob(*raw); };
        addAndMakeVisible(*k);
        knobs_[i] = std::move(k);
    }

    selectPart(0);
    setSize(1180, 760);
    startTimerHz(20);

    // One rack, one port -- make the app useful on launch instead of making
    // the user find the Connect button first.
    if (portBox_.getSelectedId() > 0)
        juce::Timer::callAfterDelay(250, [this] { connect(); });
}

MainComponent::~MainComponent() {
    stopTimer();
    setLookAndFeel(nullptr);
}

/// Everything except the rack's own ports -- forwarding the Motif to itself
/// would be a feedback loop.
void MainComponent::refreshThruDestinations() {
    thruBox_.clear(juce::dontSendNotification);
    thruBox_.addItem("no thru", 1);
    int id = 2;
    for (const auto& e : listDestinations()) {
        if (e.name.find("MOTIF") != std::string::npos) continue;
        thruBox_.addItem(e.name, id++);
    }
    thruBox_.setSelectedId(1, juce::dontSendNotification);
}

void MainComponent::refreshPorts() {
    portBox_.clear(juce::dontSendNotification);
    int id = 1, preferred = 0;
    for (const auto& e : listDestinations()) {
        portBox_.addItem(e.name, id);
        // Only Port1 carries SysEx; pick it by default.
        if (e.name.find("MOTIF") != std::string::npos &&
            e.name.find("Port1") != std::string::npos)
            preferred = id;
        ++id;
    }
    if (preferred) portBox_.setSelectedId(preferred, juce::dontSendNotification);
    else if (portBox_.getNumItems() > 0) portBox_.setSelectedItemIndex(0, juce::dontSendNotification);
}

void MainComponent::setStatus(const juce::String& text, juce::Colour colour) {
    pendingStatus_ = text;
    pendingStatusColour_ = colour;
    dirty_ = true;
}

void MainComponent::connect() {
    const auto name = portBox_.getText().toStdString();
    setStatus("connecting...", theme::dim);
    worker_.open(name, [this](bool ok, std::string err, DeviceInfo info) {
        if (!ok) {
            setStatus(juce::String(err), theme::bad);
            return;
        }
        setStatus("connected", theme::good);
        juce::MessageManager::callAsync([this, info] {
            deviceLabel_.setText("device " + juce::String(info.deviceNumber) +
                                     "   firmware " + juce::String(info.firmwareVersion, 1),
                                 juce::dontSendNotification);
        });
        // Warn if the rack is set to ignore patch selection -- it fails silently.
        if (const auto* bank = findParameter(Scope::System, 0x00, 0x00, 0x14))
            worker_.readParameter(*bank, 0, [this](std::optional<std::int32_t> v) {
                if (v && *v == 0)
                    setStatus("Bank Select receive is OFF - patch changes will be ignored",
                              theme::warn);
            });
        pullPartState();
    });
}

void MainComponent::selectPart(int part) {
    part_ = juce::jlimit(0, 15, part);
    for (int i = 0; i < 16; ++i) {
        auto& b = *partButtons_[size_t(i)];
        b.setColour(juce::TextButton::buttonColourId,
                    i == part_ ? theme::accent : theme::panelHi);
        b.setColour(juce::TextButton::textColourOffId,
                    i == part_ ? juce::Colours::black : theme::text);
    }
    pullPartState();
    repaint();
}

/// Reads the selected part's state back from the rack so the UI reflects the
/// hardware rather than assuming.
void MainComponent::pullPartState() {
    if (!worker_.isOpen()) return;
    const int part = part_;

    for (auto& k : knobs_) {
        if (!k || !k->parameter()) continue;
        auto* knob = k.get();
        worker_.readParameter(*knob->parameter(), part,
                              [this, knob](std::optional<std::int32_t> v) {
                                  if (!v) return;
                                  const int raw = *v;
                                  juce::MessageManager::callAsync(
                                      [knob, raw] { knob->setRaw(raw); });
                              });
    }

    // voice assignment
    const auto* msb = param("multi_part_bank_select");
    const auto* lsb = param("multi_part_bank_select_lsb");
    const auto* pgm = param("multi_part_program_number");
    if (msb && lsb && pgm) {
        worker_.post([this, part, msb, lsb, pgm](Device& d) {
            const auto a = d.readParameter(*msb, part);
            const auto b = d.readParameter(*lsb, part);
            const auto c = d.readParameter(*pgm, part);
            if (!a || !b || !c) return;
            const auto* v = findVoice(std::uint8_t(*a), std::uint8_t(*b), std::uint8_t(*c));
            const juce::String name = v ? juce::String(std::string(v->name)) : "(unmapped)";
            const int ma = *a, mb = *b, mc = *c;
            juce::MessageManager::callAsync([this, part, name, ma, mb, mc] {
                partVoiceNames_[size_t(part)] = name;
                if (part == part_) voices_.selectByProgram(ma, mb, mc);
                repaint();
            });
        });
    }

    // arp switch, hold and the selected slot's assigned type
    if (const auto* sw = findParameter(Scope::MultiPart, 0x38, 0x00, 0x00))
        worker_.readParameter(*sw, part, [this](std::optional<std::int32_t> v) {
            if (!v) return;
            const bool on = *v != 0;
            juce::MessageManager::callAsync([this, on] {
                arpSwitch_.setToggleState(on, juce::dontSendNotification);
                updateArpWarning();
            });
        });
    if (const auto* out = findParameter(Scope::MultiPart, 0x38, 0x00, 0x01))
        worker_.readParameter(*out, part, [this](std::optional<std::int32_t> v) {
            if (!v) return;
            const bool on = *v != 0;
            juce::MessageManager::callAsync([this, on] {
                arpMidiOut_.setToggleState(on, juce::dontSendNotification);
            });
        });
    if (const auto* hold = findParameter(Scope::MultiPart, 0x38, 0x00, 0x07))
        worker_.readParameter(*hold, part, [this](std::optional<std::int32_t> v) {
            if (!v) return;
            const bool on = *v == 2;
            juce::MessageManager::callAsync([this, on] {
                arpHold_.setToggleState(on, juce::dontSendNotification);
                updateArpWarning();
            });
        });

    const int slot = juce::jmax(1, arpSlot_.getSelectedId());
    const std::uint8_t low = std::uint8_t(0x38 + (slot - 1) * 2);
    if (const auto* assign = findParameter(Scope::MultiPart, 0x38, 0x00, low))
        worker_.readParameter(*assign, part, [this](std::optional<std::int32_t> v) {
            if (!v) return;
            const int number = *v;
            juce::MessageManager::callAsync([this, number] {
                arps_.selectByNumber(number);
                const auto* meta = findArpeggio(number);
                arpNameLabel_.setText(number == 0 ? juce::String("(no arp)")
                                                  : (meta ? juce::String(std::string(meta->name))
                                                          : juce::String(number)),
                                      juce::dontSendNotification);
            });
        });
}

/// Says plainly what a keypress will do, because an armed arpeggio means the
/// voice you just picked is not what you will hear.
void MainComponent::updateArpWarning() {
    const bool on = arpSwitch_.getToggleState();
    const bool hold = arpHold_.getToggleState();
    if (!on) {
        setStatus(worker_.isOpen() ? "connected" : "not connected",
                  worker_.isOpen() ? theme::good : theme::dim);
        return;
    }
    setStatus(hold ? "ARP armed + HOLD on part " + juce::String(part_ + 1) +
                         " - a note starts a pattern that keeps playing"
                   : "ARP armed on part " + juce::String(part_ + 1) +
                         " - playing triggers a pattern, not the voice",
              hold ? theme::bad : theme::warn);
}

void MainComponent::pushKnob(ParamKnob& k) {
    if (!k.parameter() || !worker_.isOpen()) return;
    worker_.setParameter(*k.parameter(), part_, k.raw());
}

void MainComponent::timerCallback() {
    if (!dirty_.exchange(false)) return;
    if (pendingStatus_.isNotEmpty()) {
        statusLabel_.setText(pendingStatus_, juce::dontSendNotification);
        statusLabel_.setColour(juce::Label::textColourId, pendingStatusColour_);
    }
    repaint();
}

void MainComponent::paint(juce::Graphics& g) {
    g.fillAll(theme::bg);

    auto r = getLocalBounds();
    r.removeFromTop(44);

    // part strip backdrop
    auto strip = r.removeFromTop(46).reduced(10, 4);
    g.setColour(theme::panel);
    g.fillRoundedRectangle(strip.toFloat(), 5.0f);

    // selected part's voice name, the thing the eye should land on
    auto header = r.removeFromTop(34).reduced(12, 2);
    header.removeFromRight(470);   // room for the arp controls
    g.setColour(theme::dim);
    g.setFont(juce::FontOptions(11.0f));
    g.drawText("PART " + juce::String(part_ + 1), header.removeFromLeft(60),
               juce::Justification::centredLeft);
    g.setColour(theme::text);
    g.setFont(juce::FontOptions(19.0f, juce::Font::bold));
    g.drawText(partVoiceNames_[size_t(part_)].isEmpty() ? juce::String("--")
                                                        : partVoiceNames_[size_t(part_)],
               header, juce::Justification::centredLeft, true);

    // knob row backdrop
    auto knobRow = r.removeFromBottom(112).reduced(10, 4);
    g.setColour(theme::panel);
    g.fillRoundedRectangle(knobRow.toFloat(), 5.0f);
}

void MainComponent::resized() {
    auto r = getLocalBounds();

    auto top = r.removeFromTop(44).reduced(10, 8);
    portBox_.setBounds(top.removeFromLeft(280));
    top.removeFromLeft(6);
    connectButton_.setBounds(top.removeFromLeft(90));
    top.removeFromLeft(10);
    panicButton_.setBounds(top.removeFromRight(80));
    top.removeFromRight(8);
    deviceLabel_.setBounds(top.removeFromRight(170));
    top.removeFromRight(8);
    thruBox_.setBounds(top.removeFromRight(190));
    thruLabel_.setBounds(top.removeFromRight(38));
    top.removeFromRight(8);
    statusLabel_.setBounds(top);

    auto strip = r.removeFromTop(46).reduced(14, 8);
    const int bw = strip.getWidth() / 16;
    for (int i = 0; i < 16; ++i)
        partButtons_[size_t(i)]->setBounds(strip.removeFromLeft(bw).reduced(2));

    // voice-name header row: painted text on the left, arp controls on the right
    auto nameRow = r.removeFromTop(34).reduced(12, 4);
    arpMidiOut_.setBounds(nameRow.removeFromRight(48).reduced(0, 1));
    nameRow.removeFromRight(4);
    arpHold_.setBounds(nameRow.removeFromRight(56).reduced(0, 1));
    nameRow.removeFromRight(4);
    arpSwitch_.setBounds(nameRow.removeFromRight(52).reduced(0, 1));
    nameRow.removeFromRight(4);
    arpSlot_.setBounds(nameRow.removeFromRight(68).reduced(0, 1));
    nameRow.removeFromRight(8);
    arpNameLabel_.setBounds(nameRow.removeFromRight(210));

    auto knobRow = r.removeFromBottom(112).reduced(14, 8);
    const int kw = knobRow.getWidth() / int(knobs_.size());
    for (auto& k : knobs_) k->setBounds(knobRow.removeFromLeft(kw).reduced(4));

    tabs_.setBounds(r.reduced(10, 4));
}
