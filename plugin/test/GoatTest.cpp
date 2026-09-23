#include "../Source/PluginProcessor.h"
#include <thread>

struct FakeHead : juce::AudioPlayHead
{
    double ppq = 0, bpm = 120; bool playing = true, recording = false;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p; p.setBpm (bpm); p.setIsPlaying (playing); p.setIsRecording (recording); p.setPpqPosition (ppq); return p;
    }
};

static juce::String makeState()
{
    juce::String ev;
    for (int bar = 0; bar < 8; ++bar)
        for (int i = 0; i < 16; ++i)
        {
            const int t = bar * 1920 + i * 120, p = 48 + (i * 7 + bar * 3) % 36;
            juce::String tp = (i % 3 == 0) ? ",null,null,null,null,[[0,0],[60,25],[120,-10]]" : (i % 3 == 1 ? ",-0.5,0.5,55,40" : "");
            if (ev.isNotEmpty()) ev << ",";
            ev << "[" << t << ",110," << p << "," << (60 + i * 4) << ",0.9" << tp << "]";
        }
    const juce::String events = "{\"total\":15360,\"steps\":false,\"ev\":[" + ev + "]}";
    const juce::String synth = R"({"synth":{"osc1":{"wave":"sid_combo","oct":0,"semi":0,"det":0,"lvl":0.7,"uni":3,"spread":12,"width":0.5},"osc2":{"wave":"sine","oct":1,"semi":0,"det":0,"lvl":0.3,"uni":1,"spread":0,"width":0},"sub":0.2,"noise":0.05,"filt":{"type":"lowpass","cut":2362,"res":0.16,"env":-0.3,"key":0},"amp":{"a":0.001,"d":0.1663,"s":0.873,"r":0.06},"fenv":{"a":0.001,"d":0.1,"s":1,"r":0.1},"delay":{"time":"3/16","fb":0.65,"mix":0.43},"rev":{"size":6.9,"mix":0.83},"vol":0.8},"bpm":120})";
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty ("ui", ""); o->setProperty ("events", events); o->setProperty ("synth", synth); o->setProperty ("version", 1);
    return juce::JSON::toString (juce::var (o.get()));
}

static void run (const char* name, double sr, int prepBlock, std::function<int (int)> blockAt, int blocks, bool stress)
{
    printf ("--- %s (sr %.0f, prepared %d) ... ", name, sr, prepBlock); fflush (stdout);
    std::unique_ptr<juce::AudioProcessor> proc (createPluginFilter());
    auto* ap = dynamic_cast<ArpLoomProcessor*> (proc.get());
    const auto st = makeState();
    proc->setStateInformation (st.toRawUTF8(), (int) st.getNumBytesAsUTF8());
    proc->setPlayConfigDetails (0, 2, sr, prepBlock);
    proc->prepareToPlay (sr, prepBlock);
    FakeHead head; proc->setPlayHead (&head);
    std::atomic<bool> done { false };
    std::thread msg ([&] {   // what the page does from the message thread while playing
        int k = 0;
        while (! done)
        {
            if (stress)
            {
                ap->setSynth (juce::JSON::parse (k % 2 ? R"({"synth":{"rev":{"size":2.1,"mix":0.4}},"bpm":100})" : R"({"synth":{"rev":{"size":6.9,"mix":0.83},"osc1":{"wave":"saw","uni":7,"spread":30,"width":1}},"bpm":140})"));
                ap->queuePlay (juce::JSON::parse (R"({"p":64,"d":0.4,"v":100,"delay":0.05,"tp":[[0,0],[0.1,30]],"pn":[-1,1],"gl":[60,0.1]})"));
                ap->setEvents (juce::JSON::parse (makeState()).getProperty ("events", {}).toString() == "" ? juce::var() : juce::JSON::parse (juce::JSON::parse (makeState()).getProperty ("events", {}).toString()));
            }
            ++k; std::this_thread::sleep_for (std::chrono::milliseconds (5));
        }
    });
    juce::AudioBuffer<float> buf (2, 8192); juce::MidiBuffer midi;
    double peak = 0; bool bad = false;
    for (int b = 0; b < blocks; ++b)
    {
        const int n = blockAt (b);
        juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), 2, n);
        if (b == blocks / 2) head.playing = false;           // stop, then start again
        if (b == blocks / 2 + 20) { head.playing = true; head.ppq = 3.7; }
        proc->processBlock (view, midi);
        for (int c = 0; c < 2; ++c) for (int i = 0; i < n; ++i) { const float v = view.getSample (c, i); if (! std::isfinite (v)) bad = true; peak = juce::jmax (peak, (double) std::abs (v)); }
        if (head.playing) head.ppq += n / sr * head.bpm / 60.0;
    }
    done = true; msg.join();
    proc->releaseResources();
    printf ("ok, peak %.3f%s\n", peak, bad ? "  ** NaN/inf in output **" : "");
}

