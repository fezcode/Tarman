cmake_minimum_required(VERSION 3.20)
project(tarman C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(NOT WIN32)
  message(FATAL_ERROR "Tarman is a Windows task manager; build it with MSYS2 MinGW-w64.")
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif()

# --- version ------------------------------------------------------------
# src/version.h owns the version string; parse it here so the Windows
# VERSIONINFO resource cannot drift from what `tarman --version` prints.
set(TARMAN_VERSION_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/src/version.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${TARMAN_VERSION_HEADER}")
file(STRINGS "${TARMAN_VERSION_HEADER}" _tarman_version_line
     REGEX "^#define[ \t]+TARMAN_VERSION[ \t]+\"[0-9]+\\.[0-9]+\\.[0-9]+\"")
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" TARMAN_VERSION "${_tarman_version_line}")
if(NOT TARMAN_VERSION)
  message(FATAL_ERROR "could not parse TARMAN_VERSION out of ${TARMAN_VERSION_HEADER}")
endif()
string(REPLACE "." ";" _tarman_version_parts "${TARMAN_VERSION}")
list(GET _tarman_version_parts 0 TARMAN_VERSION_MAJOR)
list(GET _tarman_version_parts 1 TARMAN_VERSION_MINOR)
list(GET _tarman_version_parts 2 TARMAN_VERSION_PATCH)
message(STATUS "Tarman version: ${TARMAN_VERSION}")

# --- raylib from MSYS2 mingw64 -------------------------------------------
# libraylib.a is static, but MSYS2 compiled it against the glfw DLL (its
# objects reference __imp_glfw*), so glfw links through its import library
# and glfw3.dll is bundled next to the exe by the POST_BUILD step below.
find_library(RAYLIB_STATIC NAMES libraylib.a    REQUIRED)
find_library(GLFW_IMPORT   NAMES libglfw3.dll.a REQUIRED)

add_executable(tarman WIN32
  src/main.c
  src/common.c
  src/ui.c
  src/sys_win.c
  src/page_proc.c
  src/page_perf.c
  src/page_analysis.c
  src/page_res.c
  src/page_sys.c
  src/page_firmware.c
  src/sys_firmware.c
  src/sys_window.c
  src/menubar.c
  src/hoswl.c
  src/page_events.c
  src/sys_events.c
)

target_include_directories(tarman PRIVATE src)
target_link_libraries(tarman PRIVATE
  ${RAYLIB_STATIC} ${GLFW_IMPORT}
  opengl32 gdi32 winmm
  # sampler & actions: process/perf APIs, PDH counters, IP helper, sessions,
  # SCM, shell, COM (shortcuts, DXGI), DWM title bar, minidumps, file versions
  psapi pdh iphlpapi ws2_32 wtsapi32 advapi32 shell32 ole32 oleaut32 uuid wbemuuid tbs wevtapi dwmapi dbghelp version user32
)
target_link_options(tarman PRIVATE -static-libgcc
  # PE hardening: relocatable, full 64-bit ASLR, DEP-compatible.
  -Wl,--dynamicbase -Wl,--high-entropy-va -Wl,--nxcompat)

# The installer ships build/LICENSE.txt: Forge bundles the license step's own
# file as display-only and skips a [[files]] entry with the same source path.
configure_file(LICENSE.txt "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt" COPYONLY)

# Bundle the MinGW runtime DLLs (glfw3.dll and whatever it pulls in) next to
# the exe so build\ is self-contained -- no PATH dependency on MSYS2.
get_filename_component(MINGW_BIN_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
add_custom_command(TARGET tarman POST_BUILD
  COMMAND ${CMAKE_COMMAND}
    "-DEXE=$<TARGET_FILE:tarman>"
    "-DOUT_DIR=$<TARGET_FILE_DIR:tarman>"
    "-DMINGW_BIN=${MINGW_BIN_DIR}"
    -P "${CMAKE_SOURCE_DIR}/cmake/CopyRuntimeDeps.cmake"
  COMMENT "Bundling MinGW runtime DLLs"
  VERBATIM)

# Icon, manifest (per-monitor DPI, common controls v6, asInvoker) and a
# VERSIONINFO block generated from TARMAN_VERSION.
# resources/tarman.ico is rendered by tools/make-logo.ps1 (committed); a build
# without it still works, just without the icon and manifest resources.
set(TARMAN_RES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources")
if(EXISTS "${TARMAN_RES_DIR}/tarman.ico")
  enable_language(RC)
  configure_file(resources/tarman.rc.in "${CMAKE_CURRENT_BINARY_DIR}/tarman.rc" @ONLY)
  target_sources(tarman PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/tarman.rc")
  # windres does not report the files it embeds, so without this an updated
  # icon never reaches the exe (it kept the very first icon for a while).
  set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/tarman.rc" PROPERTIES
    OBJECT_DEPENDS "${TARMAN_RES_DIR}/tarman.ico;${TARMAN_RES_DIR}/tarman.manifest")
else()
  message(WARNING "resources/tarman.ico missing; building without icon/manifest resources")
endif()
