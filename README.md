# fatty

A lightweight, SDL2-based terminal emulator written in C.

## Features

- **SDL2 & SDL_ttf Rendering**: Grid-based monospace font rendering using SDL2 and FreeType / TrueType fonts.
- **PTY Subprocess**: Spawns shell processes (`$SHELL` or `/bin/bash`) using POSIX pseudo-terminals (`pty.h`).
- **Scrollback History Buffer**: 2,000-line circular ring buffer for reviewing previous output.
  - Mouse wheel scrolling (`SDL_MOUSEWHEEL`).
  - Keyboard shortcuts (`Shift + PageUp`, `Shift + PageDown`, `Shift + Up`, `Shift + Down`).
  - Automatic viewport snap-back to prompt on active typing.
  - Visual scrollbar indicator on the window margin.
- **Gruvbox Dark Palette & ANSI Styling**:
  - Full 16-color ANSI support (0–7 standard, 8–15 bright).
  - Bold weight font rendering with automatic `bold-is-bright` mapping.
  - Per-cell foreground and background color rendering.
- **Dynamic Resizing & Zoom**:
  - Automatically recalculates terminal grid columns/rows upon window resize and signals the shell via `TIOCSWINSZ`.
  - Font zoom in/out/reset (`Ctrl + +` / `Ctrl + =`, `Ctrl + -`, `Ctrl + 0`).
- **Unicode & Multilingual Support**:
  - Full UTF-8 streaming decoder and 32-bit Unicode codepoint cell storage.
  - Double-width character support (`wcwidth`) with CJK and Color Emoji font fallbacks.
- **Procedural Box Drawing & Block Elements**:
  - Zero-seam rendering for box-drawing (`U+2500`–`U+257F`: single, double, heavy, rounded `╭╮╰╯`, diagonals `╱╲╳`).
  - Native geometry rendering for block elements (`U+2580`–`U+259F`: solids, fractions, quadrants, shades).
- **GPU Glyph Texture Cache**: Fast open-addressed LRU cache reusing rendered glyph textures across frames.
- **ANSI Escape Parsing**:
  - **CSI**: Cursor navigation (`H`, `f`, `A`, `B`, `C`, `D`, `E`, `F`, `G`, `d`), screen clearing (`J`, including `\e[3J` scrollback clear), and line clearing (`K`).
  - **SGR**: Text attributes and color codes (`m`).
  - **OSC**: Window title updates (`OSC 0` / `OSC 2`) reflected in the SDL window title, cleanly handling embedded styling codes.
- **Input System**:
  - `SDL_TEXTINPUT` for UTF-8 printable characters.
  - Special key bindings (Enter, Backspace, Tab, Escape, Arrows, Home, End, Delete, Page Up/Down, F11 for fullscreen).
  - Control key shortcuts (`Ctrl+A` through `Ctrl+Z`, including `Ctrl+C`, `Ctrl+D`, `Ctrl+L`).

## Requirements

- GCC / Clang
- `pkg-config`
- `SDL2` and `SDL2_ttf` development libraries
- Monospace TrueType fonts (defaults to DejaVu Sans Mono, Noto Sans CJK, and Noto Color Emoji)

On Debian/Ubuntu:
```bash
sudo apt update
sudo apt install build-essential libsdl2-dev libsdl2-ttf-dev fonts-dejavu-core fonts-noto-cjk fonts-noto-color-emoji
```

## Build & Run

To compile:
```bash
make
```

To run:
```bash
make run
# or directly:
./fatty
```

To clean build artifacts:
```bash
make clean
```
