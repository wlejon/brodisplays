add_library(brodisplays_api STATIC
    api.cpp
    host_class.cpp
    native_displays.cpp
    native_nightlight.cpp
    native_brightness.cpp
)

target_include_directories(brodisplays_api PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>
)

target_link_libraries(brodisplays_api PUBLIC
    brodisplays
    bronze_runtime_shared
)

target_compile_features(brodisplays_api PUBLIC cxx_std_20)

brodisplays_warnings(brodisplays_api)
