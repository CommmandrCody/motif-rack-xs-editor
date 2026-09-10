cmake_minimum_required(VERSION 3.21)
project(motif-xs VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Apple Silicon native")

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

    juce_add_gui_app(motif-xs-app
        PRODUCT_NAME "Motif Rack XS"
        COMPANY_NAME "motifxs"
        BUNDLE_ID "com.motifxs.app"
        MICROPHONE_PERMISSION_ENABLED FALSE)

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
        juce::juce_gui_basics
        juce::juce_gui_extra
        PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags)
else()
    message(STATUS "external/JUCE not present -- skipping the standalone app")
endif()
