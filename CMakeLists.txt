cmake_minimum_required(VERSION 3.0.0)
project(glsl-language-server)

find_package(Threads REQUIRED)

option(USE_SYSTEM_LIBS "Use system libraries" OFF)
option(HTTP_SUPPORT "Enable HTTP support" ON)

if (HTTP_SUPPORT)
    add_definitions(-DHAVE_HTTP_SUPPORT)
endif()

if (USE_SYSTEM_LIBS)
    find_package(glslang REQUIRED)
    message(STATUS "found package glslang, version: ${glslang_VERSION}")

    find_package(fmt REQUIRED)
    message(STATUS "found package fmt, version: ${fmt_VERSION}")

    if (HTTP_SUPPORT)
        find_library(mongoose mongoose)

        if (mongoose)
            message(STATUS "found library mongoose located in: ${mongoose}")
        else()
            message(FATAL_ERROR "mongoose library not found")
        endif()
    endif()
else()
    add_subdirectory(externals/glslang EXCLUDE_FROM_ALL)

    add_subdirectory(externals/fmt EXCLUDE_FROM_ALL)

    add_subdirectory(externals/json EXCLUDE_FROM_ALL)
    include_directories(
        externals/json/include
    )

    add_library(mongoose externals/mongoose/mongoose.c)
    include_directories(
        externals/mongoose
    )

    include_directories(
        externals/CLI11/include
    )
endif()


set(CMAKE_CXX_STANDARD 20)

file(GLOB SOURCES
    src/*.cpp
)

include_directories(src)
add_executable(glslls
    ${SOURCES}
)

if (MSVC)
    target_compile_options(glslls PRIVATE /W4)
else()
    target_compile_options(glslls PRIVATE -Wall -Wextra -Wpedantic)
endif()

if (CMAKE_SYSTEM_NAME MATCHES Darwin OR CMAKE_SYSTEM_NAME MATCHES FreeBSD)
    set(stdfs)
else()
    set(stdfs stdc++fs)
endif()

target_link_libraries(glslls
    ${CMAKE_THREAD_LIBS_INIT}
    ${stdfs}
    fmt::fmt-header-only
)

if (USE_SYSTEM_LIBS)
    target_link_libraries(glslls
        glslang::glslang
        glslang::glslang-default-resource-limits
    )

    if (HTTP_SUPPORT)
        target_link_libraries(glslls ${mongoose})
    endif()
else()
    target_sources(glslls PRIVATE externals/glslang/glslang/ResourceLimits/ResourceLimits.cpp)
    target_link_libraries(glslls
        glslang
        nlohmann_json
        mongoose
    )
endif()

install(TARGETS glslls
    RUNTIME DESTINATION bin
)
