# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.21)

# macOS: 11.0 (Big Sur) is the oldest system with an arm64 slice, and has
# everything the Apple TLS backend uses. Set before project() so it reaches
# the compiler checks and the vcpkg ports.
if(CMAKE_HOST_APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET AND NOT DEFINED ENV{MACOSX_DEPLOYMENT_TARGET})
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Oldest macOS the build runs on")
endif()

project(em34xx
  VERSION 0.2.1
  DESCRIPTION "Hardware emulator for IBM 34xx-family 5250 terminals"
  LANGUAGES CXX)

# C++17 is the deliberate ceiling: it is the newest standard that every
# toolchain still able to target Windows 7 (x86) implements completely.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(EM34XX_BUILD_TESTS "Build unit tests and the CPU test-vector runner" OFF)
option(EM34XX_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# Exploit mitigations for the project's own targets (GCC and Clang, MinGW
# included); the vcpkg ports are built separately and are not affected.
#   - stack protector, at compile and at link time: the driver then adds the
#     runtime support itself (static libssp or libmingwex on MinGW, libc
#     elsewhere), so no DLL is pulled in under -static
#   - _FORTIFY_SOURCE=2 in optimised builds, unless the compiler already
#     defines it (distro default), the user set it, or a sanitizer is on
#   - ELF: full RELRO.  PE: ASLR (with the base relocations it needs), DEP
#     and, on x64, high-entropy ASLR, spelled out rather than left to the
#     binutils defaults
option(EM34XX_HARDENING "Build with exploit mitigations (stack protector, fortify, RELRO/ASLR/DEP)" ON)
if(EM34XX_HARDENING AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC)
  add_compile_options(-fstack-protector-strong)
  add_link_options(-fstack-protector-strong)
  if(NOT "${CMAKE_CXX_FLAGS} $ENV{CPPFLAGS}" MATCHES "_FORTIFY_SOURCE|-fsanitize")
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS -O2)
    check_cxx_source_compiles("#ifdef _FORTIFY_SOURCE\n#error predefined\n#endif\nint main() { return 0; }"
      EM34XX_FORTIFY_NOT_PREDEFINED)
    unset(CMAKE_REQUIRED_FLAGS)
    if(EM34XX_FORTIFY_NOT_PREDEFINED)
      add_compile_definitions($<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:_FORTIFY_SOURCE=2>)
    endif()
  endif()
  if(CMAKE_EXECUTABLE_FORMAT STREQUAL "ELF")
    add_link_options(LINKER:-z,relro,-z,now)
  elseif(MINGW)
    add_link_options(LINKER:--dynamicbase LINKER:--nxcompat LINKER:--enable-reloc-section)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
      add_link_options(LINKER:--high-entropy-va)
    endif()
  endif()
endif()

# TLS for the TN5250 client uses the host's own stack: SChannel on Windows
# and Network.framework + Security.framework on macOS (always there), the
# system libssl/libcrypto elsewhere (not from vcpkg).
#   AUTO (default): use it when found, else build without TLS
#   ON: fail configuration without it;  OFF: build without TLS
set(EM34XX_TLS AUTO CACHE STRING "TLS for the TN5250 client: AUTO, ON or OFF")
set_property(CACHE EM34XX_TLS PROPERTY STRINGS AUTO ON OFF)
string(TOUPPER "${EM34XX_TLS}" _em34xx_tls)
set(EM34XX_TLS_BACKEND none)
if(NOT _em34xx_tls MATCHES "^(AUTO|ON|OFF)$")
  message(FATAL_ERROR "EM34XX_TLS must be AUTO, ON or OFF (got '${EM34XX_TLS}')")
endif()
if(NOT _em34xx_tls STREQUAL "OFF")
  if(WIN32)
    set(EM34XX_TLS_BACKEND schannel)
  elseif(APPLE)
    set(EM34XX_TLS_BACKEND apple)
  else()
    find_package(OpenSSL 1.1.1)
    if(OPENSSL_FOUND)
      set(EM34XX_TLS_BACKEND openssl)
    elseif(_em34xx_tls STREQUAL "ON")
      message(FATAL_ERROR "EM34XX_TLS=ON, but the system OpenSSL development files were not found "
                          "(install libssl-dev / openssl-devel, or configure with -DEM34XX_TLS=AUTO or OFF)")
    endif()
  endif()
