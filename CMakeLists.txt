cmake_minimum_required(VERSION 3.21)
project(motif-xs LANGUAGES C CXX)

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
