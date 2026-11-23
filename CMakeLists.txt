cmake_minimum_required(VERSION 3.25)
project(DiscCue360 C)

add_executable(default main.c toc.c drive.c metadata.c player.c ui.c)
set_target_properties(default PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)
target_compile_options(default PRIVATE -O2 -Wall -Wextra -Werror)
xsdk_add_xex(default TITLE_ID 0x44435545 NAME "DiscCue 360" VERSION 0.6.1.0)

# The stock title loader checks this header privilege before allowing direct LAN sockets.
set(xex_configurator "${CMAKE_CURRENT_SOURCE_DIR}/configure_xex.py")
set_property(TARGET default APPEND PROPERTY LINK_DEPENDS "${xex_configurator}")
add_custom_command(TARGET default POST_BUILD
    COMMAND python3 "${xex_configurator}" "$<TARGET_FILE_DIR:default>/default.xex"
    VERBATIM)

# Build the opt-in stock-dashboard plugin alongside the standalone player.
add_custom_command(OUTPUT "${XSDK_ROOT}/build/libxsdk_plugin.a"
    COMMAND make -C "${XSDK_ROOT}" "TOOLCHAIN=${XSDK_PREFIX}" build/libxsdk_plugin.a
    VERBATIM)
add_library(PlayCD OBJECT dashboard.c)
target_compile_options(PlayCD PRIVATE -O2 -Wall -Wextra -Werror)
xsdk_add_plugin(PlayCD BASE 0x91440000)
