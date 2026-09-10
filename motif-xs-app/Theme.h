#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

/// A dark, restrained palette. The rack is the instrument; the UI should stay
/// out of the way and keep the eye on names and values.
namespace theme {
inline const juce::Colour bg{0xff17181c};
inline const juce::Colour panel{0xff1f2126};
inline const juce::Colour panelHi{0xff282b32};
inline const juce::Colour line{0xff32363f};
inline const juce::Colour text{0xffe4e6ea};
inline const juce::Colour dim{0xff8b919c};
inline const juce::Colour accent{0xff5aa9e6};
inline const juce::Colour accentDim{0xff2f5f80};
inline const juce::Colour good{0xff5ec98a};
inline const juce::Colour warn{0xffe6a23c};
inline const juce::Colour bad{0xffe06c75};

/// Knobs and lists, styled once so every panel matches.
class Look : public juce::LookAndFeel_V4 {
public:
    Look() {
        setColour(juce::ResizableWindow::backgroundColourId, bg);
        setColour(juce::Label::textColourId, text);
        setColour(juce::TextEditor::backgroundColourId, panelHi);
        setColour(juce::TextEditor::textColourId, text);
        setColour(juce::TextEditor::outlineColourId, line);
        setColour(juce::TextEditor::focusedOutlineColourId, accent);
        setColour(juce::ListBox::backgroundColourId, panel);
        setColour(juce::ComboBox::backgroundColourId, panelHi);
        setColour(juce::ComboBox::textColourId, text);
        setColour(juce::ComboBox::outlineColourId, line);
        setColour(juce::ComboBox::arrowColourId, dim);
        setColour(juce::PopupMenu::backgroundColourId, panelHi);
        setColour(juce::PopupMenu::textColourId, text);
        setColour(juce::PopupMenu::highlightedBackgroundColourId, accentDim);
        setColour(juce::TextButton::buttonColourId, panelHi);
        setColour(juce::TextButton::textColourOffId, text);
        setColour(juce::ScrollBar::thumbColourId, line);
    }

    void drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h, float pos,
                          float startAngle, float endAngle, juce::Slider& s) override {
        const auto area = juce::Rectangle<int>(x, y, w, h).toFloat().reduced(3.0f);
        const auto radius = juce::jmin(area.getWidth(), area.getHeight()) / 2.0f;
        const auto centre = area.getCentre();
        const float thickness = juce::jmax(3.0f, radius * 0.22f);
        const float angle = startAngle + pos * (endAngle - startAngle);

        juce::Path track;
        track.addCentredArc(centre.x, centre.y, radius - thickness, radius - thickness,
                            0.0f, startAngle, endAngle, true);
        g.setColour(line);
        g.strokePath(track, juce::PathStrokeType(thickness, juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));

        // Bipolar parameters read from the centre, unipolar from the left.
        const bool bipolar = s.getMinimum() < 0.0 && s.getMaximum() > 0.0;
        const float from = bipolar ? startAngle + 0.5f * (endAngle - startAngle) : startAngle;
        if (std::abs(angle - from) > 0.001f) {
            juce::Path value;
            value.addCentredArc(centre.x, centre.y, radius - thickness, radius - thickness,
                                0.0f, juce::jmin(from, angle), juce::jmax(from, angle), true);
            g.setColour(s.isEnabled() ? accent : line.brighter(0.1f));
            g.strokePath(value, juce::PathStrokeType(thickness, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
        }

        juce::Path pointer;
        pointer.startNewSubPath(centre.x, centre.y - radius + thickness * 0.4f);
        pointer.lineTo(centre.x, centre.y - radius + thickness * 1.8f);
        g.setColour(s.isEnabled() ? text : dim);
        g.strokePath(pointer, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded),
                     juce::AffineTransform::rotation(angle, centre.x, centre.y));
    }
};
}  // namespace theme
