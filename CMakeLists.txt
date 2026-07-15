cmake_minimum_required(VERSION 3.25)

project(Hazzard
    VERSION 1.0.0
    DESCRIPTION "Lock-free stack with Hazard Pointers"
    LANGUAGES CXX
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(HAZZARD_ENABLE_WARNINGS "Enable strong compiler warnings" ON)
option(HAZZARD_ENABLE_SANITIZERS "Enable sanitizers in supported compilers" OFF)
option(HAZZARD_ENABLE_LTO "Enable link-time optimization in Release builds" ON)

add_executable(Hazzard
    main.cpp
)

target_compile_features(Hazzard PRIVATE cxx_std_20)

set_target_properties(Hazzard PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

find_package(Threads REQUIRED)
target_link_libraries(Hazzard PRIVATE Threads::Threads)

if(HAZZARD_ENABLE_WARNINGS)
    if(MSVC)
        target_compile_options(Hazzard PRIVATE
            /W4
            /permissive-
            /EHsc
            /Zc:__cplusplus
            /Zc:preprocessor
            /utf-8
        )
    else()
        target_compile_options(Hazzard PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
            -Wshadow
            -Wnon-virtual-dtor
            -Wold-style-cast
            -Wcast-align
            -Wunused
            -Woverloaded-virtual
            -Wnull-dereference
            -Wdouble-promotion
            -Wformat=2
        )
    endif()
endif()

if(MSVC)
    target_compile_options(Hazzard PRIVATE
        $<$<CONFIG:Debug>:/Od /Zi>
        $<$<CONFIG:Release>:/O2 /DNDEBUG>
        $<$<CONFIG:RelWithDebInfo>:/O2 /Zi>
    )
else()
    target_compile_options(Hazzard PRIVATE
        $<$<CONFIG:Debug>:-O0 -g3>
        $<$<CONFIG:Release>:-O3 -DNDEBUG>
        $<$<CONFIG:RelWithDebInfo>:-O2 -g>
    )
endif()

include(CheckIPOSupported)
check_ipo_supported(RESULT HAZZARD_IPO_SUPPORTED OUTPUT HAZZARD_IPO_OUTPUT)

if(HAZZARD_ENABLE_LTO AND HAZZARD_IPO_SUPPORTED)
    set_property(TARGET Hazzard PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set_property(TARGET Hazzard PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
endif()

if(HAZZARD_ENABLE_SANITIZERS)
    if(MSVC)
        message(WARNING "Sanitizers are not configured here for MSVC in this file.")
    else()
        target_compile_options(Hazzard PRIVATE
            $<$<CONFIG:Debug>:-fsanitize=address,undefined>
        )
        target_link_options(Hazzard PRIVATE
            $<$<CONFIG:Debug>:-fsanitize=address,undefined>
        )
    endif()
endif()

message(STATUS "Project name: ${PROJECT_NAME}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ compiler: ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "C++ standard: 20")
message(STATUS "Warnings enabled: ${HAZZARD_ENABLE_WARNINGS}")
message(STATUS "Sanitizers enabled: ${HAZZARD_ENABLE_SANITIZERS}")
message(STATUS "LTO enabled: ${HAZZARD_ENABLE_LTO}")
