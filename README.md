# f4tty

A lightweight, SDL2-based terminal emulator written in C.

## Features

- **SDL2 & SDL_ttf Rendering**: Grid-based monospace font rendering using SDL2 and FreeType / TrueType fonts.
- **PTY Subprocess**: Spawns shell processes (`$SHELL` or `/bin/bash`) using POSIX pseudo-terminals (`pty.h`).
- **Dynamic Resizing**: Automatically recalculates terminal grid columns/rows upon window resize and signals the shell via `TIOCSWINSZ`.
- **Terminal Scrolling**: Buffer line-shifting on line feeds (`\n`) and bottom-row wrapping.
- **ANSI Escape Parsing**:
  - **CSI**: Cursor navigation (`H`, `f`, `A`, `B`, `C`, `D`, `E`, `F`, `G`, `d`) and screen/line clearing (`J`, `K`).
  - **OSC**: Window title updates (`OSC 0` / `OSC 2`) reflected in the SDL window title, cleanly handling embedded styling codes.
  - Basic reset sequences (`ESC c`, `ESC M`).
- **Input System**:
  - `SDL_TEXTINPUT` for UTF-8 printable characters.
  - Special key bindings (Enter, Backspace, Tab, Escape, Arrows, Home, End, Delete, Page Up/Down).
  - Control key shortcuts (`Ctrl+A` through `Ctrl+Z`, including `Ctrl+C`, `Ctrl+D`, `Ctrl+L`).

## Requirements

- GCC / Clang
- `pkg-config`
- `SDL2` and `SDL2_ttf` development libraries
- A monospace TrueType font (defaults to Ubuntu Mono or DejaVu Sans Mono)

On Debian/Ubuntu:
```bash
sudo apt update
sudo apt install build-essential libsdl2-dev libsdl2-ttf-dev fonts-dejavu-core
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
./f4tty
```

To clean build artifacts:
```bash
make clean
```
