#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace arploom;

ArpLoomProcessor::ArpLoomProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    // leftovers from a crash / an earlier session
    for (auto& f : takesFolder().getChildFile (".pending").findChildFiles (juce::File::findFiles, false, "*.wav")) f.deleteFile();
    takeThread.startThread();
    startTimerHz (20);
}

ArpLoomProcessor::~ArpLoomProcessor()
{
    stopTimer();
    if (takeState.load() == 1) takeState = 2;      // the audio has stopped by now: keep what was recorded
    if (takeState.load() == 2) finishTake();
    takeArmed = nullptr;
    takeWriter.reset();
    if (takePending != juce::File()) takePending.deleteFile();
    takeThread.stopThread (2000);
}

juce::File ArpLoomProcessor::takesFolder()
{
    return juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Super Goat Arpeggiator").getChildFile ("Takes");
}

// message thread: open the next take's file ahead of time
void ArpLoomProcessor::armTake()
{
    if (takeWriter != nullptr || sr <= 0) return;
    auto dir = takesFolder().getChildFile (".pending");
    dir.createDirectory();
    auto f = dir.getNonexistentChildFile ("take", ".wav");
    std::unique_ptr<juce::FileOutputStream> os (f.createOutputStream());
    if (os == nullptr || os->failedToOpen()) return;
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), sr, 2, 24, {}, 0));
    if (w == nullptr) { os.reset(); f.deleteFile(); return; }
    os.release();
    takeWriter = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (w.release(), takeThread, (int) (sr * 8));
    takePending = f; takeSr = sr; takeSamples = 0;
    takeArmed = takeWriter.get();
    takeState = 0;
}

// message thread: close the finished take and give it its real name
void ArpLoomProcessor::finishTake()
{
    takeArmed = nullptr;
    takeWriter.reset();                            // flushes the rest to disk and closes the file
    const auto secs = (double) takeSamples.load() / juce::jmax (1.0, takeSr);
    if (takePending.existsAsFile())
    {
        if (secs < .15) takePending.deleteFile();   // a tap of the transport, not a take
        else
        {
            const double bpb = juce::jmax (1.0, takeBeatsPerBar.load()), q = juce::jmax (0.0, takeStartPpq.load());
            const int bar = (int) std::floor (q / bpb) + 1, beat = (int) std::floor (std::fmod (q, bpb)) + 1;
            const auto name = "Goat take " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S")
                              + " (bar " + juce::String (bar) + (beat > 1 ? "." + juce::String (beat) : juce::String()) + ")";
            auto dest = takesFolder().getNonexistentChildFile (name, ".wav");
            if (takePending.moveFileTo (dest)) { lastTake = dest; ++takeCount; }
        }
    }
    takePending = juce::File();
    takeState = 3;
}

void ArpLoomProcessor::timerCallback()
{
    if (takeState.load() == 2) finishTake();
    if (takeState.load() == 3) { armTake(); return; }
    if (takeState.load() == 0 && std::abs (takeSr - sr) > .5)
    {   // the sample rate changed while idle: re-open the armed file at the new rate
        takeState = 3; takeArmed = nullptr; takeWriter.reset();
        takePending.deleteFile(); takePending = juce::File();
        armTake();
    }
}

bool ArpLoomProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void ArpLoomProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sr = sampleRate;
    engine.prepare (sampleRate, juce::jmax (samplesPerBlock, 512));
}

//==============================================================================
static double toD (const juce::var& v, double d = 0) { return v.isVoid() ? d : (double) v; }

