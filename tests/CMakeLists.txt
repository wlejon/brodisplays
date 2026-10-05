function(add_bd_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE brodisplays::brodisplays)
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src)
    brodisplays_warnings(${name})
    if(WIN32)
        target_compile_definitions(${name} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
        target_link_libraries(${name} PRIVATE user32 shcore)
    endif()
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77)
endfunction()

# Common tests (run on all platforms)
add_bd_test(test_edid common/test_edid.cpp)
add_bd_test(test_event_queue common/test_event_queue.cpp)
add_bd_test(test_revert_timer common/test_revert_timer.cpp)
add_bd_test(test_backlight_curves common/test_backlight_curves.cpp)

# Windows tests
if(WIN32)
    add_bd_test(test_win_query win/test_win_query.cpp)
    add_bd_test(test_win_revert win/test_win_revert.cpp)
    add_bd_test(test_win_confirm win/test_win_confirm.cpp)
    add_bd_test(test_win_events win/test_win_events.cpp)
    add_bd_test(test_win_gamma win/test_win_gamma.cpp)
endif()

# Linux tests
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_bd_test(test_linux_query linux/test_linux_query.cpp)
    add_bd_test(test_linux_revert linux/test_linux_revert.cpp)
    add_bd_test(test_linux_drm_sysfs linux/test_linux_drm_sysfs.cpp)
    add_bd_test(test_linux_backlight linux/test_linux_backlight.cpp)
    add_bd_test(test_linux_unresponsive linux/test_linux_unresponsive.cpp)
    set_tests_properties(test_linux_query test_linux_unresponsive PROPERTIES TIMEOUT 60)

    # Against private servers each test starts (Xvfb, headless sway, virtual
    # KWin, headless mutter): skipped when the server is not installed.
    add_bd_test(test_linux_x11_stopped linux/test_linux_x11_stopped.cpp linux/private_session.cpp)
    add_bd_test(test_linux_night_light_x11 linux/test_linux_night_light_x11.cpp linux/private_session.cpp)
    target_link_libraries(test_linux_night_light_x11 PRIVATE PkgConfig::XCB)
    add_bd_test(test_linux_night_light_wlr linux/test_linux_night_light_wlr.cpp linux/private_session.cpp)
    target_include_directories(test_linux_night_light_wlr PRIVATE ${PROTO_DIR})
    target_link_libraries(test_linux_night_light_wlr PRIVATE PkgConfig::WAYLAND_CLIENT)
    add_bd_test(test_linux_night_light_kwin linux/test_linux_night_light_kwin.cpp linux/private_session.cpp)
    add_bd_test(test_linux_night_light_gnome linux/test_linux_night_light_gnome.cpp linux/private_session.cpp)
    set_tests_properties(test_linux_x11_stopped test_linux_night_light_x11 test_linux_night_light_wlr
                         test_linux_night_light_kwin test_linux_night_light_gnome PROPERTIES TIMEOUT 90)
endif()

# macOS tests
if(APPLE)
    add_bd_test(test_mac_query mac/test_mac_query.cpp)
    add_bd_test(test_mac_revert mac/test_mac_revert.cpp)
    add_bd_test(test_mac_confirm mac/test_mac_confirm.cpp)
    add_bd_test(test_mac_gamma mac/test_mac_gamma.cpp)
endif()
