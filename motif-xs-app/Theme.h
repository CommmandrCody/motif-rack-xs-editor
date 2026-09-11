#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

/// Drawn from the instrument it controls: the MOTIF XS is a near-black chassis
/// with a saturated blue backlit display, and Yamaha's own mark is violet. The
/// rack is the instrument, so the UI still stays out of the way -- the colour
/// is on the values, not the furniture.
namespace theme {
inline const juce::Colour bg{0xff121317};        // chassis
inline const juce::Colour panel{0xff1a1c22};
inline const juce::Colour panelHi{0xff23262e};
inline const juce::Colour line{0xff333846};
inline const juce::Colour text{0xffe8ebf2};
inline const juce::Colour dim{0xff868d9d};
inline const juce::Colour accent{0xff1fa8dc};    // the backlit display
inline const juce::Colour accentDim{0xff174e69};
inline const juce::Colour violet{0xff7a5cd6};    // Yamaha's mark
inline const juce::Colour good{0xff3fc294};
inline const juce::Colour warn{0xffe8a33d};
inline const juce::Colour bad{0xffe0584f};

/// Panel lettering. DIN Condensed is the typeface equipment panels have been
/// silkscreened in for decades, which is exactly the association wanted here;
/// Avenir Next Condensed stands in if it is missing, and the system font if
/// both are. Used for labels only -- lists and values stay in the system face,
/// where legibility matters more than character.
inline juce::Font panelFont(float height, bool bold = true) {
    static const juce::String family = [] {
        const auto installed = juce::Font::findAllTypefaceNames();
        for (const char* candidate : {"DIN Condensed", "Avenir Next Condensed",
                                      "HelveticaNeue-CondensedBold"})
            if (installed.contains(candidate)) return juce::String(candidate);
        return juce::String{};
    }();
    if (family.isEmpty())
        return juce::Font(juce::FontOptions(height).withStyle(bold ? "Bold" : "Regular"));
    // DIN Condensed ships bold only, and reads small, so it is nudged up.
    return juce::Font(juce::FontOptions(family, height * 1.18f, juce::Font::plain));
}

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
