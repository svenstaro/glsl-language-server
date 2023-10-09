# glsl-language-server
Language server implementation for GLSL

# I'm searching for a new maintainer.
If you'd like to maintain this, please create an issue. This is currently still maintained and I
will accept PRs but I'd like for someone to give this more love.

When I started this, there were no other language servers for GLSL but now there are some
alternatives which you can check out:

- [glsl-lsp](https://github.com/KubaP/glsl-lsp)
- [glsl_analyzer](https://github.com/nolanderc/glsl_analyzer)

## Status
Currently this LSP implementation can be interfaced with using either HTTP or stdio.

### Current Features

- Diagnostics
- Completion
- Hover
- Jump to def

### Planned Features

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

### Neovim

[lspconfig](https://github.com/neovim/nvim-lspconfig) offers a ready-to-go configuration:

```lua
require'lspconfig'.glslls.setup{}
```
