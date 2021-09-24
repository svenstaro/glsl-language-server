cmake_minimum_required(VERSION 3.0.0)
project(glsl-language-server)

find_package(Threads REQUIRED)

add_subdirectory(externals/glslang EXCLUDE_FROM_ALL)
include_directories(
    externals/glslang/
    externals/glslang/glslang/Public
    externals/glslang/StandAlone
)

add_subdirectory(externals/fmt EXCLUDE_FROM_ALL)
include_directories(
    externals/fmt
)

add_subdirectory(externals/json EXCLUDE_FROM_ALL)
include_directories(
    externals/json/include
)

add_library(mongoose externals/mongoose/mongoose.c)
include_directories(
    externals/mongoose
)

include_directories(
    externals/CLI11/include)

set(CMAKE_CXX_STANDARD 17)

file(GLOB SOURCES
    src/*.cpp
)

include_directories(src)
add_executable(glslls
    ${SOURCES}
    externals/glslang/StandAlone/ResourceLimits.cpp
)

if (CMAKE_SYSTEM_NAME MATCHES Darwin)
    set(stdfs)
else()
    set(stdfs stdc++fs)
endif()

target_link_libraries(glslls
    ${CMAKE_THREAD_LIBS_INIT}
    glslang
    nlohmann_json
    mongoose
    ${stdfs}
    SPIRV
    fmt::fmt-header-only
)

install(TARGETS glslls
        RUNTIME DESTINATION bin)
