#include "PluginEditor.h"
#include "BinaryData.h"
#include <cstdlib>

static bool prepareWebViewEnvironment()
{
   #if JUCE_WINDOWS
    // let the page's audio clock run without a click first (the page never outputs sound itself)
    _putenv_s ("WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", "--autoplay-policy=no-user-gesture-required");
   #endif
    return true;
}

// one web-view data folder per host program (Reason, the standalone app, other DAWs):
// WebView2 refuses to share a folder between different programs, which left a blank white window
static juce::File webViewDataFolder()
{
    const auto host = juce::File::getSpecialLocation (juce::File::hostApplicationPath).getFileNameWithoutExtension();
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
             .getChildFile ("ArpLoom").getChildFile ("WebView2").getChildFile (host.isNotEmpty() ? host : juce::String ("default"));
}

ArpLoomEditor::ArpLoomEditor (ArpLoomProcessor& p)
    : AudioProcessorEditor (p), proc (p),
      envReady (prepareWebViewEnvironment()),
      web (juce::WebBrowserComponent::Options{}
               .withBackend (juce::WebBrowserComponent::Options::Backend::webview2)
               .withWinWebView2Options (juce::WebBrowserComponent::Options::WinWebView2{}
                                            .withUserDataFolder (webViewDataFolder())
                                            .withBackgroundColour (juce::Colour (0xff0c0e13))
                                            .withStatusBarDisabled())
               .withNativeIntegrationEnabled()
               .withKeepPageLoadedWhenBrowserIsHidden()
               .withResourceProvider ([this] (const juce::String& url) { return getResource (url); })
               .withEventListener ("ready",     [this] (juce::var)   { pageReady = true; sendInit(); })
               .withEventListener ("state",     [this] (juce::var v) { proc.setUiState (v.getProperty ("json", "").toString()); })
               .withEventListener ("events",    [this] (juce::var v) { proc.setEvents (v); })
               .withEventListener ("synth",     [this] (juce::var v) { proc.setSynth (v); })
               .withEventListener ("play",      [this] (juce::var v) { proc.queuePlay (v); })
               .withEventListener ("transport", [this] (juce::var v) { proc.setTransport ((bool) v.getProperty ("play", false), (double) v.getProperty ("pos", 0.0)); })
               .withEventListener ("midiMode",  [this] (juce::var v) { proc.setMidiMode (v.getProperty ("mode", "arp").toString()); })
               .withEventListener ("saveFile",  [this] (juce::var v) { saveFile (v); })
               .withEventListener ("openFile",  [this] (juce::var v) { openFile (v); })
               .withEventListener ("takeMode",  [this] (juce::var v) { proc.setTakeMode ((int) v.getProperty ("mode", 1)); sentTakeKey.clear(); })
               .withEventListener ("takeReveal",[this] (juce::var)   { auto f = proc.getLastTake(); if (f.existsAsFile()) f.revealToUser(); else ArpLoomProcessor::takesFolder().revealToUser(); })
               .withEventListener ("takeDrag",  [this] (juce::var)   { dragLastTake(); }))
{
    juce::ignoreUnused (envReady);
    addAndMakeVisible (web);
    setResizable (true, true);
    setResizeLimits (800, 500, 5000, 3200);
    setSize (1500, 950);
    web.goToURL (juce::WebBrowserComponent::getResourceProviderRoot());
    seenStateVersion = proc.stateVersion.load();
    startTimerHz (30);
}

ArpLoomEditor::~ArpLoomEditor() { stopTimer(); }

void ArpLoomEditor::resized() { web.setBounds (getLocalBounds()); }

std::optional<juce::WebBrowserComponent::Resource> ArpLoomEditor::getResource (const juce::String& url)
{
    const auto path = url.fromFirstOccurrenceOf ("/", false, false).upToFirstOccurrenceOf ("?", false, false);
    if (path.isEmpty() || path == "index.html" || path == "arp-loom.html")
    {
        for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
            if (juce::String (BinaryData::originalFilenames[i]) == "arp-loom.html")
            {
                int size = 0;
                const char* data = BinaryData::getNamedResource (BinaryData::namedResourceList[i], size);
                std::vector<std::byte> bytes ((size_t) size);
                std::memcpy (bytes.data(), data, (size_t) size);
                return juce::WebBrowserComponent::Resource { std::move (bytes), "text/html" };
            }
    }
    return std::nullopt;
}

void ArpLoomEditor::sendInit()
{
    sentTakeKey.clear();
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty ("state", proc.getUiState());
    o->setProperty ("bpm", proc.uiBpm.load());
    web.emitEventIfBrowserIsVisible ("init", juce::var (o.get()));
    seenStateVersion = proc.stateVersion.load();
}

