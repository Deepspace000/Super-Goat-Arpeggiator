cmake_minimum_required(VERSION 3.22)
project(ArpLoom VERSION 0.1.0)

# WebView2 SDK (NuGet package unpacked into ./deps)
set(JUCE_WEBVIEW2_PACKAGE_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/deps" CACHE PATH "WebView2 package folder")

add_subdirectory(JUCE)

juce_add_plugin(ArpLoom
    COMPANY_NAME "Goat Bleats"
    PLUGIN_MANUFACTURER_CODE Nflr
    PLUGIN_CODE ArLm
    FORMATS VST3 Standalone
    PRODUCT_NAME "Super Goat Arpeggiator"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    NEEDS_WEBVIEW2 TRUE
    VST3_CATEGORIES Instrument Synth)

# The web interface is the same arp-loom.html the browser version uses
juce_add_binary_data(ArpLoomData SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../arp-loom.html")

target_sources(ArpLoom PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/SynthEngine.cpp)

target_compile_features(ArpLoom PRIVATE cxx_std_17)

target_compile_definitions(ArpLoom PUBLIC
    JUCE_WEB_BROWSER=1
    JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(ArpLoom
    PRIVATE
        ArpLoomData
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Crash-test harness (off by default): runs the processor like a DAW would, in a Debug build with checks on.
option(ARPLOOM_TEST "Build the GoatTest harness" OFF)
if(ARPLOOM_TEST)
    juce_add_console_app(GoatTest PRODUCT_NAME "GoatTest" NEEDS_WEBVIEW2 TRUE)
    target_sources(GoatTest PRIVATE test/GoatTest.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SynthEngine.cpp)
    target_compile_features(GoatTest PRIVATE cxx_std_17)
    target_compile_definitions(GoatTest PRIVATE JucePlugin_Name="Super Goat Arpeggiator" JucePlugin_IsSynth=1 JucePlugin_WantsMidiInput=1
        JucePlugin_ProducesMidiOutput=0 JucePlugin_IsMidiEffect=0 JUCE_WEB_BROWSER=1 JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(GoatTest PRIVATE ArpLoomData juce::juce_audio_utils juce::juce_dsp juce::juce_gui_extra juce::juce_recommended_config_flags)
endif()
