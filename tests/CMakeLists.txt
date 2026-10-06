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

if(BRODISPLAYS_ENABLE_API)
    add_executable(brodisplays_test_api
        test_displays_api.cpp
    )
    target_include_directories(brodisplays_test_api PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/api
        ${PROJECT_SOURCE_DIR}/src
    )
    target_link_libraries(brodisplays_test_api PRIVATE
        brodisplays_api
        brodisplays
        bronze_runtime_shared
    )
    brodisplays_warnings(brodisplays_test_api)
    if(WIN32)
        target_compile_definitions(brodisplays_test_api PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
        target_link_libraries(brodisplays_test_api PRIVATE user32 shcore)
        add_custom_command(TARGET brodisplays_test_api POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:bronze_runtime_shared>
                $<TARGET_FILE_DIR:brodisplays_test_api>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_LINKER_FILE:bronze_runtime_shared>
                $<TARGET_FILE_DIR:brodisplays_test_api>
        )
    endif()

    if(NOT BRASS_HOST_BACKEND)
        message(STATUS "brodisplays: brodisplays_test_api not registered (brass has no code generator for this target)")
    else()
        add_test(NAME brodisplays_test_api COMMAND brodisplays_test_api)
        set_tests_properties(brodisplays_test_api PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)

        # A collection at every allocation over a Debug bronze (-O0, and
        # instrumented in the coverage job) takes minutes, not seconds. The
        # limit is there to catch a hang, not to time the run.
        add_test(NAME brodisplays_test_api_gc_stress COMMAND brodisplays_test_api)
        set_tests_properties(brodisplays_test_api_gc_stress PROPERTIES
            TIMEOUT 600
            SKIP_RETURN_CODE 77
            ENVIRONMENT "BRONZE_GC_STRESS=1;BRONZE_GC_POISON=1"
        )
        if(WIN32)
            set_tests_properties(brodisplays_test_api PROPERTIES
                ENVIRONMENT "PATH=$<TARGET_FILE_DIR:bronze_runtime_shared>;$<TARGET_FILE_DIR:brodisplays_test_api>;$ENV{PATH}")
            set_property(TEST brodisplays_test_api_gc_stress APPEND PROPERTY
                ENVIRONMENT "PATH=$<TARGET_FILE_DIR:bronze_runtime_shared>;$<TARGET_FILE_DIR:brodisplays_test_api>;$ENV{PATH}")
        endif()
    endif()
endif()

# Every test executable links no_dialogs.cpp (see the file).
if(WIN32)
    get_property(_test_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_t IN LISTS _test_targets)
        get_target_property(_type ${_t} TYPE)
        if(_type STREQUAL "EXECUTABLE")
            target_sources(${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/no_dialogs.cpp)
        endif()
    endforeach()
endif()

