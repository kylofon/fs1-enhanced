cmake_minimum_required(VERSION 3.24)
project(fs1 VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Use an installed SDL3 (3.4 or newer) if there is one; otherwise download and build it,
# which is what headless Linux machines without SDL3 packages need.
find_package(SDL3 3.4 CONFIG QUIET)
if(NOT SDL3_FOUND)
    message(STATUS "SDL3 not found: fetching and building it")
    include(FetchContent)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.4.0
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(SDL3)
endif()

# FS1_EMULATOR (3.22): also build the 8086 interpreter, the emulated PIC/PIT/ports and BIOS,
# the original boot loader, --verify, --trace and --stats counting, for the verification tools
# (tools/verify_campaign.py, trace_campaign.py, insn_stats.py). The default build runs the game
# with the C scheduler only and has no interpreter.
option(FS1_EMULATOR "Build the 8086 interpreter and emulated PC for verification" OFF)

add_executable(fs1
    src/main.c
    src/disk.c
    src/cpu_core.c
    src/pc.c
    src/sched.c
    src/native.c
)
if(FS1_EMULATOR)
    target_sources(fs1 PRIVATE src/cpu8086.c)
    target_compile_definitions(fs1 PRIVATE FS1_EMULATOR=1)
endif()
# One file per native area (phase 3); new files are picked up on the next configure.
file(GLOB NATIVE_AREA_SOURCES CONFIGURE_DEPENDS src/natives/*.c)
target_sources(fs1 PRIVATE ${NATIVE_AREA_SOURCES})
# ENH: the enhancements (PLAN.md), hooked into the engine at ENH: points.
file(GLOB_RECURSE ENHANCED_SOURCES CONFIGURE_DEPENDS src/enhanced/*.c)
target_sources(fs1 PRIVATE ${ENHANCED_SOURCES})
target_compile_definitions(fs1 PRIVATE FS1_VERSION="${PROJECT_VERSION}")
target_include_directories(fs1 PRIVATE src)
set_target_properties(fs1 PROPERTIES OUTPUT_NAME fs1-enhanced)   # ENH: exe name
target_link_libraries(fs1 PRIVATE SDL3::SDL3)

if(MSVC)
    target_compile_options(fs1 PRIVATE /W4)
else()
    target_compile_options(fs1 PRIVATE -Wall -Wextra)
endif()

# ENH: the launcher ("FS1 Enhanced.exe", wxWidgets): the disk image, start location, display and sound options.
option(FS1_LAUNCHER "Also build the launcher, FS1 Enhanced.exe (wxWidgets)" ON)
if(FS1_LAUNCHER)
    add_subdirectory(launcher)
endif()
