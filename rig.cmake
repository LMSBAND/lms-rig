# LMS Rig — included from ysfx's cmake.plugin.txt when LMS_RIG_DIR is set.
#   cmake -B build -DLMS_RIG_DIR=$HOME/lms-rig && cmake --build build --target lms_rig_Standalone

juce_add_plugin(lms_rig
    PLUGIN_CODE "Lrig"
    PLUGIN_MANUFACTURER_CODE "LMSx"
    PRODUCT_NAME "LMS Rig"
    COMPANY_NAME "LMS"
    BUNDLE_ID "com.lms.rig"
    FORMATS Standalone VST3
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    NEEDS_CURL FALSE
    NEEDS_WEB_BROWSER FALSE
    IS_SYNTH FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE)

# the JSFX themselves ship inside the app; extracted to the user's app-data dir on launch
file(GLOB LMS_RIG_JSFX
    "${LMS_RIG_DIR}/jsfx/lms_*_v2.jsfx"
    "${LMS_RIG_DIR}/jsfx/lms_satans_pedalboard.jsfx"
    "${LMS_RIG_DIR}/jsfx/lms_core.jsfx-inc"
    "${LMS_RIG_DIR}/jsfx/shakebot_logo.png")
juce_add_binary_data(lms_rig_jsfx NAMESPACE LmsRigData SOURCES ${LMS_RIG_JSFX})
set_target_properties(lms_rig_jsfx PROPERTIES POSITION_INDEPENDENT_CODE ON)

file(GLOB LMS_RIG_YSFX_SOURCES
    "${PROJECT_SOURCE_DIR}/plugin/*.cpp"
    "${PROJECT_SOURCE_DIR}/plugin/components/*.cpp"
    "${PROJECT_SOURCE_DIR}/plugin/utility/*.cpp")

target_sources(lms_rig
    PRIVATE
        ${LMS_RIG_YSFX_SOURCES}
        "${LMS_RIG_DIR}/rig.cpp"
        "${LMS_RIG_DIR}/rig.h"
        "${LMS_RIG_DIR}/rig_ui.cpp"
        "${LMS_RIG_DIR}/rig_ui.h")

target_compile_definitions(lms_rig
    PUBLIC
        VERSION_STRING="${PROJECT_VERSION}"
        "YSFX_NO_PLUGIN_ENTRY=1"
        "JUCE_WEB_BROWSER=0"
        "JUCE_USE_CURL=0"
        "JUCE_VST3_CAN_REPLACE_VST2=0")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(lms_rig PUBLIC "JUCE_JACK=1")
endif()

target_include_directories(lms_rig
    PRIVATE
        "${PROJECT_SOURCE_DIR}/plugin"
        "${LMS_RIG_DIR}")

target_link_libraries(lms_rig
    PRIVATE
        ysfx::ysfx
        json
        lms_rig_jsfx
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_audio_devices
        juce::juce_gui_basics
        juce::juce_gui_extra
        juce::juce_opengl
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
