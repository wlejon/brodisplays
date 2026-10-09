cmake_minimum_required(VERSION 3.24)

project(brodisplays VERSION 0.1.0 LANGUAGES C CXX)

# Dependencies (cmake/bro_deps.cmake): an outer project's target wins, else the
# ../<name> working tree beside the top-level project, else the pinned commit,
# fetched at configure.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/bro_deps.cmake)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(_brodisplays_top_level OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_brodisplays_top_level ON)
endif()

option(BRODISPLAYS_BUILD_TESTS "Build the brodisplays tests" ${_brodisplays_top_level})

# Coverage. Scoped to a top-level build: a consumer that add_subdirectory()s
# brodisplays has no business having its own build instrumented from in here.
option(BRODISPLAYS_COVERAGE "Instrument the build for gcov coverage (GCC/Clang)" OFF)
if(BRODISPLAYS_COVERAGE AND _brodisplays_top_level)
    if(MSVC)
        message(FATAL_ERROR "BRODISPLAYS_COVERAGE requires GCC or Clang (gcov instrumentation)")
    endif()
    # Atomic counters: the watchers and revert timer run threads, and racing
    # plain counters leave negative branch counts gcov cannot read.
    add_compile_options(--coverage -fprofile-update=atomic -O0 -g)
    add_link_options(--coverage)
    message(STATUS "brodisplays: coverage instrumentation ON (-O0)")
endif()

# MSVC runtime: the default, the DLL CRT (/MD, /MDd), as bro builds with. The
# api's tests load bronze_runtime_shared.dll and pass std::string and vectors
# across that boundary; with the static CRT each module has its own heap, and
# memory freed on the other side is a Debug assert (__acrt_first_block ==
# header) and silent heap corruption in Release.
if(MSVC AND _brodisplays_top_level)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

function(brodisplays_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /MP)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

file(GLOB BRODISPLAYS_SOURCES CONFIGURE_DEPENDS
    src/common/types.cpp
    src/common/edid.cpp
    src/common/color_temperature.cpp
    src/common/revert_timer.cpp
    src/common/backlight_types.cpp)

set(BRODISPLAYS_LINK_LIBS "")
set(BRODISPLAYS_PRIVATE_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(WIN32)
    file(GLOB _win_sources CONFIGURE_DEPENDS src/win/*.cpp)
    list(APPEND BRODISPLAYS_SOURCES ${_win_sources})
    list(APPEND BRODISPLAYS_LINK_LIBS user32 gdi32 shell32 ole32 advapi32 dxva2 shcore)
elseif(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 20)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    file(GLOB _mac_sources CONFIGURE_DEPENDS src/mac/*.cpp src/mac/*.mm)
    list(APPEND BRODISPLAYS_SOURCES ${_mac_sources})
    list(APPEND BRODISPLAYS_LINK_LIBS
        "-framework CoreFoundation"
        "-framework CoreGraphics"
        "-framework ColorSync"
        "-framework IOKit"
        "-framework Cocoa"
        "-framework AppKit")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)
    find_package(Threads REQUIRED)

    pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
    # X11 is spoken through XCB (not Xlib): every request is bounded by a
    # watchdog that can abandon a server that stops answering, which Xlib's
    # fatal I/O error handling does not allow.
    pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb xcb-randr xau)
    # GIO (D-Bus + GSettings) drives the desktops' own night lights (KDE
    # Plasma's KWin, GNOME). Optional: without it night light falls back to
    # gamma ramps where no desktop service owns them, and is reported
    # unsupported under KDE and GNOME.
    pkg_check_modules(GIO IMPORTED_TARGET gio-2.0)
    option(BRODISPLAYS_WITH_GIO "Control KDE / GNOME night light over D-Bus and GSettings (gio-2.0)" ${GIO_FOUND})

    find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

    set(PROTO_DIR ${CMAKE_CURRENT_BINARY_DIR}/protocols)
    file(MAKE_DIRECTORY ${PROTO_DIR})
    list(APPEND BRODISPLAYS_PRIVATE_INCLUDES ${PROTO_DIR})

    foreach(_proto wlr-output-management-unstable-v1 wlr-gamma-control-unstable-v1)
        set(_xml ${CMAKE_CURRENT_SOURCE_DIR}/protocols/${_proto}.xml)
        set(_header ${PROTO_DIR}/${_proto}-client-protocol.h)
        set(_code ${PROTO_DIR}/${_proto}-protocol.c)
        add_custom_command(
            OUTPUT ${_header} ${_code}
            COMMAND ${WAYLAND_SCANNER} client-header ${_xml} ${_header}
            COMMAND ${WAYLAND_SCANNER} private-code ${_xml} ${_code}
            DEPENDS ${_xml}
            VERBATIM)
        list(APPEND BRODISPLAYS_SOURCES ${_header} ${_code})
    endforeach()

    file(GLOB _linux_sources CONFIGURE_DEPENDS src/linux/*.cpp src/linux/backlight/*.cpp)
    list(APPEND BRODISPLAYS_SOURCES ${_linux_sources})
    list(APPEND BRODISPLAYS_LINK_LIBS
        PkgConfig::WAYLAND_CLIENT
        PkgConfig::XCB
        Threads::Threads)
    if(BRODISPLAYS_WITH_GIO)
        list(APPEND BRODISPLAYS_LINK_LIBS PkgConfig::GIO)
    endif()
else()
    message(FATAL_ERROR "brodisplays supports Windows, macOS and Linux only")
endif()

add_library(brodisplays STATIC ${BRODISPLAYS_SOURCES})
add_library(brodisplays::brodisplays ALIAS brodisplays)

target_include_directories(brodisplays
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${BRODISPLAYS_PRIVATE_INCLUDES})

target_link_libraries(brodisplays PRIVATE ${BRODISPLAYS_LINK_LIBS})
brodisplays_warnings(brodisplays)

if(WIN32)
    target_compile_definitions(brodisplays PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
elseif(APPLE)
    target_compile_options(brodisplays PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
elseif(BRODISPLAYS_WITH_GIO)
    target_compile_definitions(brodisplays PRIVATE BRODISPLAYS_HAVE_GIO=1)
endif()

# --- Standalone Bronze JavaScript API ---
# On by default only when brodisplays is the top-level project. A library that
# consumes brodisplays (brocompositor) then never needs bronze checked out;
# bro, which does want the binding, sets BRODISPLAYS_ENABLE_API ON itself.
option(BRODISPLAYS_ENABLE_API "Build Bronze JavaScript API" ${PROJECT_IS_TOP_LEVEL})
if(BRODISPLAYS_ENABLE_API)
    # bronze (with brass, its code generator) through cmake/bro_deps.cmake: an
    # outer project's target, else ../bronze beside the top-level project, else
    # the pinned commit, fetched at configure.
    bro_dependency(bronze GITHUB wlejon/bronze TARGET bronze_runtime_shared
                   OPTIONS BRONZE_BUILD_SHARED_RUNTIME=ON BRONZE_BUILD_TESTS=OFF)

    add_subdirectory(src/api)
endif()

if(BRODISPLAYS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

