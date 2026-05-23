##
# @file CMakeLists.txt
# @brief T5AI speaker beep validation example.
#/

if (CONFIG_ENABLE_AUDIO_CODECS STREQUAL "y")
set(APP_PATH ${CMAKE_CURRENT_LIST_DIR})
get_filename_component(APP_NAME ${APP_PATH} NAME)
aux_source_directory(${APP_PATH}/src APP_SRCS)

add_library(${EXAMPLE_LIB})

target_sources(${EXAMPLE_LIB}
    PRIVATE
        ${APP_SRCS}
)
else()
message(FATAL_ERROR "speaker_beep cannot work when ENABLE_AUDIO_CODECS is not set")
endif()
