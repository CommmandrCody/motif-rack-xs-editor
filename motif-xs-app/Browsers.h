#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "motifxs/catalog.hpp"
#include "Theme.h"

/// A search field that hands the arrow and return keys to the list below it,
/// so you can type a few letters and then walk the results without reaching
/// for the mouse.
class SearchBox : public juce::TextEditor {
public:
    std::function<bool(const juce::KeyPress&)> onNavigationKey;

    bool keyPressed(const juce::KeyPress& key) override {
        if (onNavigationKey &&
            (key.isKeyCode(juce::KeyPress::downKey) || key.isKeyCode(juce::KeyPress::upKey) ||
             key.isKeyCode(juce::KeyPress::returnKey)))
            if (onNavigationKey(key)) return true;
        return juce::TextEditor::keyPressed(key);
    }
};

/// Searchable list of the 1217 factory voices, grouped into collapsible
/// categories. 1217 flat rows is a lot to scroll; collapsed categories turn it
/// into about twenty. Typing expands whatever matches.
class VoiceBrowser : public juce::Component,
                     private juce::ListBoxModel,
                     private juce::TextEditor::Listener {
public:
    std::function<void(const motifxs::Voice&)> onPick;

    VoiceBrowser() {
        search_.setTextToShowWhenEmpty("search voices by name or category", theme::dim);
        search_.addListener(this);
        addAndMakeVisible(search_);

        bank_.addItem("all banks", 1);
        int id = 2;
        for (const auto& b : {"PRE1", "PRE2", "PRE3", "PRE4", "PRE5", "PRE6", "PRE7", "PRE8",
                              "GM", "Preset"})
            bank_.addItem(b, id++);
        bank_.setSelectedId(1, juce::dontSendNotification);
        bank_.onChange = [this] { rebuild(); };
        addAndMakeVisible(bank_);

        expandAll_.setButtonText("expand all");
        expandAll_.onClick = [this] {
            const bool anyClosed = std::any_of(open_.begin(), open_.end(),
                                               [](auto& kv) { return !kv.second; });
            for (auto& kv : open_) kv.second = anyClosed;
            expandAll_.setButtonText(anyClosed ? "collapse all" : "expand all");
            rebuild();
        };
        addAndMakeVisible(expandAll_);

        list_.setModel(this);
        list_.setRowHeight(22);
        addAndMakeVisible(list_);

        search_.onNavigationKey = [this](const juce::KeyPress& k) {
            const int first = firstVoiceRow();
            if (first < 0) return false;
            list_.grabKeyboardFocus();
            list_.selectRow(first, false, true);
            if (k.isKeyCode(juce::KeyPress::returnKey)) pickRow(first);
            return true;
        };
        rebuild();
    }

    void resized() override {
        auto r = getLocalBounds();
        auto top = r.removeFromTop(26);
        bank_.setBounds(top.removeFromRight(104).reduced(1));
        expandAll_.setBounds(top.removeFromRight(94).reduced(1));
        search_.setBounds(top.reduced(1));
        r.removeFromTop(4);
        list_.setBounds(r);
    }

    /// Opens the containing category and scrolls the voice into view.
    void selectByProgram(int msb, int lsb, int program) {
        for (const auto& v : motifxs::allVoices())
            if (v.msb == msb && v.lsb == lsb && v.program == program) {
                open_[std::string(v.categoryMain)] = true;
                rebuild();
                break;
            }
        for (int i = 0; i < int(rows_.size()); ++i) {
            const auto& row = rows_[size_t(i)];
            if (row.voice && row.voice->msb == msb && row.voice->lsb == lsb &&
                row.voice->program == program) {
                const juce::ScopedValueSetter<bool> guard(suppress_, true);
                list_.selectRow(i, false, true);
                return;
            }
        }
        const juce::ScopedValueSetter<bool> guard(suppress_, true);
        list_.deselectAllRows();
    }

private:
    struct Row {
        const motifxs::Voice* voice{};   // null for a category header
        std::string category;
        int count{};
    };

    void rebuild() {
        const auto query = search_.getText().trim().toLowerCase();
        const auto bankId = bank_.getSelectedId();
        const juce::String bank = bankId <= 1 ? juce::String() : bank_.getText();

        // group in catalog order so categories stay in Yamaha's ordering
        std::vector<std::string> order;
        std::map<std::string, std::vector<const motifxs::Voice*>> groups;
        for (const auto& v : motifxs::allVoices()) {
            if (bank.isNotEmpty() && juce::String(std::string(v.bank)) != bank) continue;
            if (query.isNotEmpty()) {
                const auto name = juce::String(std::string(v.name)).toLowerCase();
                const auto cat = juce::String(std::string(v.categoryMain)).toLowerCase();
                const auto sub = juce::String(std::string(v.categorySub)).toLowerCase();
                if (!name.contains(query) && !cat.contains(query) && !sub.contains(query))
                    continue;
            }
            const std::string cat(v.categoryMain.empty() ? "Other" : v.categoryMain);
            if (!groups.count(cat)) order.push_back(cat);
            groups[cat].push_back(&v);
        }

        rows_.clear();
        for (const auto& cat : order) {
            auto& voices = groups[cat];
            if (!open_.count(cat)) open_[cat] = false;
            // a search narrow enough to be readable should just show its hits
            const bool expanded = open_[cat] || (query.isNotEmpty() && rowsIfExpanded(groups) <= 120);
            rows_.push_back({nullptr, cat, int(voices.size())});
            if (expanded)
                for (const auto* v : voices) rows_.push_back({v, cat, 0});
        }
        {
            const juce::ScopedValueSetter<bool> guard(suppress_, true);
            list_.updateContent();
        }
        list_.repaint();
    }

    static int rowsIfExpanded(const std::map<std::string, std::vector<const motifxs::Voice*>>& g) {
        int n = 0;
        for (const auto& kv : g) n += int(kv.second.size());
        return n;
    }

    int firstVoiceRow() const {
        for (int i = 0; i < int(rows_.size()); ++i)
            if (rows_[size_t(i)].voice) return i;
        return -1;
    }

    void pickRow(int row) {
        if (row < 0 || row >= int(rows_.size())) return;
        if (const auto* v = rows_[size_t(row)].voice)
            if (onPick) onPick(*v);
    }

    /// Arrow-key movement should load the voice, the same as a click -- but
    /// programmatic selection (syncing the list to the rack) must not, or the
    /// app would re-send the voice it just read back.
    void selectedRowsChanged(int lastRow) override {
        if (suppress_) return;
        pickRow(lastRow);
    }

    int getNumRows() override { return int(rows_.size()); }

    void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override {
        if (row < 0 || row >= int(rows_.size())) return;
        const auto& r = rows_[size_t(row)];

        if (!r.voice) {                                   // category header
            g.setColour(theme::panelHi);
            g.fillRect(0, 0, w, h);
            g.setColour(theme::line);
            g.fillRect(0, h - 1, w, 1);

            const bool expanded = open_.count(r.category) && open_.at(r.category);
            juce::Path tri;
            const float cx = 14.0f, cy = h * 0.5f, s = 4.0f;
            if (expanded) { tri.addTriangle(cx - s, cy - s * 0.6f, cx + s, cy - s * 0.6f, cx, cy + s * 0.8f); }
            else          { tri.addTriangle(cx - s * 0.6f, cy - s, cx - s * 0.6f, cy + s, cx + s * 0.8f, cy); }
            g.setColour(theme::accent);
            g.fillPath(tri);

            g.setColour(theme::text);
            g.setFont(theme::panelFont(13.0f));
            g.drawText(juce::String(r.category), 28, 0, w - 90, h, juce::Justification::centredLeft);
            g.setColour(theme::dim);
            g.setFont(juce::FontOptions(11.0f));
            g.drawText(juce::String(r.count), w - 54, 0, 46, h, juce::Justification::centredRight);
            return;
        }

        const auto* v = r.voice;
        if (selected) g.fillAll(theme::accentDim);
        else if (row % 2) g.fillAll(theme::panelHi.withAlpha(0.28f));

        g.setColour(selected ? theme::text : theme::dim);
        g.setFont(juce::FontOptions(11.0f));
        g.drawText(juce::String(std::string(v->bank)) + " " + juce::String(std::string(v->slot)),
                   28, 0, 74, h, juce::Justification::centredLeft);

        g.setColour(theme::text);
        g.setFont(juce::FontOptions(13.0f));
        g.drawText(juce::String(std::string(v->name)), 106, 0, w - 106 - 96, h,
                   juce::Justification::centredLeft, true);

        g.setColour(theme::dim);
        g.setFont(juce::FontOptions(11.0f));
        g.drawText(juce::String(std::string(v->categorySub)), w - 92, 0, 84, h,
                   juce::Justification::centredRight);
    }

    void listBoxItemClicked(int row, const juce::MouseEvent&) override {
        if (row < 0 || row >= int(rows_.size())) return;
        const auto& r = rows_[size_t(row)];
        if (!r.voice) {
            open_[r.category] = !open_[r.category];
            rebuild();
            return;
        }
        if (onPick) onPick(*r.voice);
    }

    void textEditorTextChanged(juce::TextEditor&) override { rebuild(); }

    SearchBox search_;
    juce::ComboBox bank_;
    juce::TextButton expandAll_;
    juce::ListBox list_;
    std::vector<Row> rows_;
    std::map<std::string, bool> open_;
    bool suppress_{false};
};

