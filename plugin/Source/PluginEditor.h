#pragma once
#include <juce_gui_extra/juce_gui_extra.h>
#include "PluginProcessor.h"

class ArpLoomEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit ArpLoomEditor (ArpLoomProcessor&);
    ~ArpLoomEditor() override;
    void resized() override;
    void paint (juce::Graphics& g) override { g.fillAll (juce::Colour (0xff0c0e13)); }

private:
    void timerCallback() override;
    std::optional<juce::WebBrowserComponent::Resource> getResource (const juce::String& url);
    void sendInit();
    void saveFile (const juce::var& v);
    void dragLastTake();
    void sendTakeInfo();
    juce::String sentTakeKey;

    ArpLoomProcessor& proc;
    bool envReady;                         // must be initialised before the web view (sets WebView2 flags)
    juce::WebBrowserComponent web;
    bool pageReady = false;
    int seenStateVersion = -1;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArpLoomEditor)
};
