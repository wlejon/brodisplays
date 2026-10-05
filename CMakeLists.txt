cmake_minimum_required(VERSION 3.24)

project(brodisplays VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(_brodisplays_top_level OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_brodisplays_top_level ON)
endif()

option(BRODISPLAYS_BUILD_TESTS "Build the brodisplays tests" ${_brodisplays_top_level})

if(MSVC AND _brodisplays_top_level)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
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
    src/common/revert_timer.cpp)

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
    pkg_check_modules(XRANDR REQUIRED IMPORTED_TARGET xrandr)
    pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)

    find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

    set(PROTO_DIR ${CMAKE_CURRENT_BINARY_DIR}/protocols)
    file(MAKE_DIRECTORY ${PROTO_DIR})
    list(APPEND BRODISPLAYS_PRIVATE_INCLUDES ${PROTO_DIR})

    set(WLR_XML ${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-output-management-unstable-v1.xml)
    set(WLR_HEADER ${PROTO_DIR}/wlr-output-management-unstable-v1-client-protocol.h)
    set(WLR_CODE ${PROTO_DIR}/wlr-output-management-unstable-v1-protocol.c)

    add_custom_command(
        OUTPUT ${WLR_HEADER} ${WLR_CODE}
        COMMAND ${WAYLAND_SCANNER} client-header ${WLR_XML} ${WLR_HEADER}
        COMMAND ${WAYLAND_SCANNER} private-code ${WLR_XML} ${WLR_CODE}
        DEPENDS ${WLR_XML}
        VERBATIM)

    file(GLOB _linux_sources CONFIGURE_DEPENDS src/linux/*.cpp)
    list(APPEND BRODISPLAYS_SOURCES ${_linux_sources} ${WLR_CODE} ${WLR_HEADER})
    list(APPEND BRODISPLAYS_LINK_LIBS
        PkgConfig::WAYLAND_CLIENT
        PkgConfig::XRANDR
        PkgConfig::X11
        Threads::Threads)
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
endif()

if(BRODISPLAYS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
