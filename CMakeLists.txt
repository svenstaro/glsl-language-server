cmake_minimum_required(VERSION 3.0.0)
project(glsl-language-server)

find_package(Threads REQUIRED)

add_subdirectory(externals/glslang)
include_directories(
    externals/glslang/glslang/Public
    externals/glslang/StandAlone
)

add_subdirectory(externals/fmt)
include_directories(
    externals/fmt
)

add_subdirectory(externals/json)
include_directories(
    externals/json/src
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
target_link_libraries(glslls
    ${CMAKE_THREAD_LIBS_INIT}
    glslang
    nlohmann_json
    mongoose
    stdc++fs
    fmt::fmt-header-only
)

install(TARGETS glslls
        RUNTIME DESTINATION bin)