static void onCrash (void*) { printf ("\nCRASH\n%s\n", juce::SystemStats::getStackBacktrace().toRawUTF8()); fflush (stdout); std::_Exit (3); }

static void takeTest()
{
    printf ("--- take recording ... "); fflush (stdout);
    auto* mm = juce::MessageManager::getInstance();
    std::unique_ptr<juce::AudioProcessor> proc (createPluginFilter());
    const auto st = makeState();
    proc->setStateInformation (st.toRawUTF8(), (int) st.getNumBytesAsUTF8());
    proc->setPlayConfigDetails (0, 2, 44100, 256); proc->prepareToPlay (44100, 256);
    FakeHead head; head.playing = false; proc->setPlayHead (&head);
    juce::AudioBuffer<float> buf (2, 256); juce::MidiBuffer midi;
    const auto before = ArpLoomProcessor::takesFolder().findChildFiles (juce::File::findFiles, false, "*.wav").size();
    auto blocks = [&] (int count) { for (int b = 0; b < count; ++b) { proc->processBlock (buf, midi); if (head.playing) head.ppq += 256 / 44100.0 * 2; if (b % 8 == 0) mm->runDispatchLoopUntil (1); } };
    mm->runDispatchLoopUntil (200);                       // arm
    head.playing = true; blocks (100);                    // playing only: no take
    head.recording = true; head.ppq = 16.0; blocks (1723); // ~10 s recording from bar 5
    head.playing = head.recording = false; blocks (10); mm->runDispatchLoopUntil (400);
    auto files = ArpLoomProcessor::takesFolder().findChildFiles (juce::File::findFiles, false, "*.wav");
    printf ("%d new take(s)\n", files.size() - before);
    for (auto& f : files)
        if (auto* ap = dynamic_cast<ArpLoomProcessor*> (proc.get()); ap && f == ap->getLastTake())
        {
            juce::WavAudioFormat wav; std::unique_ptr<juce::AudioFormatReader> r (wav.createReaderFor (f.createInputStream().release(), true));
            printf ("    %s: %.2f s, %d-bit, %.0f Hz\n", f.getFileName().toRawUTF8(), r ? r->lengthInSamples / r->sampleRate : 0.0, r ? (int) r->bitsPerSample : 0, r ? r->sampleRate : 0.0);
            r.reset(); f.deleteFile();   // the test's own take, not the user's
        }
    proc.reset();
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    juce::SystemStats::setApplicationCrashHandler (onCrash);
    run ("steady 64", 48000, 64, [] (int) { return 64; }, 6000, false);
    run ("steady 512 @44.1k", 44100, 512, [] (int) { return 512; }, 1500, false);
    run ("variable blocks <= prepared", 48000, 1024, [] (int b) { return 1 + (b * 37) % 1024; }, 3000, false);
    run ("message-thread stress", 48000, 128, [] (int) { return 128; }, 6000, true);
    run ("blocks LARGER than prepared", 48000, 64, [] (int b) { return b % 5 == 0 ? 2048 : 64; }, 2000, false);
    takeTest();
    {   // export path: the page sends standard base64 from a data: URL
        juce::MemoryBlock src; for (int i = 0; i < 300000; ++i) src.append (&i, 1);
        const auto b64 = juce::Base64::toBase64 (src.getData(), src.getSize());
        juce::MemoryOutputStream out; const bool ok = juce::Base64::convertFromBase64 (out, b64);
        juce::MemoryBlock old; const bool oldOk = old.fromBase64Encoding (b64);
        printf ("--- export decode: new %s (%d bytes), old way %s (%d bytes)\n", ok && out.getMemoryBlock() == src ? "OK" : "FAIL", (int) out.getDataSize(), oldOk ? "ok" : "failed", (int) old.getSize());
    }
    printf ("all runs finished\n");
    return 0;
}