void ArpLoomProcessor::applyEvents (const juce::var& v)
{
    auto list = std::make_shared<EvList>();
    list->total = juce::jmax (1.0, toD (v.getProperty ("total", 15360)));
    list->tuneSteps = (bool) v.getProperty ("steps", false);
    if (auto* arr = v.getProperty ("ev", {}).getArray())
    {
        list->ev.reserve ((size_t) arr->size());
        // each event: [t, d, p, v, fc, pn0|null, pn1|null, glFrom|null, glTicks|null, tp|null]
        for (auto& e : *arr)
        {
            auto* a = e.getArray(); if (a == nullptr || a->size() < 4) continue;
            Ev x;
            x.t = toD ((*a)[0]); x.d = toD ((*a)[1], 120); x.p = (int) toD ((*a)[2], 60); x.v = (float) toD ((*a)[3], 100);
            if (a->size() > 4) x.fc = (float) toD ((*a)[4], 1);
            if (a->size() > 6 && ! (*a)[5].isVoid() && ! (*a)[5].isUndefined()) { x.hasPan = true; x.pn0 = (float) toD ((*a)[5]); x.pn1 = (float) toD ((*a)[6]); }
            if (a->size() > 8 && ! (*a)[7].isVoid() && ! (*a)[7].isUndefined()) { x.glFrom = (int) toD ((*a)[7], -1); x.glTicks = toD ((*a)[8]); }
            if (a->size() > 9)
                if (auto* tp = (*a)[9].getArray())
                    for (auto& pt : *tp)
                        if (auto* q = pt.getArray(); q != nullptr && q->size() >= 2) x.tp.push_back ({ toD ((*q)[0]), (float) toD ((*q)[1]) });
            list->ev.push_back (std::move (x));
        }
    }
    std::sort (list->ev.begin(), list->ev.end(), [] (const Ev& a, const Ev& b) { return a.t < b.t; });
    std::atomic_store (&events, std::shared_ptr<const EvList> (list));
}

void ArpLoomProcessor::applySynth (const juce::var& v)
{
    auto p = std::make_shared<SynthParams> (SynthParams::fromVar (v.getProperty ("synth", {})));
    stateBpm = toD (v.getProperty ("bpm", 120), 120);
    engine.setParams (p);
}

void ArpLoomProcessor::setEvents (const juce::var& v)
{
    applyEvents (v);
    const juce::ScopedLock l (stateLock); eventsJson = juce::JSON::toString (v, true);
}

void ArpLoomProcessor::setSynth (const juce::var& v)
{
    applySynth (v);
    const juce::ScopedLock l (stateLock); synthJson = juce::JSON::toString (v, true);
}

void ArpLoomProcessor::setUiState (const juce::String& json) { const juce::ScopedLock l (stateLock); uiState = json; }
juce::String ArpLoomProcessor::getUiState() const             { const juce::ScopedLock l (stateLock); return uiState; }

void ArpLoomProcessor::setTransport (bool play, double posTick)
{
    previewPos = posTick; previewJump = true; previewPlaying = play;
}

void ArpLoomProcessor::queuePlay (const juce::var& v)
{
    PlayReq r;
    auto& s = r.spec;
    s.pitch = (int) toD (v.getProperty ("p", 60), 60);
    s.vel = (float) toD (v.getProperty ("v", 100), 100);
    s.durSec = toD (v.getProperty ("d", .3), .3);
    s.filterMul = (float) toD (v.getProperty ("fm", 1), 1);
    if (auto* pn = v.getProperty ("pn", {}).getArray(); pn != nullptr && pn->size() >= 2) { s.hasPan = true; s.pan0 = (float) toD ((*pn)[0]); s.pan1 = (float) toD ((*pn)[1]); }
    if (auto* gl = v.getProperty ("gl", {}).getArray(); gl != nullptr && gl->size() >= 2) { s.glideFrom = (int) toD ((*gl)[0], -1); s.glideSec = toD ((*gl)[1]); }
    if (auto* tp = v.getProperty ("tp", {}).getArray())
        for (auto& pt : *tp) if (auto* q = pt.getArray(); q != nullptr && q->size() >= 2) s.tune.push_back ({ toD ((*q)[0]), (float) toD ((*q)[1]) });
    s.tuneSteps = (bool) v.getProperty ("steps", false);
    const auto syn = v.getProperty ("syn", {});
    if (syn.isObject()) s.params = std::make_shared<SynthParams> (SynthParams::fromVar (syn));
    r.delaySec = juce::jlimit (0.0, 2.0, toD (v.getProperty ("delay", 0)));

    const juce::ScopedLock l (playWriteLock);
    int s1, n1, s2, n2;
    playFifo.prepareToWrite (1, s1, n1, s2, n2);
    if (n1 > 0) playRing[(size_t) s1] = std::move (r);
    else if (n2 > 0) playRing[(size_t) s2] = std::move (r);
    playFifo.finishedWrite (n1 + n2);
}

