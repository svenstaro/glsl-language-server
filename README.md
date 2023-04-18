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
    cmake -Bbuild -GNinja
    ninja -Cbuild

You can also use the `Makefile` in the project root which is provided for convenience.

## Install

    ninja -Cbuild install

## Usage

You can run `glslls` to use a HTTP server to handle IO. Alternatively, run
`glslls --stdin` to handle IO on stdin.

## Editor Examples
The following are examples of how to run `glslls` from various editors that support LSP.

### Emacs

[lsp-mode](https://github.com/emacs-lsp/lsp-mode/) has this language server
integrated into the core. This assumes you have [glsl-mode](https://github.com/jimhourihan/glsl-mode)
installed. See the lsp-mode's [GLSL](https://emacs-lsp.github.io/lsp-mode/page/lsp-glsl/)
for more details.
