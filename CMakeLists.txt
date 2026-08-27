cmake_minimum_required(VERSION 3.20)
project(cvisor C)

# cvisor is Linux x86-64 only (src/cvisor.h enforces this with #error).
# On macOS this file exists so CLion can index the sources; build with the
# Makefile inside a Linux x86-64 environment instead (see README.md).
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(WARNING "cvisor builds only on Linux x86-64; use `make` there.")
endif()

set(CMAKE_C_STANDARD 11)

add_executable(cvisor
        src/main.c
        src/analyzer.c
        src/recorder.c
        src/trace.c
        src/tui.c)

find_library(NCURSES_LIB ncurses)
if(NCURSES_LIB)
    target_link_libraries(cvisor ${NCURSES_LIB})
endif()

# separate target so CLion indexes the example program (it has its own
# main(), so it must NOT be compiled into the cvisor binary above)
add_executable(ch5_p1 ch5/p1.c)