//==============================================================================
void ArpLoomProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();
    const int n = buffer.getNumSamples();

    // --- host transport ---
    bool hostPlay = false, hostRec = false; double ppq = 0, bpm = stateBpm.load(), beatsPerBar = 4;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            if (auto b = pos->getBpm()) bpm = *b;
            hostPlay = pos->getIsPlaying();
            hostRec = pos->getIsRecording();
            if (auto q = pos->getPpqPosition()) ppq = *q;
            if (auto ts = pos->getTimeSignature(); ts && ts->denominator > 0) beatsPerBar = ts->numerator * 4.0 / ts->denominator;
        }
    engine.setTempo (bpm);
    uiBpm = bpm;
    const double ticksPerSec = bpm / 60.0 * 480.0, tps = ticksPerSec / sr, blockTicks = n * tps;
    auto ev = std::atomic_load (&events);

    bool running = false; double startTick = 0;
    if (hostPlay) { running = true; startTick = ppq * 480.0; previewPlaying = false; }
    else if (previewPlaying)
    {
        running = true; startTick = previewPos.load();
        previewPos = startTick + blockTicks;
    }
    if (wasRunning && ! running) engine.stopSequenced();
    if (previewJump.exchange (false) && running && ! hostPlay) engine.stopSequenced();
    wasRunning = running;
    uiPlaying = running; uiHostPlaying = hostPlay;

    if (running && ev != nullptr && ! ev->ev.empty())
    {
        const double total = ev->total;
        double a = std::fmod (startTick, total); if (a < 0) a += total;
        const double b = a + blockTicks;
        auto sched = [&] (double lo, double hi, double offTicks)
        {
            auto it = std::lower_bound (ev->ev.begin(), ev->ev.end(), lo, [] (const Ev& e, double t) { return e.t < t; });
            for (; it != ev->ev.end() && it->t < hi; ++it)
            {
                const auto& e = *it;
                NoteSpec s;
                s.pitch = e.p; s.vel = e.v; s.durSec = e.d / ticksPerSec; s.filterMul = e.fc;
                s.hasPan = e.hasPan; s.pan0 = e.pn0; s.pan1 = e.pn1;
                if (e.glFrom >= 0) { s.glideFrom = e.glFrom; s.glideSec = e.glTicks / ticksPerSec; }
                for (auto& q : e.tp) s.tune.push_back ({ q.first / ticksPerSec, q.second });
                s.tuneSteps = ev->tuneSteps;
                engine.startNote (s, (int) ((e.t - lo + offTicks) / tps));
            }
        };
        sched (a, juce::jmin (b, total), 0);
        if (b > total) sched (0, b - total, total - a);
        uiPosTick = a;
    }
    else if (! running) uiPosTick = previewPos.load();

    // --- notes requested by the page ---
    {
        int s1, n1, s2, n2;
        playFifo.prepareToRead (playFifo.getNumReady(), s1, n1, s2, n2);
        auto take = [&] (int start, int cnt) { for (int i = 0; i < cnt; ++i) { auto& r = playRing[(size_t) (start + i)]; engine.startNote (r.spec, (int) (r.delaySec * sr)); } };
        take (s1, n1); take (s2, n2);
        playFifo.finishedRead (n1 + n2);
    }

    // --- MIDI from the DAW (your keyboard) ---
    for (const auto meta : midi)
    {
        const auto m = meta.getMessage();
        if (m.getRawDataSize() <= 3)
        {   // forward to the page (live arp, recording, the MIDI panel)
            int s1, n1, s2, n2;
            midiFifo.prepareToWrite (1, s1, n1, s2, n2);
            const auto* d = m.getRawData();
            const juce::uint32 packed = (juce::uint32) d[0] | ((juce::uint32) (m.getRawDataSize() > 1 ? d[1] : 0) << 8) | ((juce::uint32) (m.getRawDataSize() > 2 ? d[2] : 0) << 16);
            if (n1 > 0) midiRing[(size_t) s1] = packed; else if (n2 > 0) midiRing[(size_t) s2] = packed;
            midiFifo.finishedWrite (n1 + n2);
        }
        if (! playsKeysDirectly) continue;   // Live arp mode: the page plays the arpeggio
        if (m.isNoteOn())
        {
            NoteSpec s; s.pitch = m.getNoteNumber(); s.vel = (float) m.getVelocity(); s.durSec = -1; s.live = true;
            engine.startNote (s, meta.samplePosition);
        }
        else if (m.isNoteOff()) engine.noteOffLive (m.getNoteNumber());
        else if (m.isSustainPedalOn()) engine.sustainPedal (true);
        else if (m.isSustainPedalOff()) engine.sustainPedal (false);
        else if (m.isPitchWheel()) engine.setBend ((float) ((m.getPitchWheelValue() - 8192) / 8192.0 * 200.0));
        else if (m.isAllNotesOff() || m.isAllSoundOff()) engine.panic();
    }

    engine.render (buffer, 0, n);

    // --- takes: record our own output while the DAW records (or plays) ---
    const int mode = takeMode.load();
    const bool wantTake = hostPlay && (mode == TakeWhenPlaying || (mode == TakeWhenRecording && hostRec));
    if (wantTake && takeState.load() == 0 && takeArmed.load() != nullptr)
    {
        takeStartPpq = ppq; takeBeatsPerBar = beatsPerBar;
        takeState = 1;
    }
    if (takeState.load() == 1)
    {
        if (wantTake)
        {
            if (auto* w = takeArmed.load()) { w->write (buffer.getArrayOfReadPointers(), n); takeSamples += n; }
        }
        else takeState = 2;
    }
}

