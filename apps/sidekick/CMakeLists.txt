##
# @file CMakeLists.txt
# @brief Sidekick AI tutor app for TuyaOpen.
#/

set(APP_PATH ${CMAKE_CURRENT_LIST_DIR})
get_filename_component(APP_NAME ${APP_PATH} NAME)

set(APP_SRCS
    ${APP_PATH}/src/sidekick_main.c
    ${APP_PATH}/hardware/sidekick_hardware.c
    ${APP_PATH}/vision/sidekick_camera.c
    ${APP_PATH}/audio/sidekick_audio.c
    ${APP_PATH}/ui/sidekick_ui.c
    ${APP_PATH}/tutor/sidekick_session.c
)

set(APP_INC
    ${APP_PATH}/include
    ${APP_PATH}/hardware
    ${APP_PATH}/vision
    ${APP_PATH}/audio
    ${APP_PATH}/ui
    ${APP_PATH}/tutor
)

add_library(${EXAMPLE_LIB})

target_sources(${EXAMPLE_LIB}
    PRIVATE
        ${APP_SRCS}
)

target_include_directories(${EXAMPLE_LIB}
    PRIVATE
        ${APP_INC}
)
