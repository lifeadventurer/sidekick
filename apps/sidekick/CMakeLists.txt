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
    ${APP_PATH}/backend_client/sidekick_backend.c
    ${APP_PATH}/audio/sidekick_audio.c
    ${APP_PATH}/ui/sidekick_ui.c
    ${APP_PATH}/tutor/sidekick_session.c
)

set(SIDEKICK_DEVICE_CONFIG_SCRIPT "${APP_PATH}/scripts/gen_sidekick_device_config.py")
set(SIDEKICK_DEVICE_CONFIG_HEADER "${APP_PATH}/include/sidekick_device_config.h")

if(DEFINED ENV{OPEN_SDK_PYTHON} AND NOT "$ENV{OPEN_SDK_PYTHON}" STREQUAL "")
    set(SIDEKICK_PYTHON_CMD $ENV{OPEN_SDK_PYTHON})
else()
    find_program(SIDEKICK_PYTHON_CMD python3)
endif()

if(SIDEKICK_PYTHON_CMD)
    add_custom_command(
        OUTPUT ${SIDEKICK_DEVICE_CONFIG_HEADER}
        COMMAND ${CMAKE_COMMAND} -E env PYTHONIOENCODING=utf-8 PYTHONUNBUFFERED=1
                ${SIDEKICK_PYTHON_CMD} ${SIDEKICK_DEVICE_CONFIG_SCRIPT}
                --app-root ${APP_PATH}
                --output ${SIDEKICK_DEVICE_CONFIG_HEADER}
        DEPENDS ${SIDEKICK_DEVICE_CONFIG_SCRIPT} ${APP_PATH}/.env.example
        COMMENT "Generating sidekick device config from .env.local"
        VERBATIM
    )

    add_custom_target(generate_sidekick_device_config
        DEPENDS ${SIDEKICK_DEVICE_CONFIG_HEADER}
    )
endif()

set(APP_INC
    ${APP_PATH}/include
    ${APP_PATH}/hardware
    ${APP_PATH}/vision
    ${APP_PATH}/backend_client
    ${APP_PATH}/audio
    ${APP_PATH}/ui
    ${APP_PATH}/tutor
)

add_library(${EXAMPLE_LIB})

target_sources(${EXAMPLE_LIB}
    PRIVATE
        ${APP_SRCS}
)

if(SIDEKICK_PYTHON_CMD)
    target_sources(${EXAMPLE_LIB} PRIVATE ${SIDEKICK_DEVICE_CONFIG_HEADER})
    add_dependencies(${EXAMPLE_LIB} generate_sidekick_device_config)
endif()

target_include_directories(${EXAMPLE_LIB}
    PRIVATE
        ${APP_INC}
)
