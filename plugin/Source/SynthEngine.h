#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <vector>
#include <memory>
#include <array>

// C++ port of Arp Loom's Web Audio synth (voice(), buildFX(), makeIR()).

namespace arploom
{

//==============================================================================
enum Wave { W_SINE, W_TRI, W_SAW, W_SQUARE, W_PULSE25, W_PULSE12, W_GLASS, W_ORGAN, W_HOLLOW,
            W_PULSE40, W_SID_COMBO, W_NES_TRI, W_GB_WAVE, W_GB_SAW, W_BIT_SINE, W_YM_BUZZ, W_YM_SAW,
            W_FM_BRASS, W_FM_LEAD, W_FM_ORGAN, W_FM_HOLLOW, W_FM_BASS, W_COUNT };

int waveFromName (const juce::String& name);

// Band-limited single-cycle tables, one mip level per halving of the allowed harmonics.
class Wavetables
{
public:
    static constexpr int tableSize = 2048;
    static constexpr int numLevels = 10;   // 512, 256, ... 1 harmonics
    static const Wavetables& get();
    // returns a table whose harmonics stay below Nyquist for this frequency
    const float* table (int wave, double freqHz, double sampleRate) const;
private:
    Wavetables();
    std::vector<float> data;   // [wave][level][tableSize]
};

//==============================================================================
struct SynthParams
{
    struct Osc { int wave = W_TRI; int oct = 0; int semi = 0; float det = 0, lvl = .7f; int uni = 1; float spread = 0, width = 0; };
    struct Env { float a = .01f, d = .5f, s = .5f, r = 1.f; };
    Osc osc1, osc2;
    float sub = 0, noise = 0;
    int filtType = 0;          // 0 lowpass, 1 highpass, 2 bandpass
    float cut = 2600, res = 1.2f, env = 1.5f, key = .3f;
    Env amp, fenv;
    float delayBeats = .75f, delayFb = .35f, delayMix = .22f;
    float revSize = 3.5f, revMix = .3f;
    float vol = .8f;

    static SynthParams fromVar (const juce::var& v);   // S.synth JSON from the page
};

//==============================================================================
struct NoteSpec
{
    int pitch = 60;
    float vel = 100;
    double durSec = .3;            // < 0 = held until noteOff (live keyboard)
    float filterMul = 1.f;
    bool hasPan = false; float pan0 = 0, pan1 = 0;
    int glideFrom = -1; double glideSec = 0;
    std::vector<std::pair<double, float>> tune;   // (seconds from start, cents)
    bool tuneSteps = false;
    std::shared_ptr<const SynthParams> params;     // null = engine's current params
    bool live = false;             // keyboard-held note (for noteOff / sustain)
};

//==============================================================================
class Voice
{
public:
    void start (const NoteSpec& spec, const SynthParams& p, double sampleRate, int startDelaySamples);
    void release();                 // note-off (live notes)
    void quickStop();               // fast fade (transport stop / panic)
    bool isActive() const noexcept  { return active; }
    bool isLive() const noexcept    { return spec.live; }
    int  pitch() const noexcept     { return spec.pitch; }
    int64_t age() const noexcept    { return ageSamples; }
    void render (float* L, float* R, int numSamples, float bendCents);
    bool sustained = false;
private:
    bool active = false;
    NoteSpec spec;
    SynthParams P;
    double sr = 48000;
    int delay = 0;                  // samples before the note starts
    int64_t ageSamples = 0, offAt = -1, killAt = -1;
    // oscillators: up to 7 unison per osc
    std::array<double, 7> ph1 {}, ph2 {};
    double phSub = 0;
    uint32_t noiseSeed = 1;
    // envelopes
    enum Stage { Attack, Decay, Release };
    Stage aStage = Attack, fStage = Attack;
    float aLev = 0, fLev = 0;
    float fadeMul = 1.f, fadeStep = 0;
    // filter
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double zL1 = 0, zL2 = 0, zR1 = 0, zR2 = 0;
    int coefCountdown = 0;
    double baseCut = 1000, peakCut = 1000, freqHz = 440;
    void updateFilter (double cutHz);
    void stepEnv (Stage& st, float& lev, const SynthParams::Env& e, bool releasing);
};

//==============================================================================
class Engine
{
public:
    void prepare (double sampleRate, int maxBlock);
    void setParams (std::shared_ptr<const SynthParams> p);
    std::shared_ptr<const SynthParams> currentParams() const { return std::atomic_load (&params); }
    void setTempo (double bpm) { tempo = bpm; }

    void startNote (const NoteSpec& spec, int startDelaySamples);
    void noteOffLive (int pitch);
    void sustainPedal (bool down);
    void setBend (float cents) { bendCents = cents; }
    void stopSequenced();            // quick-release everything that isn't a held key
    void panic();

    void render (juce::AudioBuffer<float>& out, int start, int num);
private:
    static constexpr int maxVoices = 64;
    std::array<Voice, maxVoices> voices;
    std::shared_ptr<const SynthParams> params = std::make_shared<SynthParams>();
    double sr = 48000, tempo = 120;
    float bendCents = 0;
    bool sustain = false;

    // FX
    juce::AudioBuffer<float> voiceBus, revSend;
    std::vector<float> dlyL, dlyR; int dlyW = 0;
    double toneB[3] {}, toneA[3] {}; double tzL1 = 0, tzL2 = 0, tzR1 = 0, tzR2 = 0;
    juce::dsp::Convolution convolution;
    float irSize = -1;
    juce::dsp::Compressor<float> comp;
    void rebuildIR (float size);
    void renderChunk (juce::AudioBuffer<float>& out, int start, int num);
};

} // namespace arploom
