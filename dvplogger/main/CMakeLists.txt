# During ESP-IDF's early component-requirements scan this CMakeLists.txt is
# evaluated from a temporary build-side script.  Do not run generators there.
if(CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_register()
    return()
endif()

set(COMPRESSED_BINARIES_C "${CMAKE_CURRENT_BINARY_DIR}/binaries_lzss.c")
set(BINARIES_LZSS_TOOL "${COMPONENT_DIR}/../tools/compress_binaries_lzss.py")
set(BINARIES_SOURCE_C "${COMPONENT_DIR}/binaries.c")

add_custom_command(
    OUTPUT "${COMPRESSED_BINARIES_C}"
    COMMAND "${PYTHON}" "${BINARIES_LZSS_TOOL}"
            --input "${BINARIES_SOURCE_C}"
            --output "${COMPRESSED_BINARIES_C}"
    DEPENDS "${BINARIES_SOURCE_C}" "${BINARIES_LZSS_TOOL}"
    COMMENT "Generating compressed embedded flasher binaries"
    VERBATIM
)
set_source_files_properties("${COMPRESSED_BINARIES_C}" PROPERTIES GENERATED TRUE)

idf_component_register(
SRCS "morse_decoder_simple.cpp"
SRCS "antenna.cpp"
SRCS "mux_transport.cpp"
SRCS "web_server.cpp"
SRCS "callhist_fd.cpp"
SRCS "multi_acag.cpp"
SRCS "multi_arrl.cpp"
SRCS "main.cpp"
SRCS "cardkey.cpp"
SRCS "multi.cpp"
SRCS "cmd_interp.cpp"
SRCS "variables.cpp"
SRCS "sd_files.cpp"
SRCS "cw_keying.cpp"
SRCS "cat.cpp"
SRCS "usb_host.cpp"
SRCS "uac_pcm2901.cpp"
SRCS "usb_cat_transport.cpp"
SRCS "../components/arduino/libraries/USB_Host_Shield_2.0-1.7.0/cp2105.cpp"
SRCS "keyboard.cpp"
SRCS "so2r.cpp"
SRCS "display.cpp"
SRCS "i2c_guard.cpp"
SRCS "log.cpp"
SRCS "edit_buf.cpp"
SRCS "dupechk.cpp"
SRCS "qso.cpp"
SRCS "callhist.cpp"
SRCS "callhist_pkb.cpp"
SRCS "packed_string.cpp"
SRCS "callhist_remote.cpp"
SRCS "callhist_mem.cpp"
SRCS "cluster.cpp"
SRCS "bandmap.cpp"
SRCS "iambic_keyer.cpp"
SRCS "multi_process.cpp"
SRCS "mcp.cpp"
SRCS "mcp_interface.cpp"
SRCS "ui.cpp"
SRCS "Plan13.cpp"
SRCS "misc.cpp"
SRCS "settings.cpp"
SRCS "processes.cpp"
SRCS "Plan13.cpp"
SRCS "contest.cpp"
SRCS "user_contest_md.cpp"
SRCS "cp932_utf8.cpp"
SRCS "console.cpp"
SRCS "tcp_server.cpp"
SRCS "network.cpp"
SRCS "network_arena.cpp"
SRCS "timekeep.cpp"
SRCS "zserver.cpp"
SRCS "ringbuf.cpp"
SRCS "satellite.cpp"
SRCS "dac-adc.cpp"
SRCS "cty.cpp"
SRCS "cty_chk.cpp"
SRCS "esp32_flasher.c"
SRCS "sd_wrapper.cpp"
SRCS "${COMPRESSED_BINARIES_C}"
INCLUDE_DIRS ""
INCLUDE_DIRS "../components/esp-serial-flasher/examples/common/"
REQUIRES mbedtls nvs_flash
)

# AudioPlayer is a SUBCPU feature on HW1/Mini.  Do not link it into MAIN:
# the implementation carries several KiB of permanent audio/I2S buffers.
if(NOT "${JK1DVPLOG_HWVER}" STREQUAL "1")
    target_sources(${COMPONENT_LIB} PRIVATE "AudioPlayer.cpp")
endif()
# USE_SD 定義を追加
#target_compile_definitions(${COMPONENT_LIB} PRIVATE USE_SD)
target_compile_definitions(
    ${COMPONENT_LIB}
    PRIVATE
        USE_SD
        JK1DVPLOG_HWVER=${JK1DVPLOG_HWVER}
)
