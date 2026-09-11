cmake_minimum_required(VERSION 3.21)

# This MUST come before project(). project() populates CMAKE_OSX_ARCHITECTURES
# in the cache, after which a non-FORCE set() here is silently a no-op -- and an
# empty value makes CMake build for its own architecture. The Homebrew cmake on
# this machine is an x86_64 binary running under Rosetta, so the whole project
# quietly built Intel on an Apple Silicon Mac.
if(APPLE AND NOT DEFINED CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal: Apple Silicon and Intel")
endif()

project(motif-xs VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The core knows nothing about JUCE, VST or any UI, so the CLI, the plugin and
# the tests can all link it. See docs/vst-architecture.md.
add_library(motif-xs-core STATIC
    motif-xs-core/src/sysex.cpp
    motif-xs-core/src/parameters.cpp
    motif-xs-core/src/parameter_table.cpp
    motif-xs-core/src/catalog.cpp
    motif-xs-core/src/catalog_table.cpp
    motif-xs-core/src/device.cpp
    motif-xs-core/src/worker.cpp
    motif-xs-core/src/state.cpp
)
target_include_directories(motif-xs-core PUBLIC motif-xs-core/include)

if(APPLE)
    target_link_libraries(motif-xs-core PUBLIC
        "-framework CoreMIDI" "-framework CoreFoundation" "-framework CoreAudio")
endif()

add_executable(motifxs motif-xs-cli/src/main.cpp)
target_link_libraries(motifxs PRIVATE motif-xs-core)

enable_testing()
add_executable(motif-xs-tests tests/test_sysex.cpp)
target_link_libraries(motif-xs-tests PRIVATE motif-xs-core)
add_test(NAME core COMMAND motif-xs-tests)

# ---------------------------------------------------------------------------
# Standalone GUI (milestone 2). JUCE is used only here and in the plugin --
# motif-xs-core stays free of it.
# ---------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/JUCE/CMakeLists.txt")
    add_subdirectory(external/JUCE)

    # The mark is the drawbar registration of "1972 AS1": eight elements, one
    # per footage, at the levels the element editor shows as faders.
    set(MOTIFXS_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.png")

    juce_add_gui_app(motif-xs-app
        PRODUCT_NAME "Motif Rack XS"
        COMPANY_NAME "motifxs"
        BUNDLE_ID "com.motifxs.app"
        ICON_BIG "${MOTIFXS_ICON}"
        ICON_SMALL "${MOTIFXS_ICON}"
        # The scope reads an audio input, which macOS gates behind the
        # microphone permission even for a line input on an interface.
        MICROPHONE_PERMISSION_ENABLED TRUE
        MICROPHONE_PERMISSION_TEXT "Motif Rack XS shows the rack's audio on its SCOPE page.")

    target_sources(motif-xs-app PRIVATE
        motif-xs-app/Main.cpp
        motif-xs-app/MainComponent.cpp)

    target_include_directories(motif-xs-app PRIVATE motif-xs-app)

    target_compile_definitions(motif-xs-app PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:motif-xs-app,JUCE_PRODUCT_NAME>"
        JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:motif-xs-app,JUCE_VERSION>")

    target_link_libraries(motif-xs-app PRIVATE
        motif-xs-core
        juce::juce_audio_devices
        juce::juce_audio_utils
        juce::juce_gui_basics
        juce::juce_gui_extra
        juce::juce_dsp
        juce::juce_data_structures
        PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags)
    # -----------------------------------------------------------------------
    # VST3 / AU plugin (milestone 3). Shares MainComponent with the standalone
    # app; the connection is injected, so both hosts drive the same UI.
    # -----------------------------------------------------------------------
    juce_add_plugin(motif-xs-plugin
        PRODUCT_NAME "Motif Rack XS"
        COMPANY_NAME "motifxs"
        BUNDLE_ID "com.motifxs.plugin"
        PLUGIN_MANUFACTURER_CODE Mtfx
        PLUGIN_CODE Mrxs
        FORMATS VST3 AU
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT TRUE
        EDITOR_WANTS_KEYBOARD_FOCUS TRUE
        COPY_PLUGIN_AFTER_BUILD TRUE
        ICON_BIG "${MOTIFXS_ICON}"
        ICON_SMALL "${MOTIFXS_ICON}"
        MICROPHONE_PERMISSION_ENABLED FALSE)

    target_sources(motif-xs-plugin PRIVATE
        motif-xs-plugin/PluginProcessor.cpp
        motif-xs-plugin/PluginEditor.cpp
        motif-xs-app/MainComponent.cpp)

    target_include_directories(motif-xs-plugin PRIVATE motif-xs-app motif-xs-plugin)

    target_compile_definitions(motif-xs-plugin PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)

    target_link_libraries(motif-xs-plugin PRIVATE
        motif-xs-core
        juce::juce_audio_utils
        juce::juce_gui_extra
        juce::juce_dsp
        PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags)
else()
    message(STATUS "external/JUCE not present -- skipping the standalone app")
endif()
