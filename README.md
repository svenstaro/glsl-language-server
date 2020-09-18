# glsl-language-server
Language server implementation for GLSL

## Status
Currently this LSP implementation can be interfaced with using either HTTP or stdio.

### Current Features

- Diagnostics

### Planned Features

- Completion
- Hover
- Jump to def
- Workspace symbols
- Find references


## Compile

    git submodule update --init
    cmake -Bbuild
    make -Cbuild

You can also use the `Makefile` in the project root which is provided for convenience.

## Install

    make -Cbuild install

## Usage

You can run `glslls` to use a HTTP server to handle IO. Alternatively, run
`glslls --stdin` to handle IO on stdin.
