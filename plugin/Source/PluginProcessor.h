#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "SynthEngine.h"
#include <atomic>

class ArpLoomProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    ArpLoomProcessor();
    ~ArpLoomProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "Super Goat Arpeggiator"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 8.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // ---- called by the editor (message thread) ----
    void setUiState (const juce::String& json);
    juce::String getUiState() const;
    void setEvents (const juce::var& v);
    void setSynth (const juce::var& v);
    void queuePlay (const juce::var& v);
    void setTransport (bool play, double posTick);
    void setMidiMode (const juce::String& m) { playsKeysDirectly = (m == "play" || m == "rec"); }

    // ---- takes: the plugin writes its own output to a WAV while the DAW records ----
    enum TakeMode { TakeOff = 0, TakeWhenRecording = 1, TakeWhenPlaying = 2 };
    void setTakeMode (int m) { takeMode = juce::jlimit (0, 2, m); }
    int getTakeMode() const { return takeMode; }
    bool isTakeRecording() const { return takeState.load() == 1; }
    juce::File getLastTake() const { return lastTake; }   // message thread only
    int getTakeCount() const { return takeCount; }
    static juce::File takesFolder();

    // ---- read by the editor timer ----
    std::atomic<double> uiPosTick { 0 };
    std::atomic<bool> uiPlaying { false }, uiHostPlaying { false };
    std::atomic<double> uiBpm { 120 };
    std::atomic<int> stateVersion { 0 };        // bumps when the DAW restores a saved song
    juce::AbstractFifo midiFifo { 1024 };
    std::array<juce::uint32, 1024> midiRing {};

private:
    struct Ev
    {
        double t = 0, d = 120; int p = 60; float v = 100, fc = 1;
        bool hasPan = false; float pn0 = 0, pn1 = 0;
        int glFrom = -1; double glTicks = 0;
        std::vector<std::pair<double, float>> tp;   // (ticks from note start, cents)
    };
    struct EvList { std::vector<Ev> ev; double total = 15360; bool tuneSteps = false; };

    arploom::Engine engine;
    std::shared_ptr<const EvList> events = std::make_shared<EvList>();
    double sr = 48000;

    // notes requested by the page (auditions, live arp) — handed to the audio thread through a FIFO
    struct PlayReq { arploom::NoteSpec spec; double delaySec = 0; };
    juce::AbstractFifo playFifo { 256 };
    std::array<PlayReq, 256> playRing;
    juce::CriticalSection playWriteLock;

    std::atomic<bool> previewPlaying { false };
    std::atomic<double> previewPos { 0 }, stateBpm { 120 };
    std::atomic<bool> previewJump { false };
    bool wasRunning = false;
    std::atomic<bool> playsKeysDirectly { false };

    juce::String uiState, eventsJson, synthJson;
    mutable juce::CriticalSection stateLock;

    // takes: a WAV writer is kept armed (file already open) so recording starts on the exact sample
    void timerCallback() override;
    void armTake();
    void finishTake();
    juce::TimeSliceThread takeThread { "Super Goat takes" };
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> takeWriter;   // created/destroyed on the message thread
    std::atomic<juce::AudioFormatWriter::ThreadedWriter*> takeArmed { nullptr };
    std::atomic<int> takeState { 3 };      // 0 idle (armed), 1 capturing, 2 finished -> message thread finalises, 3 not armed
    std::atomic<int> takeMode { TakeWhenRecording };
    std::atomic<double> takeStartPpq { 0 }, takeBeatsPerBar { 4 };
    std::atomic<juce::int64> takeSamples { 0 };
    juce::File takePending, lastTake;
    double takeSr = 0;
    int takeCount = 0;

    void applyEvents (const juce::var& v);
    void applySynth (const juce::var& v);
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArpLoomProcessor)
};