endif()
if(EM34XX_TLS_BACKEND STREQUAL "openssl")
  message(STATUS "TN5250 TLS: system OpenSSL ${OPENSSL_VERSION} (${OPENSSL_SSL_LIBRARY})")
elseif(EM34XX_TLS_BACKEND STREQUAL "schannel")
  message(STATUS "TN5250 TLS: Windows SChannel (secur32, crypt32)")
elseif(EM34XX_TLS_BACKEND STREQUAL "apple")
  message(STATUS "TN5250 TLS: Apple Network.framework and Security.framework")
else()
  message(STATUS "TN5250 TLS: disabled (EM34XX_TLS=${EM34XX_TLS}); install the OpenSSL development "
                 "files (libssl-dev / openssl-devel) and reconfigure to enable it")
endif()
set(_em34xx_tls_source src/tn5250/tls_${EM34XX_TLS_BACKEND}.cpp)
if(EM34XX_TLS_BACKEND STREQUAL "apple")
  # Objective-C++ with ARC (Network.framework blocks and objects).
  enable_language(OBJCXX)
  set(_em34xx_tls_source src/tn5250/tls_apple.mm)
endif()

# Front ends: each is its own executable, configured only where it applies.
#   EM34XX_FRONTEND_SDL  AUTO (default): build `em34xx` when SDL3 is found
#                        ON: fail configuration without SDL3;  OFF: skip it
# The Win32 `em34xxw` is built on WIN32 only; the Cocoa `em34xx.app`
# (EM34XX_FRONTEND_COCOA) on APPLE only.
set(EM34XX_FRONTEND_SDL AUTO CACHE STRING "SDL3 front end (em34xx): AUTO, ON or OFF")
set_property(CACHE EM34XX_FRONTEND_SDL PROPERTY STRINGS AUTO ON OFF)
string(TOUPPER "${EM34XX_FRONTEND_SDL}" _em34xx_sdl)
if(NOT _em34xx_sdl MATCHES "^(AUTO|ON|OFF)$")
  message(FATAL_ERROR "EM34XX_FRONTEND_SDL must be AUTO, ON or OFF (got '${EM34XX_FRONTEND_SDL}')")
endif()
set(EM34XX_HAVE_SDL OFF)
if(_em34xx_sdl STREQUAL "ON")
  find_package(SDL3 CONFIG REQUIRED)
  set(EM34XX_HAVE_SDL ON)
elseif(_em34xx_sdl STREQUAL "AUTO")
  find_package(SDL3 CONFIG QUIET)
  if(SDL3_FOUND)
    set(EM34XX_HAVE_SDL ON)
  endif()
endif()
message(STATUS "SDL3 front end (em34xx): ${EM34XX_HAVE_SDL}")

