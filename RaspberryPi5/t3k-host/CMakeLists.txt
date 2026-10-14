cmake_minimum_required(VERSION 3.22)

# Headless host for the TONE3000 plugin processor.
# Compiles the processor sources straight from a tone3000-plugin checkout
# (same recipe as the plugin's own test/CMakeLists.txt, HEADLESS=1) and
# drives it from an ALSA device. No editor, no plugin format, no window.
project(t3k-host VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(T3K_PLUGIN_DIR "" CACHE PATH "Path to a tone3000-plugin checkout (with submodules)")
if (NOT EXISTS "${T3K_PLUGIN_DIR}/plugin/src/Processor.cpp")
    message(FATAL_ERROR "Set -DT3K_PLUGIN_DIR=<tone3000-plugin checkout>")
endif()
if (NOT EXISTS "${T3K_PLUGIN_DIR}/plugin/NeuralAmpModelerCore/NAM/dsp.cpp")
    message(FATAL_ERROR "Submodules missing: git submodule update --init --recursive in ${T3K_PLUGIN_DIR}")
endif()

# Same as the plugin's root CMakeLists.txt: char is unsigned by default on
# ARM, and WDL (AudioDSPTools' resampler) refuses to compile that way.
add_compile_options(-fsigned-char)

# Tune for the CM5 (Cortex-A76). Off for cross-checks on other machines.
option(T3K_HOST_TUNE_CM5 "Add -mcpu=cortex-a76" ON)
if (T3K_HOST_TUNE_CM5 AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    add_compile_options(-mcpu=cortex-a76)
endif()

# Same JUCE version the plugin pins (root CMakeLists.txt, CPMAddPackage).
include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 9.0.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(JUCE)

set(T3K_PLUGIN "${T3K_PLUGIN_DIR}/plugin")
set(NAM_SRC "${T3K_PLUGIN}/NeuralAmpModelerCore/NAM")

# NAM static library, same source list as plugin/CMakeLists.txt.
add_library(NAM STATIC
    ${NAM_SRC}/activations.cpp
    ${NAM_SRC}/conv1d.cpp
    ${NAM_SRC}/container.cpp
    ${NAM_SRC}/convnet.cpp
    ${NAM_SRC}/dsp.cpp
    ${NAM_SRC}/get_dsp.cpp
    ${NAM_SRC}/lstm.cpp
    ${NAM_SRC}/ring_buffer.cpp
    ${NAM_SRC}/util.cpp
    ${NAM_SRC}/wavenet/a2_fast.cpp
    ${NAM_SRC}/wavenet/model.cpp
    ${NAM_SRC}/wavenet/slimmable.cpp
)
target_include_directories(NAM PUBLIC
    ${T3K_PLUGIN}/NeuralAmpModelerCore
    ${T3K_PLUGIN}/NeuralAmpModelerCore/Dependencies/eigen
    ${T3K_PLUGIN}/NeuralAmpModelerCore/Dependencies/nlohmann
)
target_compile_definitions(NAM PUBLIC NAM_ENABLE_A2_FAST)

juce_add_console_app(t3k-host PRODUCT_NAME "t3k-host")

target_sources(t3k-host PRIVATE
    src/main.cpp
    src/display/DisplayUi.cpp
    src/display/SpiLcd.cpp
    src/input/EncoderInput.cpp
    src/input/EncoderReader.cpp
    src/ui/PanelUi.cpp
    ${T3K_PLUGIN}/src/AutoOffset.cpp
    ${T3K_PLUGIN}/src/BlockEq.cpp
    ${T3K_PLUGIN}/src/BlockSpectrum.cpp
    ${T3K_PLUGIN}/src/LegacyParamIds.cpp
    ${T3K_PLUGIN}/src/MidiMapper.cpp
    ${T3K_PLUGIN}/src/NamEngine.cpp
    ${T3K_PLUGIN}/src/PresetFile.cpp
    ${T3K_PLUGIN}/src/PresetManager.cpp
    ${T3K_PLUGIN}/src/Processor.cpp
    ${T3K_PLUGIN}/src/ProcessorChain.cpp
    ${T3K_PLUGIN}/src/ProcessorHistory.cpp
    ${T3K_PLUGIN}/src/ProcessorModelLoader.cpp
    ${T3K_PLUGIN}/src/ProcessorPresets.cpp
    ${T3K_PLUGIN}/src/ProcessorState.cpp
    ${T3K_PLUGIN}/src/Spread.cpp
    ${T3K_PLUGIN}/src/StandaloneStateAutosave.cpp
    ${T3K_PLUGIN}/src/PitchShift.cpp
    ${T3K_PLUGIN}/src/StereoOffset.cpp
    ${T3K_PLUGIN}/src/TunerDetector.cpp
)

target_include_directories(t3k-host PRIVATE
    src
    ${T3K_PLUGIN}/include
    # Gear pictograms (ui/core/GearGlyphs.h, header only).
    ${T3K_PLUGIN}/ui
    ${T3K_PLUGIN}/AudioDSPTools
    ${T3K_PLUGIN}/AudioDSPTools/dsp/ResamplingContainer/Dependencies
)

target_compile_definitions(t3k-host PRIVATE
    HEADLESS=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_WEBP=0
    # No network in the host yet: tones arrive as local files.
    JUCE_USE_CURL=0
    JUCE_ALSA=1
    JUCE_JACK=0
)

target_link_libraries(t3k-host
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_dsp
        juce::juce_gui_basics
        # Whole-archive so NAM's self-registering config parsers survive the
        # linker (same recipe as plugin/CMakeLists.txt).
        -Wl,--whole-archive NAM -Wl,--no-whole-archive
    PUBLIC
        juce::juce_recommended_config_flags
)

add_dependencies(t3k-host NAM)

# Version for --version and the splash screen, with the plugin commit the
# host was built against. Only on the two files that show it, so a version
# change does not rebuild the plugin sources.
execute_process(
    COMMAND git -C "${T3K_PLUGIN_DIR}" rev-parse --short HEAD
    OUTPUT_VARIABLE T3K_PLUGIN_REF
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if (NOT T3K_PLUGIN_REF)
    set(T3K_PLUGIN_REF "unknown")
endif()
set_source_files_properties(src/main.cpp src/display/DisplayUi.cpp PROPERTIES COMPILE_DEFINITIONS
    "T3K_HOST_VERSION=\"${PROJECT_VERSION}\";T3K_PLUGIN_REF=\"${T3K_PLUGIN_REF}\""
)

# Display wiring/panel check; plain C++, no JUCE.
add_executable(lcd-test tools/lcd-test.cpp src/display/SpiLcd.cpp)
target_include_directories(lcd-test PRIVATE src)

# Encoder wiring check; plain C++, no JUCE.
add_executable(enc-test tools/enc-test.cpp src/input/EncoderReader.cpp)
target_include_directories(enc-test PRIVATE src)
