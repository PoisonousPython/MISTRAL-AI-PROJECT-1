cmake_minimum_required(VERSION 3.20)
project(ThermalGovernor CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    add_library(governor SHARED
        src/governor.cpp
        src/config.cpp
        src/pacer.cpp
        src/pe.cpp
        src/iat.cpp
        src/scheduler.cpp
        src/hooks_dx9.cpp
        src/hooks_dxgi.cpp
        src/hooks_opengl.cpp
        src/hooks_vulkan.cpp
    )
    target_compile_definitions(governor PRIVATE UNICODE _UNICODE)
    target_link_libraries(governor PRIVATE user32 kernel32 winmm psapi powrprof)
    set_target_properties(governor PROPERTIES OUTPUT_NAME "governor")

    add_executable(governor_injector tools/injector.cpp)
    target_compile_definitions(governor_injector PRIVATE UNICODE _UNICODE)
    target_link_libraries(governor_injector PRIVATE user32 kernel32)
else()
    add_executable(tgov_tests
        tests/test_portable.cpp
        src/config.cpp
        src/pacer.cpp
        src/pe.cpp
    )
    target_include_directories(tgov_tests PRIVATE src)
    enable_testing()
    add_test(NAME portable COMMAND tgov_tests)
    set_tests_properties(portable PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
