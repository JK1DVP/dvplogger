set(DVP_EXT_SRCS
    "main.cpp"
    "keyboard.cpp"
    "mux_transport.cpp"
    "dupechk.cpp"
    "packed_string.cpp"
    "callhist_mem.cpp"
    "callhist_remote.cpp"
)

# Keep HW1 SUBCPU lean: keyboard/MUX/DUPE remain, audio playback is HW3-only.
if(NOT JK1DVPLOG_HWVER EQUAL 1)
    list(APPEND DVP_EXT_SRCS "AudioPlayer.cpp")
endif()

idf_component_register(
    SRCS ${DVP_EXT_SRCS}
    INCLUDE_DIRS ""
)


# Build shared sources for the extension/sub CPU.
#target_compile_definitions(${COMPONENT_LIB} PRIVATE DVPLOGGER_EXT)
target_compile_definitions(
    ${COMPONENT_LIB}
    PRIVATE
        DVPLOGGER_EXT
        JK1DVPLOG_HWVER=${JK1DVPLOG_HWVER}
)
