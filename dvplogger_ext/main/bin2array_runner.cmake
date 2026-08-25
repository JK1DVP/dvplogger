# bin2array_runner.cmake
cmake_minimum_required(VERSION 3.13)

# 必要な変数を取得（-Dで指定 or 環境変数）
if(NOT DEFINED INPUT_DIR)
    message(FATAL_ERROR "INPUT_DIR not set")
endif()
if(NOT DEFINED OUTPUT_FILE)
    message(FATAL_ERROR "OUTPUT_FILE not set")
endif()

# デバッグ出力（任意）
message(STATUS "Generating ${OUTPUT_FILE} from files in ${INPUT_DIR}")

# 空のCファイル作成
file(WRITE ${OUTPUT_FILE} "#include <stdint.h>\n\n")

# flasher() に内蔵するのは SUBCPU 起動に必要な3ファイルだけ。
# build tree 全体を GLOB_RECURSE すると spiffs.bin, ota_data_initial.bin,
# CMake compiler-test binaries まで binaries.c に混入するので明示指定する。
set(BIN_PATHS
    "${INPUT_DIR}/bootloader/bootloader.bin"
    "${INPUT_DIR}/partition_table/partition-table.bin"
    "${INPUT_DIR}/jk1dvplog_ext.bin"
)

foreach(BIN_FILE IN LISTS BIN_PATHS)
    if(NOT EXISTS "${BIN_FILE}")
        message(FATAL_ERROR
            "Required SUBCPU flasher image not found: ${BIN_FILE}")
    endif()
endforeach()

foreach(BIN_FILE IN LISTS BIN_PATHS)
    # ファイル名部分だけ取得
    file(RELATIVE_PATH REL_NAME "${INPUT_DIR}" "${BIN_FILE}")
    string(REGEX REPLACE "[\\./-]" "_" SYMBOL_NAME "${REL_NAME}")

    # バイナリをHEXとして読み込み
    file(READ "${BIN_FILE}" HEX_CONTENT HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," C_ARRAY "${HEX_CONTENT}")

    # md5sum取得
    execute_process(COMMAND md5sum "${BIN_FILE}" OUTPUT_VARIABLE MD5_LINE)
    string(REGEX REPLACE " .*" "" MD5_HASH "${MD5_LINE}")

    # 追記
    file(APPEND ${OUTPUT_FILE}
        "const uint8_t  ${SYMBOL_NAME}[] = {${C_ARRAY}};\n"
        "const uint32_t ${SYMBOL_NAME}_size = sizeof(${SYMBOL_NAME});\n"
        "const uint8_t  ${SYMBOL_NAME}_md5[] = \"${MD5_HASH}\";\n\n"
    )
endforeach()
