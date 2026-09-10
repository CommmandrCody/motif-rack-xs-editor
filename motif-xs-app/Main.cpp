#include <juce_gui_basics/juce_gui_basics.h>

#include "motifxs/worker.hpp"

#include "MainComponent.h"

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
            setContentOwned(new MainComponent(worker_), true);
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
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Window)
    };

    std::unique_ptr<Window> window_;
};

START_JUCE_APPLICATION(MotifXsApplication)