/// Searchable, filterable list of the 6633 arpeggio types.
class ArpBrowser : public juce::Component,
                   private juce::ListBoxModel,
                   private juce::TextEditor::Listener {
public:
    std::function<void(const motifxs::Arpeggio&)> onPick;

    ArpBrowser() {
        search_.setTextToShowWhenEmpty("search arpeggios", theme::dim);
        search_.addListener(this);
        addAndMakeVisible(search_);

        cat_.addItem("all categories", 1);
        int id = 2;
        for (const auto& c : {"ApKb", "BaMG", "Bass", "Brass", "Chord", "Cntr", "CPrc", "DrPc",
                              "GtMG", "GtPl", "Hybrd", "Lead", "Organ", "PdMe", "RdPp", "Seq",
                              "Strng"})
            cat_.addItem(c, id++);
        cat_.setSelectedId(1, juce::dontSendNotification);
        cat_.onChange = [this] { refresh(); };
        addAndMakeVisible(cat_);

        sig_.addItem("any metre", 1);
        id = 2;
        for (const auto& s : {"4/4", "3/4", "6/8", "5/4", "5/8", "7/8", "9/8", "8/8", "15/16"})
            sig_.addItem(s, id++);
        sig_.setSelectedId(1, juce::dontSendNotification);
        sig_.onChange = [this] { refresh(); };
        addAndMakeVisible(sig_);

        // Tempo matters most for DAW work: an arp near the project tempo needs
        // the least stretching, so make it a first-class filter.
        tempo_.setRange(0.0, 300.0, 1.0);
        tempo_.setSliderStyle(juce::Slider::TwoValueHorizontal);
        tempo_.setMinAndMaxValues(0.0, 300.0, juce::dontSendNotification);
        tempo_.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        tempo_.onValueChange = [this] { refresh(); };
        addAndMakeVisible(tempo_);
        tempoLabel_.setJustificationType(juce::Justification::centredRight);
        tempoLabel_.setFont(juce::FontOptions(11.0f));
        tempoLabel_.setColour(juce::Label::textColourId, theme::dim);
        addAndMakeVisible(tempoLabel_);

        list_.setModel(this);
        list_.setRowHeight(22);
        addAndMakeVisible(list_);

        search_.onNavigationKey = [this](const juce::KeyPress& k) {
            if (rows_.empty()) return false;
            list_.grabKeyboardFocus();
            list_.selectRow(0, false, true);
            if (k.isKeyCode(juce::KeyPress::returnKey) && onPick) onPick(*rows_[0]);
            return true;
        };
        refresh();
    }

    void resized() override {
        auto r = getLocalBounds();
        search_.setBounds(r.removeFromTop(26).reduced(1));
        r.removeFromTop(3);
        auto filters = r.removeFromTop(24);
        cat_.setBounds(filters.removeFromLeft(120).reduced(1));
        sig_.setBounds(filters.removeFromLeft(90).reduced(1));
        tempoLabel_.setBounds(filters.removeFromRight(96).reduced(1));
        tempo_.setBounds(filters.reduced(4, 1));
        r.removeFromTop(4);
        list_.setBounds(r);
    }

    void selectByNumber(int number) {
        for (int i = 0; i < int(rows_.size()); ++i)
            if (rows_[size_t(i)]->number == number) {
                list_.selectRow(i, false, true);
                return;
            }
        list_.deselectAllRows();
    }

private:
    void refresh() {
        const auto q = search_.getText().trim().toLowerCase();
        const auto lo = int(tempo_.getMinValue()), hi = int(tempo_.getMaxValue());
        const juce::String cat = cat_.getSelectedId() <= 1 ? juce::String() : cat_.getText();
        const juce::String sig = sig_.getSelectedId() <= 1 ? juce::String() : sig_.getText();
        tempoLabel_.setText(lo == 0 && hi >= 300 ? "any tempo"
                                                 : juce::String(lo) + "-" + juce::String(hi) + " bpm",
                            juce::dontSendNotification);

        rows_.clear();
        for (const auto& a : motifxs::allArpeggios()) {
            if (cat.isNotEmpty() && juce::String(std::string(a.mainCategory)) != cat) continue;
            if (sig.isNotEmpty() && juce::String(std::string(a.timeSignature)) != sig) continue;
            if (a.originalTempo < lo || a.originalTempo > hi) continue;
            if (q.isNotEmpty()) {
                const auto n = juce::String(std::string(a.name)).toLowerCase();
                const auto vt = juce::String(std::string(a.voiceType)).toLowerCase();
                if (!n.contains(q) && !vt.contains(q)) continue;
            }
            rows_.push_back(&a);
        }
        list_.updateContent();
        list_.repaint();
    }

    int getNumRows() override { return int(rows_.size()); }

    void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override {
        if (row < 0 || row >= int(rows_.size())) return;
        const auto* a = rows_[size_t(row)];
        if (selected) g.fillAll(theme::violet.withAlpha(0.42f));
        else if (row % 2) g.fillAll(theme::panelHi.withAlpha(0.35f));

        g.setColour(selected ? theme::text : theme::violet.withAlpha(0.85f));
        g.setFont(juce::FontOptions(11.0f));
        g.drawText(juce::String(a->number), 8, 0, 44, h, juce::Justification::centredLeft);

        g.setColour(theme::text);
        g.setFont(juce::FontOptions(13.0f));
        g.drawText(juce::String(std::string(a->name)), 56, 0, w - 56 - 150, h,
                   juce::Justification::centredLeft, true);

        g.setColour(theme::dim);
        g.setFont(juce::FontOptions(11.0f));
        g.drawText(juce::String(std::string(a->mainCategory)) + "  " +
                       juce::String(std::string(a->timeSignature)),
                   w - 146, 0, 84, h, juce::Justification::centredLeft);
        g.setColour(a->accent ? theme::warn : theme::dim);
        g.drawText(juce::String(a->originalTempo) + " bpm", w - 62, 0, 56, h,
                   juce::Justification::centredRight);
    }

    void listBoxItemClicked(int row, const juce::MouseEvent&) override {
        if (onPick && row >= 0 && row < int(rows_.size())) onPick(*rows_[size_t(row)]);
    }

    void textEditorTextChanged(juce::TextEditor&) override { refresh(); }

    SearchBox search_;
    juce::ComboBox cat_, sig_;
    juce::Slider tempo_;
    juce::Label tempoLabel_;
    juce::ListBox list_;
    std::vector<const motifxs::Arpeggio*> rows_;
};