void ArpLoomEditor::timerCallback()
{
    if (! pageReady) return;
    if (proc.stateVersion.load() != seenStateVersion) sendInit();   // the DAW loaded a song while the window was open

    juce::DynamicObject::Ptr pos = new juce::DynamicObject();
    pos->setProperty ("tick", proc.uiPosTick.load());
    pos->setProperty ("playing", proc.uiPlaying.load());
    pos->setProperty ("host", proc.uiHostPlaying.load());
    pos->setProperty ("bpm", proc.uiBpm.load());
    web.emitEventIfBrowserIsVisible ("pos", juce::var (pos.get()));
    sendTakeInfo();

    const int ready = proc.midiFifo.getNumReady();
    if (ready > 0)
    {
        juce::Array<juce::var> msgs;
        int s1, n1, s2, n2;
        proc.midiFifo.prepareToRead (ready, s1, n1, s2, n2);
        auto take = [&] (int start, int cnt) { for (int i = 0; i < cnt; ++i) { const auto w = proc.midiRing[(size_t) (start + i)]; msgs.add (juce::Array<juce::var> { (int) (w & 255), (int) ((w >> 8) & 255), (int) ((w >> 16) & 255) }); } };
        take (s1, n1); take (s2, n2);
        proc.midiFifo.finishedRead (n1 + n2);
        juce::DynamicObject::Ptr m = new juce::DynamicObject();
        m->setProperty ("m", msgs);
        web.emitEventIfBrowserIsVisible ("midi", juce::var (m.get()));
    }
}

void ArpLoomEditor::dragLastTake()
{
    const auto f = proc.getLastTake();
    if (! f.existsAsFile()) return;
    juce::DragAndDropContainer::performExternalDragDropOfFiles ({ f.getFullPathName() }, false, this);
}

void ArpLoomEditor::sendTakeInfo()
{
    const auto last = proc.getLastTake();
    const juce::String key = juce::String ((int) proc.isTakeRecording()) + "|" + juce::String (proc.getTakeMode()) + "|" + last.getFullPathName();
    if (key == sentTakeKey) return;
    sentTakeKey = key;
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty ("rec", proc.isTakeRecording());
    o->setProperty ("mode", proc.getTakeMode());
    o->setProperty ("last", last.existsAsFile() ? last.getFileName() : juce::String());
    o->setProperty ("count", proc.getTakeCount());
    web.emitEventIfBrowserIsVisible ("take", juce::var (o.get()));
}

// the page asks for a file (Load project, Import MIDI, Restore backup): a native Open dialog,
// then the file goes back to the page as base64 — the web view's own file picker isn't dependable inside a DAW
void ArpLoomEditor::openFile (const juce::var& v)
{
    const auto id = v.getProperty ("id", "").toString();
    auto start = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Downloads");   // where the browser version saves things
    if (! start.isDirectory()) start = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
    chooser = std::make_unique<juce::FileChooser> (v.getProperty ("title", "Open").toString(), start, v.getProperty ("patterns", "*").toString());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe = juce::Component::SafePointer<ArpLoomEditor> (this), id] (const juce::FileChooser& fc)
                          {
                              if (safe == nullptr) return;
                              juce::DynamicObject::Ptr o = new juce::DynamicObject();
                              o->setProperty ("id", id);
                              const auto f = fc.getResult();
                              if (f == juce::File())
                                  o->setProperty ("cancelled", true);
                              else
                              {
                                  juce::MemoryBlock data;
                                  if (f.getSize() > 64 * 1024 * 1024 || ! f.loadFileAsData (data))
                                      o->setProperty ("error", "Couldn't read " + f.getFileName());
                                  else
                                  {
                                      o->setProperty ("name", f.getFileName());
                                      o->setProperty ("b64", juce::Base64::toBase64 (data.getData(), data.getSize()));
                                  }
                              }
                              safe->web.emitEventIfBrowserIsVisible ("fileOpened", juce::var (o.get()));
                          });
}

void ArpLoomEditor::saveFile (const juce::var& v)
{
    const auto name = v.getProperty ("name", "arp-loom").toString();
    // the page sends standard base64 (from a data: URL) — MemoryBlock::fromBase64Encoding is JUCE's own
    // different format, which decoded to nothing, and replaceWithData() deletes the file when given 0 bytes
    juce::MemoryOutputStream decoded;
    const bool ok = juce::Base64::convertFromBase64 (decoded, v.getProperty ("b64", "").toString());
    auto data = std::make_shared<juce::MemoryBlock> (decoded.getData(), decoded.getDataSize());
    auto report = [safe = juce::Component::SafePointer<ArpLoomEditor> (this)] (const juce::String& msg, bool good)
    {
        if (safe == nullptr) return;
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty ("msg", msg); o->setProperty ("ok", good);
        safe->web.emitEventIfBrowserIsVisible ("saved", juce::var (o.get()));
    };
    if (! ok || data->getSize() == 0) { report ("Couldn't save " + name + ": the file arrived empty.", false); return; }

    chooser = std::make_unique<juce::FileChooser> ("Save " + name,
                                                   juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile (name));
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                          [data, report] (const juce::FileChooser& fc)
                          {
                              const auto f = fc.getResult();
                              if (f == juce::File()) return;
                              if (f.replaceWithData (data->getData(), data->getSize()))
                                  report ("Saved " + f.getFullPathName(), true);
                              else
                                  report ("Couldn't write " + f.getFullPathName(), false);
                          });
}
