# glsl-language-server
Language server implementation for GLSL

## Status
Currently this LSP implementation can be interfaced with using either HTTP or stdio.

## Compile

    git submodule update --init
    mkdir build
    cd build
    cmake ..
    make

You can also use the `Makefile` in the project root which is provided for convenience.

## Install

    cd build
    make install
