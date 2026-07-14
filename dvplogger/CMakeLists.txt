# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

if(NOT DEFINED JK1DVPLOG_HWVER)
    message(FATAL_ERROR
        "Specify -DJK1DVPLOG_HWVER=1 or 3")
endif()

if(NOT JK1DVPLOG_HWVER STREQUAL "1"
   AND NOT JK1DVPLOG_HWVER STREQUAL "3")
    message(FATAL_ERROR
        "JK1DVPLOG_HWVER must be 1 or 3")
endif()

set(EXTRA_COMPONENT_DIRS components/esp-serial-flasher)

project(dvplogger-hw${JK1DVPLOG_HWVER})

