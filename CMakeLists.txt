cmake_minimum_required(VERSION 3.20)
project(ThermalGovernor CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(tgov_core STATIC
    src/core/config.cpp
    src/core/pacer.cpp
)
target_include_directories(tgov_core PUBLIC src src/core)

add_library(tgov_pe STATIC
    src/pe/pe.cpp
)
target_include_directories(tgov_pe PUBLIC src src/pe)

if(WIN32)
    add_library(tgov_sched STATIC
        src/scheduler/scheduler.cpp
    )
    target_include_directories(tgov_sched PUBLIC src src/core)
    target_link_libraries(tgov_sched PUBLIC tgov_core)
    target_link_libraries(tgov_sched PUBLIC user32 kernel32 psapi powrprof)

    add_library(tgov_hooks STATIC
        src/hooks/iat.cpp
        src/hooks/hooks_dx9.cpp
        src/hooks/hooks_dxgi.cpp
        src/hooks/hooks_opengl.cpp
        src/hooks/hooks_vulkan.cpp
    )
    target_include_directories(tgov_hooks PUBLIC src src/core src/hooks)
    target_link_libraries(tgov_hooks PUBLIC tgov_core tgov_pe)
    target_link_libraries(tgov_hooks PUBLIC user32 kernel32 winmm psapi)

    add_library(governor SHARED
        src/runtime/governor.cpp
    )
    target_include_directories(governor PRIVATE src src/core src/runtime)
    target_compile_definitions(governor PRIVATE UNICODE _UNICODE)
    target_link_libraries(governor PRIVATE tgov_core tgov_sched tgov_hooks)
    set_target_properties(governor PROPERTIES OUTPUT_NAME "governor")

    add_executable(governor_injector tools/injector.cpp)
    target_compile_definitions(governor_injector PRIVATE UNICODE _UNICODE)
    target_link_libraries(governor_injector PRIVATE user32 kernel32)

    add_executable(governor_setup tools/governor_setup.cpp)
    target_compile_definitions(governor_setup PRIVATE UNICODE _UNICODE)
    target_link_libraries(governor_setup PRIVATE ole32)
else()
    add_executable(tgov_tests
        tests/test_portable.cpp
    )
    target_include_directories(tgov_tests PRIVATE src src/core)
    target_link_libraries(tgov_tests PRIVATE tgov_core tgov_pe)
    enable_testing()
    add_test(NAME portable COMMAND tgov_tests)
    set_tests_properties(portable PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
