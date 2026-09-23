# 🐐 Super Goat Arpeggiator

*by Goat Bleats*

A piano-roll arpeggiator and synth that runs in your browser. It also runs as a VST3 instrument (tested in Reason 14).

**▶ Play it in the browser: https://deepspace000.github.io/Super-Goat-Arpeggiator/**

Works best in Chrome or Edge. Those have Web MIDI, so you can play it from a MIDI keyboard.

## What it does

- **Piano-roll sequencer** of any length, with optional polyphony.
- **Arp patterns:** up, down, up & down, random and 50-odd more, plus your own. A **mode timeline** switches patterns at points in the song.
- **Drawable lanes:** dynamics, volume, filter, tuning, pan, articulation, glide and rate. Each has shape presets (drifts, random, trends and more).
- **Chord sequence panel:** type chord names, including extended chords and polychords, with voicings, inversions and drop voicings. There's also a chord library you can audition in context.
- **Synth:** two oscillators (classic, chip/console and FM waves), a filter, envelopes, delay and reverb, with many presets.
- **MIDI keyboard input:** live arpeggiation and recording.
- **Export:** 24-bit / 48 kHz WAV, and MIDI. In the MIDI file, the filter lane becomes CC74, pan becomes CC10, and tuning becomes pitch bend.
- **Layout:** the roll sits on the first screen. Scroll down, or press **Look inside the barn**, for all the devices.

## The VST3 plugin (Windows)

The same page runs inside a JUCE plugin (WebView2). The synth is ported to C++, so the plugin plays in sync with your DAW's transport. When the DAW records, the plugin records its own sound as a WAV **take**. Drag the take card onto an audio track.

Build it with Visual Studio 2022 Build Tools (C++ workload) and CMake 3.22 or newer:

```
git clone --recursive https://github.com/Deepspace000/Super-Goat-Arpeggiator.git
cd Super-Goat-Arpeggiator/plugin
powershell -ExecutionPolicy Bypass -File get-deps.ps1
cmake -S . -B build
cmake --build build --config Release --parallel
```

This builds `plugin/build/ArpLoom_artefacts/Release/VST3/Super Goat Arpeggiator.vst3`. Copy it to `C:\Program Files\Common Files\VST3` (this needs admin), then rescan plugins in your DAW. The build targets are named `ArpLoom`, the project's working name.

There's also a crash-test program. Configure with `cmake -S . -B build-test -DARPLOOM_TEST=ON`, then build the `GoatTest` target in Debug. It runs the synth engine the way a DAW does, with odd block sizes, stress and take recording.
