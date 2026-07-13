idf_component_register(
SRCS "main.cpp" "keyboard.cpp" "mux_transport.cpp" "AudioPlayer.cpp" "dupechk.cpp" "callhist_mem.cpp" "callhist_remote.cpp"
INCLUDE_DIRS ""
)


# Build shared sources for the extension/sub CPU.
target_compile_definitions(${COMPONENT_LIB} PRIVATE DVPLOGGER_EXT)