# SDL draws its own window frame on Wayland through libdecor, and only when
# libdecor's headers were present as vcpkg built SDL (cmake/triplets/
# x64-linux.cmake). Without them the window has no title bar or border on
# compositors without server-side decorations, GNOME among them.
if(EM34XX_HAVE_SDL AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(EM34XX_LIBDECOR QUIET libdecor-0)
    if(NOT EM34XX_LIBDECOR_FOUND)
      message(WARNING
        "libdecor development files not found (libdecor-devel / libdecor-0-dev): "
        "the em34xx window will have no title bar or border on GNOME Wayland. "
        "Install them, then rebuild SDL: delete ${CMAKE_BINARY_DIR}/vcpkg_installed "
        "and vcpkg's cached sdl3 archive (~/.cache/vcpkg/archives), and configure again.")
    endif()
  endif()
endif()

# EM34XX_FRONTEND_COCOA  AUTO (default) / ON: build em34xx.app on macOS;
#                        OFF: skip it. Never built elsewhere.
set(EM34XX_FRONTEND_COCOA AUTO CACHE STRING "Cocoa front end (em34xx.app, macOS only): AUTO, ON or OFF")
set_property(CACHE EM34XX_FRONTEND_COCOA PROPERTY STRINGS AUTO ON OFF)
string(TOUPPER "${EM34XX_FRONTEND_COCOA}" _em34xx_cocoa)
if(NOT _em34xx_cocoa MATCHES "^(AUTO|ON|OFF)$")
  message(FATAL_ERROR "EM34XX_FRONTEND_COCOA must be AUTO, ON or OFF (got '${EM34XX_FRONTEND_COCOA}')")
endif()
set(EM34XX_HAVE_COCOA OFF)
if(APPLE AND NOT _em34xx_cocoa STREQUAL "OFF")
  set(EM34XX_HAVE_COCOA ON)
elseif(_em34xx_cocoa STREQUAL "ON")
  message(FATAL_ERROR "EM34XX_FRONTEND_COCOA=ON, but em34xx.app builds on macOS only")
endif()
message(STATUS "Cocoa front end (em34xx.app): ${EM34XX_HAVE_COCOA}")

# Executable names, one variable per target so they can change in one place.
set(EM34XX_EXE_SDL em34xx)                 # SDL3 window + developer tooling
set(EM34XX_EXE_HEADLESS em34xx-headless)   # no window, monitor on stdin/stdout
set(EM34XX_EXE_WIN32 em34xxw)              # native Win32 GUI (end-user), WIN32 only
set(EM34XX_EXE_COCOA em34xx)               # Cocoa GUI, bundled as em34xx.app (target em34xx_app), APPLE only
find_package(replxx CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)
# toml++ through em34xx_tomlplusplus. A universal macOS build (more than one
# CMAKE_OSX_ARCHITECTURES) uses it header-only: the vcpkg port's compiled
# library has one architecture (cmake/triplets/universal-osx.cmake).
add_library(em34xx_tomlplusplus INTERFACE)
list(LENGTH CMAKE_OSX_ARCHITECTURES _em34xx_osx_arch_count)
if(APPLE AND _em34xx_osx_arch_count GREATER 1)
  get_target_property(_em34xx_tomlpp_include tomlplusplus::tomlplusplus INTERFACE_INCLUDE_DIRECTORIES)
  target_include_directories(em34xx_tomlplusplus SYSTEM INTERFACE ${_em34xx_tomlpp_include})
  target_compile_definitions(em34xx_tomlplusplus INTERFACE TOML_HEADER_ONLY=1)
else()
  target_link_libraries(em34xx_tomlplusplus INTERFACE tomlplusplus::tomlplusplus)
endif()
find_package(Threads REQUIRED)

add_library(em34xx_warnings INTERFACE)
if(MSVC)
  target_compile_options(em34xx_warnings INTERFACE /W4)
else()
  target_compile_options(em34xx_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
endif()
if(EM34XX_WARNINGS_AS_ERRORS)
  target_compile_options(em34xx_warnings INTERFACE $<IF:$<BOOL:${MSVC}>,/WX,-Werror>)
endif()

# --- Libraries ---------------------------------------------------------------
# Layered; each library links only the layers below it (docs/architecture.md):
#
#   em34xx_emu       util, cpu, bus, rom, machine (Machine, Emulator, interfaces)
#   em34xx_net       sockets                                   -> emu
#   em34xx_machines  devices, models, input                    -> emu
#   em34xx_host      Twinax hosts, workstation controller      -> emu
#   em34xx_tn5250    TN5250 client (TLS)                       -> host, net
#   em34xx_bridge    TN5250 bridge (controller + sessions)     -> host, tn5250
#   em34xx_present   CRT rendering, display profiles           -> emu
#   em34xx_runtime   config, Application, HostLink             -> machines, bridge, present
#   em34xx_devtools  monitor, console (replxx), GDB stub, probes -> runtime, net
#   em34xx_frontend_sdl  SDL3 front end                        -> runtime
#   em34xx_frontend_win32  Win32 front end (WIN32 only)        -> runtime
#   em34xx_frontend_cocoa  Cocoa front end (APPLE only)        -> runtime
#
# End-user GUI executables link em34xx_runtime plus their front end only;
# em34xx (SDL) and em34xx-headless also link em34xx_devtools.
function(em34xx_library name)
  add_library(${name} STATIC ${ARGN})
  target_include_directories(${name} PUBLIC src)
  target_link_libraries(${name} PRIVATE em34xx_warnings)
endfunction()

em34xx_library(em34xx_emu
  src/util/log.cpp
  src/util/sha256.cpp
  src/util/fingerprint.cpp
  src/util/strings.cpp
  src/util/file_io.cpp
  src/bus/bus.cpp
  src/cpu/cpu.cpp
  src/cpu/i86.cpp
  src/cpu/i86_disasm.cpp
  src/rom/romset.cpp
  src/machine/display_subtype.cpp
  src/machine/model_info.cpp
  src/machine/twinax_port.cpp
  src/machine/machine.cpp
  src/machine/emulator.cpp
)
target_link_libraries(em34xx_emu PUBLIC Threads::Threads)
if(WIN32)
  target_compile_definitions(em34xx_emu PUBLIC _WIN32_WINNT=0x0601 WINVER=0x0601)
endif()
if(MINGW)
  # C99 printf family (%zu, %llu) instead of msvcrt's; see util/strings.hpp.
  target_compile_definitions(em34xx_emu PUBLIC __USE_MINGW_ANSI_STDIO=1)
endif()

em34xx_library(em34xx_net src/net/socket.cpp)
target_link_libraries(em34xx_net PUBLIC em34xx_emu)
if(WIN32)
  target_link_libraries(em34xx_net PUBLIC ws2_32)
endif()

em34xx_library(em34xx_machines
  src/models/registry.cpp
  src/models/model_3477.cpp
  src/models/model_3486.cpp
  src/models/model_3489.cpp
  src/input/host_keymap.cpp
  src/input/keyboard_ids.cpp
  src/input/ps2_mouse.cpp
  src/input/set3_keyboard.cpp
  src/devices/battery_ram.cpp
  src/devices/i2c.cpp
  src/devices/ibm3477/display.cpp
  src/devices/ibm3477/i2c.cpp
  src/devices/ibm3477/keyboard_if.cpp
  src/devices/ibm3477/system.cpp
  src/devices/ibm3477/twinax_engine.cpp
  src/devices/ibm3486/aux_port.cpp
)
target_link_libraries(em34xx_machines PUBLIC em34xx_emu PRIVATE em34xx_tomlplusplus)

em34xx_library(em34xx_host
  src/host/host_config.cpp
  src/host/twinax_link.cpp
  src/host/wsc/device_commands.cpp
  src/host/wsc/idle_host.cpp
  src/host/wsc/keyboard_map.cpp
  src/host/wsc/link_scheduler.cpp
  src/host/wsc/screen_writer.cpp
  src/host/wsc/station.cpp
  src/host/wsc/tn5250_adapter.cpp
)
target_link_libraries(em34xx_host PUBLIC em34xx_emu)

# --- TN5250 client: the host side of the Twinax bridge (src/tn5250) --------
em34xx_library(em34xx_tn5250
  src/tn5250/client.cpp
  src/tn5250/codepage.cpp
  src/tn5250/engine.cpp
  src/tn5250/pointer.cpp
  src/tn5250/record.cpp
  src/tn5250/telnet.cpp
  src/tn5250/tls_policy.cpp
  ${_em34xx_tls_source}
  src/tn5250/transport.cpp
)
target_link_libraries(em34xx_tn5250 PUBLIC em34xx_host em34xx_net Threads::Threads)
if(EM34XX_TLS_BACKEND STREQUAL "openssl")
  target_link_libraries(em34xx_tn5250 PRIVATE OpenSSL::SSL OpenSSL::Crypto)
elseif(EM34XX_TLS_BACKEND STREQUAL "schannel")
  target_link_libraries(em34xx_tn5250 PRIVATE secur32 crypt32)
elseif(EM34XX_TLS_BACKEND STREQUAL "apple")
  set_source_files_properties(src/tn5250/tls_apple.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_link_libraries(em34xx_tn5250 PRIVATE
    "-framework Network" "-framework Security" "-framework CoreFoundation" "-framework Foundation")
endif()

# --- TN5250 bridge: the workstation controller with TN5250 sessions --------
em34xx_library(em34xx_bridge src/host/wsc/tn5250_bridge.cpp)
target_link_libraries(em34xx_bridge PUBLIC em34xx_tn5250 em34xx_host)

em34xx_library(em34xx_present
  src/present/crt_render.cpp
  src/present/display_profile.cpp
)
target_link_libraries(em34xx_present PUBLIC em34xx_emu)
# The CRT renderer works per output pixel (millions per frame): unoptimised
# it takes 200+ ms a frame and makes a Debug build unusable interactively.
# Always optimise it; it is rarely what one steps through in a debugger.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set_source_files_properties(src/present/crt_render.cpp src/present/display_profile.cpp
    PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:Debug>:-O2>")
endif()

# --- Application runtime: what every executable links, GUIs included ------
# Version header (generated/em34xx_version.h): project version plus the git
# short hash, regenerated at every build so it follows HEAD (cmake/version.cmake).
set(EM34XX_HOMEPAGE_URL "https://github.com/cyberdotgent/em34xx" CACHE STRING
  "Project page shown in Help > About (empty: not yet announced)")
add_custom_target(em34xx_version
  BYPRODUCTS ${PROJECT_BINARY_DIR}/generated/em34xx_version.h
  COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
    -DOUTPUT=${PROJECT_BINARY_DIR}/generated/em34xx_version.h
    -DMAJOR=${PROJECT_VERSION_MAJOR} -DMINOR=${PROJECT_VERSION_MINOR}
    -DPATCH=${PROJECT_VERSION_PATCH} -DHOMEPAGE=${EM34XX_HOMEPAGE_URL}
    -P ${PROJECT_SOURCE_DIR}/cmake/version.cmake
  COMMENT "Version header"
  VERBATIM)
add_library(em34xx_version_header INTERFACE)
target_include_directories(em34xx_version_header INTERFACE ${PROJECT_BINARY_DIR}/generated)
add_dependencies(em34xx_version_header em34xx_version)

em34xx_library(em34xx_runtime
  src/config/config.cpp
  src/config/connect_args.cpp
  src/config/profile.cpp
  src/config/settings.cpp
  src/config/user_dirs.cpp
  src/app/application.cpp
  src/app/host_link.cpp
)
target_link_libraries(em34xx_runtime PRIVATE em34xx_version_header)
target_link_libraries(em34xx_runtime
  PUBLIC em34xx_machines em34xx_host em34xx_bridge em34xx_present em34xx_tomlplusplus)

# --- Developer tooling: SDL and headless executables only ------------------
em34xx_library(em34xx_devtools
  src/devtools/console.cpp
  src/devtools/dev_tools.cpp
  src/devtools/probe_device.cpp
  src/monitor/expr.cpp
  src/monitor/monitor.cpp
  src/gdb/gdb_stub.cpp
)
target_link_libraries(em34xx_devtools PUBLIC em34xx_runtime em34xx_net PRIVATE replxx::replxx)

add_executable(tn5250-probe src/tools/tn5250_probe.cpp)
target_link_libraries(tn5250-probe PRIVATE em34xx_tn5250 em34xx_warnings)

# --- Executables: one per front end ----------------------------------------
add_executable(${EM34XX_EXE_HEADLESS} src/main_headless.cpp)
target_link_libraries(${EM34XX_EXE_HEADLESS} PRIVATE em34xx_devtools em34xx_warnings)

if(EM34XX_HAVE_SDL)
  em34xx_library(em34xx_frontend_sdl src/frontend/sdl/sdl_frontend.cpp)
  target_link_libraries(em34xx_frontend_sdl PUBLIC em34xx_runtime PRIVATE SDL3::SDL3)

  add_executable(${EM34XX_EXE_SDL} src/main_sdl.cpp)
  target_link_libraries(${EM34XX_EXE_SDL} PRIVATE em34xx_frontend_sdl em34xx_devtools em34xx_warnings)
endif()

# Native end-user front ends link em34xx_runtime and their own front-end
# library only, never em34xx_devtools (docs/architecture.md).
#
# em34xxw: the Win32 GUI (GUI subsystem, resources from em34xxw.rc built by
# windres; docs/frontend-win32.md).
if(WIN32)
  enable_language(RC)
  em34xx_library(em34xx_frontend_win32
    src/frontend/win32/win32_frontend.cpp
    src/frontend/win32/win32_dialogs.cpp
    src/frontend/win32/win32_util.cpp
  )
  target_link_libraries(em34xx_frontend_win32 PUBLIC em34xx_runtime
    PRIVATE comctl32 comdlg32 shell32 gdi32 user32 advapi32)
  # Unicode (W) APIs throughout; the generic macros map to them too.
  target_compile_definitions(em34xx_frontend_win32 PUBLIC UNICODE _UNICODE)
  target_link_libraries(em34xx_frontend_win32 PRIVATE em34xx_version_header)
  add_executable(${EM34XX_EXE_WIN32} WIN32 src/main_win32.cpp src/frontend/win32/em34xxw.rc)
  target_link_libraries(${EM34XX_EXE_WIN32} PRIVATE em34xx_frontend_win32 em34xx_warnings)
  target_link_libraries(${EM34XX_EXE_WIN32} PRIVATE em34xx_version_header)
  # The resource script includes resource.h and the files under res/.
  set_property(SOURCE src/frontend/win32/em34xxw.rc APPEND PROPERTY OBJECT_DEPENDS
    ${PROJECT_SOURCE_DIR}/src/frontend/win32/resource.h
    ${PROJECT_SOURCE_DIR}/src/frontend/win32/em34xxw.manifest
    ${PROJECT_SOURCE_DIR}/src/frontend/win32/res/em34xxw.ico
    ${PROJECT_SOURCE_DIR}/src/frontend/win32/res/profile.ico
    ${PROJECT_SOURCE_DIR}/src/frontend/win32/res/notices.txt
    ${PROJECT_BINARY_DIR}/generated/em34xx_version.h
    ${PROJECT_SOURCE_DIR}/src/frontend/win32/res/toolbar16.bmp
    ${PROJECT_SOURCE_DIR}/src/frontend/win32/res/toolbar24.bmp
    ${PROJECT_SOURCE_DIR}/src/frontend/win32/res/toolbar32.bmp)
endif()

# em34xx.app: the Cocoa GUI (APPLE only; docs/frontend-cocoa.md). Built as a
# universal binary with the macos-* presets (CMAKE_OSX_ARCHITECTURES).
if(EM34XX_HAVE_COCOA)
  enable_language(OBJCXX)
  em34xx_library(em34xx_frontend_cocoa
    src/frontend/cocoa/cocoa_frontend.mm
    src/frontend/cocoa/cocoa_dialogs.mm
    src/frontend/cocoa/cocoa_util.mm
  )
  target_compile_options(em34xx_frontend_cocoa PRIVATE -fobjc-arc)
  target_link_libraries(em34xx_frontend_cocoa PUBLIC em34xx_runtime
    PRIVATE em34xx_version_header "-framework AppKit" "-framework Carbon" "-framework Foundation")

  # Icons from the PNG masters (tools/gen_macos_assets.py).
  set(_em34xx_icons)
  foreach(_icon IN ITEMS em34xx profile)
    set(_icns ${PROJECT_BINARY_DIR}/cocoa/${_icon}.icns)
    add_custom_command(OUTPUT ${_icns}
      COMMAND ${CMAKE_COMMAND} -DSOURCE=${PROJECT_SOURCE_DIR}/src/frontend/cocoa/res/${_icon}.png
        -DICONSET=${PROJECT_BINARY_DIR}/cocoa/${_icon}.iconset -DOUTPUT=${_icns}
        -P ${PROJECT_SOURCE_DIR}/src/frontend/cocoa/make_icns.cmake
      DEPENDS ${PROJECT_SOURCE_DIR}/src/frontend/cocoa/res/${_icon}.png
        ${PROJECT_SOURCE_DIR}/src/frontend/cocoa/make_icns.cmake
      COMMENT "Icon ${_icon}.icns"
      VERBATIM)
    list(APPEND _em34xx_icons ${_icns})
  endforeach()
  set(_em34xx_bundle_resources ${_em34xx_icons} ${PROJECT_SOURCE_DIR}/src/frontend/cocoa/notices.txt)
  set_source_files_properties(${_em34xx_bundle_resources} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

  add_executable(em34xx_app MACOSX_BUNDLE src/main_cocoa.mm ${_em34xx_bundle_resources})
  target_compile_options(em34xx_app PRIVATE -fobjc-arc)
  target_link_libraries(em34xx_app PRIVATE em34xx_frontend_cocoa em34xx_warnings)
  set_target_properties(em34xx_app PROPERTIES
    OUTPUT_NAME ${EM34XX_EXE_COCOA}
    MACOSX_BUNDLE_INFO_PLIST ${PROJECT_SOURCE_DIR}/src/frontend/cocoa/Info.plist.in)
  string(REPLACE ";" "," _em34xx_archs "${CMAKE_OSX_ARCHITECTURES}")
  add_custom_command(TARGET em34xx_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_BUNDLE_DIR:em34xx_app> -DEXE=$<TARGET_FILE:em34xx_app>
      -DARCHS=${_em34xx_archs} -P ${PROJECT_SOURCE_DIR}/src/frontend/cocoa/finish_bundle.cmake
    COMMENT "Checking the architectures of em34xx.app and signing it ad hoc"
    VERBATIM)
endif()

if(EM34XX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
