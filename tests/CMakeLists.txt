function(add_bd_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE brodisplays::brodisplays)
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src)
    brodisplays_warnings(${name})
    if(WIN32)
        target_compile_definitions(${name} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
    endif()
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77)
endfunction()

# Common tests (run on all platforms)
add_bd_test(test_edid common/test_edid.cpp)
add_bd_test(test_event_queue common/test_event_queue.cpp)
add_bd_test(test_revert_timer common/test_revert_timer.cpp)

# Windows tests
if(WIN32)
    add_bd_test(test_win_query win/test_win_query.cpp)
    add_bd_test(test_win_revert win/test_win_revert.cpp)
    add_bd_test(test_win_events win/test_win_events.cpp)
    add_bd_test(test_win_gamma win/test_win_gamma.cpp)
endif()

# Linux tests
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_bd_test(test_linux_query linux/test_linux_query.cpp)
    add_bd_test(test_linux_revert linux/test_linux_revert.cpp)
    add_bd_test(test_linux_drm_sysfs linux/test_linux_drm_sysfs.cpp)
    add_bd_test(test_linux_unresponsive linux/test_linux_unresponsive.cpp)
    set_tests_properties(test_linux_query test_linux_unresponsive PROPERTIES TIMEOUT 60)
endif()

# macOS tests
if(APPLE)
    add_bd_test(test_mac_query mac/test_mac_query.cpp)
    add_bd_test(test_mac_revert mac/test_mac_revert.cpp)
    add_bd_test(test_mac_gamma mac/test_mac_gamma.cpp)
endif()
