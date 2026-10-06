cmake_minimum_required(VERSION 3.20)

# 64-bit by default with the Visual Studio generator unless the caller chose
# a platform explicitly (cmake -A Win32 still works).
if(NOT CMAKE_GENERATOR_PLATFORM AND CMAKE_GENERATOR MATCHES "Visual Studio")
    set(CMAKE_GENERATOR_PLATFORM x64 CACHE STRING "" FORCE)
endif()

project(bootfix VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BOOTFIX_GUI "Build the --gui interface (Dear ImGui, third_party/imgui submodule)" ON)
set(BOOTFIX_HOMEPAGE "https://github.com/Pugnator/bootfix" CACHE STRING "Shown in --help as the home page / bug report address")

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(WARNING "bootfix is meant to be a 64-bit build (got ${CMAKE_SIZEOF_VOID_P}*8-bit)")
endif()

add_executable(bootfix
    src/main.cc
    src/log.cc
    src/args.cc
    src/ntapi.cc
    src/disk.cc
    src/bootsector.cc
    src/bcd.cc
    src/cmd_scan.cc
    src/cmd_bootcode.cc
    src/cmd_bcd.cc
)

target_include_directories(bootfix PRIVATE src)
target_compile_definitions(bootfix PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    _WIN32_WINNT=0x0A00
    "BOOTFIX_HOMEPAGE=L\"${BOOTFIX_HOMEPAGE}\""
)

if(NOT MSVC)
    message(FATAL_ERROR "bootfix builds with MSVC only (Visual Studio 2022 or later)")
endif()
target_compile_options(bootfix PRIVATE /W4 /permissive- /utf-8 /EHsc)
# Static CRT so the exe runs from WinPE / a recovery console with no redist.
set_property(TARGET bootfix PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# asInvoker so help, version and the GUI can run from any shell; commands check elevation themselves.
target_link_options(bootfix PRIVATE /SUBSYSTEM:CONSOLE /MANIFESTUAC:level=asInvoker)

target_link_libraries(bootfix PRIVATE ntdll advapi32 version ole32 shell32)

# --- Dear ImGui GUI ---------------------------------------------------------
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
if(BOOTFIX_GUI AND EXISTS ${IMGUI_DIR}/imgui.cpp)
    set(IMGUI_SOURCES
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
        ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
    )
    target_sources(bootfix PRIVATE src/gui.cc ${IMGUI_SOURCES})
    target_include_directories(bootfix PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends)
    target_compile_definitions(bootfix PRIVATE BOOTFIX_GUI=1 IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
    target_link_libraries(bootfix PRIVATE d3d11 dxgi d3dcompiler dwmapi)
    # Third-party code is not held to our warning level.
    set_source_files_properties(${IMGUI_SOURCES} PROPERTIES COMPILE_OPTIONS "/W3")
    message(STATUS "bootfix: GUI enabled (Dear ImGui at ${IMGUI_DIR})")
elseif(BOOTFIX_GUI)
    message(WARNING "bootfix: third_party/imgui is empty - run `git submodule update --init`; building without --gui")
endif()

install(TARGETS bootfix RUNTIME DESTINATION .)
