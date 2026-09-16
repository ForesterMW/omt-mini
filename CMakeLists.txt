cmake_minimum_required(VERSION 3.16)
project(omt-mini CXX)

set(OMTMINI_VERSION "0.2.0" CACHE STRING "Version stamped into the binaries")
string(REPLACE "." "," OMTMINI_VERSION_COMMA_BASE "${OMTMINI_VERSION}")
set(OMTMINI_VERSION_COMMA "${OMTMINI_VERSION_COMMA_BASE},0")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    message(FATAL_ERROR
        "OMT Mini is a Windows application. Build with MSVC, or cross compile "
        "from Linux with scripts/cross-build.sh (mingw-w64).")
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Vendored libomt header (MIT). The matching DLLs are fetched by
# scripts/fetch-omt.sh and are not committed to this repository.
set(OMT_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/external/omt")
if(NOT EXISTS "${OMT_INCLUDE_DIR}/libomt.h")
    message(FATAL_ERROR "external/omt/libomt.h is missing.")
endif()

add_compile_definitions(
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    OMTMINI_VERSION="${OMTMINI_VERSION}"
    OMTMINI_VERSION_W=L"${OMTMINI_VERSION}"
)

if(MSVC)
    add_compile_options(/W3 /permissive- /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
    # No libgcc/libstdc++ runtime next to the executable.
    add_link_options(-static-libgcc -static-libstdc++ -static)
    if(MINGW)
        # wWinMain rather than WinMain, and UTF-16 argv.
        add_compile_options(-municode)
        add_link_options(-municode)
    endif()
endif()

configure_file("${CMAKE_SOURCE_DIR}/res/omtmini_version.h.in"
               "${CMAKE_BINARY_DIR}/generated/omtmini_version.h" @ONLY)
include_directories("${CMAKE_BINARY_DIR}/generated")

# --------------------------------------------------------------- application
set(OMTMINI_SOURCES
    src/main.cpp
    src/app.cpp
    src/util.cpp
    src/omt.cpp
    src/settings.cpp
    src/discovery.cpp
    src/gfx.cpp
    src/ui.cpp
    src/window.cpp
    src/viewer.cpp
    src/sourceswin.cpp
    src/settingswin.cpp
    src/capture.cpp
    src/webcam.cpp
    src/audiomon.cpp
    src/audiocap.cpp
    src/uninstall.cpp
    src/update.cpp
)

add_executable(OMTMini WIN32 ${OMTMINI_SOURCES} res/omtmini.rc)
target_include_directories(OMTMini PRIVATE "${OMT_INCLUDE_DIR}" src)
target_link_libraries(OMTMini PRIVATE
    d3d11 dxgi d2d1 dwrite
    ole32 oleaut32 uuid
    shell32 shlwapi user32 gdi32 advapi32
    dwmapi winmm avrt
    winhttp bcrypt
)
set_target_properties(OMTMini PROPERTIES OUTPUT_NAME "OMTMini")

# ------------------------------------------------------------ virtual camera
add_library(omtmini_vcam SHARED vcam/vcam.cpp res/vcam.rc)
target_include_directories(omtmini_vcam PRIVATE src)
target_link_libraries(omtmini_vcam PRIVATE strmiids ole32 oleaut32 uuid advapi32)
set_target_properties(omtmini_vcam PROPERTIES
    PREFIX ""
    OUTPUT_NAME "omtmini_vcam"
)
# The module definition file is what exports DllGetClassObject and friends.
# CMake hands a .def source to both the MSVC and the mingw linker, and without
# it the DLL has no exports and cannot be registered.
target_sources(omtmini_vcam PRIVATE vcam/vcam.def)

# ------------------------------------------------------------------ installer
add_executable(omtmini-setup WIN32 installer/installer.cpp res/omtmini.rc)
target_include_directories(omtmini-setup PRIVATE src)
target_link_libraries(omtmini-setup PRIVATE shell32 shlwapi ole32 user32 advapi32 comctl32)
set_target_properties(omtmini-setup PROPERTIES OUTPUT_NAME "OMTMini-Setup")

