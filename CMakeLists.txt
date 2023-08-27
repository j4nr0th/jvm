cmake_minimum_required(VERSION 3.26)
project(jvm C)

set(CMAKE_C_STANDARD 99)
find_package(Vulkan REQUIRED)

add_library(jvm
        source/jvm.c
        include/jvm.h
        source/internal.c
        source/internal.h)

target_include_directories(jvm PRIVATE "${Vulkan_INCLUDE_DIR}")
target_link_libraries(jvm PRIVATE "${Vulkan_LIBRARY}")
target_compile_definitions(jvm PRIVATE JVM_BUILD_LIBRARY)
get_target_property(JVM_BUILD_TYPE jvm TYPE)
if ("${JVM_BUILD_TYPE}" STREQUAL SHARED_LIBRARY)
    target_compile_definitions(jvm PRIVATE JVM_BUILD_SHARED)
endif ()

if (CMAKE_C_COMPILER_ID STREQUAL GNU)
    target_compile_options(jvm PRIVATE -Wall -Wextra -Werror)
endif ()
