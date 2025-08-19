# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(dvplogger_ext)

# SPIFFSイメージの自動作成（spiffsgenを呼び出す）
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/spiffs.bin
    COMMAND ${PYTHON_EXECUTABLE} ${IDF_PATH}/components/spiffs/spiffsgen.py
            0x1A0000 ${CMAKE_SOURCE_DIR}/spiffs_data ${CMAKE_BINARY_DIR}/spiffs.bin
    DEPENDS ${CMAKE_SOURCE_DIR}/spiffs_data
    COMMENT "Generating SPIFFS image"
)

add_custom_target(spiffs_image ALL DEPENDS ${CMAKE_BINARY_DIR}/spiffs.bin)

add_custom_command(
    POST_BUILD
    OUTPUT ${CMAKE_BINARY_DIR}/binaries.c
    COMMAND ${CMAKE_COMMAND}
        -DINPUT_DIR=${CMAKE_BINARY_DIR}
        -DOUTPUT_FILE=${CMAKE_BINARY_DIR}/binaries.c
        -P ${CMAKE_CURRENT_LIST_DIR}/main/bin2array_runner.cmake
    DEPENDS ${CMAKE_BINARY_DIR}/dvplogger_ext.elf
    COMMENT "Converting bin to C array !"
)

add_custom_target(generate_resources ALL
    DEPENDS ${CMAKE_BINARY_DIR}/binaries.c
)

