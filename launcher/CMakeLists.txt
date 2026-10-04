# The game's launcher, "FS1 Enhanced": the disk image, where to start, the display and sound options, then
# fs1-enhanced.
#
#   cmake -S launcher -B launcher/build -G Ninja && cmake --build launcher/build
#
# or, from the main project, -DFS1_LAUNCHER=ON (the default) builds it next to fs1-enhanced.exe.
# Needs wxWidgets 3.2 (MSYS2: mingw-w64-x86_64-wxwidgets3.2-msw; Debian and Ubuntu: libwxgtk3.2-dev).
cmake_minimum_required(VERSION 3.24)
project(fs1launcher LANGUAGES CXX)
if(WIN32)
    enable_language(RC)
endif()

# MSYS2 and wxWidgets' own CMake builds install a package config; distributions
# such as Debian only ship wx-config, which CMake's FindwxWidgets uses.
find_package(wxWidgets 3.2 CONFIG QUIET COMPONENTS core base)
if(wxWidgets_FOUND AND TARGET wx::wxcore)
    set(FS1L_WX_LIBRARIES wx::wxcore wx::wxbase)
else()
    find_package(wxWidgets 3.2 REQUIRED COMPONENTS core base)
    include(${wxWidgets_USE_FILE})
    set(FS1L_WX_LIBRARIES ${wxWidgets_LIBRARIES})
endif()

add_executable(fs1launcher WIN32
    app.cpp
    game.cpp
    icon.cpp
    launcher.cpp
    settings.cpp
)
set_target_properties(fs1launcher PROPERTIES OUTPUT_NAME "FS1 Enhanced" CXX_EXTENSIONS OFF)
target_compile_features(fs1launcher PRIVATE cxx_std_17)
target_link_libraries(fs1launcher PRIVATE ${FS1L_WX_LIBRARIES})
# Built from the main project: beside fs1-enhanced.exe, where it looks for it.
if(NOT PROJECT_IS_TOP_LEVEL)
    set_target_properties(fs1launcher PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endif()

if(WIN32)
    target_sources(fs1launcher PRIVATE app.rc)
    target_link_libraries(fs1launcher PRIVATE comctl32)
    if(MINGW)
        # The C++ runtime inside the exe, so the build needs fewer DLLs beside it.
        target_link_options(fs1launcher PRIVATE -static-libgcc -static-libstdc++ $<$<CONFIG:Release>:-s>)
    endif()
    # The wxWidgets DLLs and everything they load, found where the compiler's own DLLs are.
    get_filename_component(FS1L_DLL_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    add_custom_command(TARGET fs1launcher POST_BUILD
        COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:fs1launcher> "-DSEARCH=${FS1L_DLL_DIR}"
                -DCMAKE_OBJDUMP=${CMAKE_OBJDUMP} -P ${CMAKE_CURRENT_SOURCE_DIR}/copy_dlls.cmake)
endif()

if(MSVC)
    target_compile_options(fs1launcher PRIVATE /W4 /utf-8)
else()
    target_compile_options(fs1launcher PRIVATE -Wall -Wextra)
endif()