//==============================================================================
void ArpLoomProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    {
        const juce::ScopedLock l (stateLock);
        o->setProperty ("ui", uiState);
        o->setProperty ("events", eventsJson);
        o->setProperty ("synth", synthJson);
    }
    o->setProperty ("version", 1);
    o->setProperty ("takeMode", takeMode.load());
    juce::MemoryOutputStream mo (dest, false);
    mo.writeString (juce::JSON::toString (juce::var (o.get()), true));
}

void ArpLoomProcessor::setStateInformation (const void* data, int size)
{
    const auto text = juce::MemoryInputStream (data, (size_t) size, false).readString();
    const auto v = juce::JSON::parse (text);
    if (! v.isObject()) return;
    if (v.hasProperty ("takeMode")) setTakeMode ((int) v.getProperty ("takeMode", 1));
    {
        const juce::ScopedLock l (stateLock);
        uiState = v.getProperty ("ui", "").toString();
        eventsJson = v.getProperty ("events", "").toString();
        synthJson = v.getProperty ("synth", "").toString();
    }
    // the plugin plays straight away, even before its window is opened
    if (eventsJson.isNotEmpty()) applyEvents (juce::JSON::parse (eventsJson));
    if (synthJson.isNotEmpty()) applySynth (juce::JSON::parse (synthJson));
    ++stateVersion;
}

juce::AudioProcessorEditor* ArpLoomProcessor::createEditor() { return new ArpLoomEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new ArpLoomProcessor(); }
