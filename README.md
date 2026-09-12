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
- **Text Selection & Clipboard (Copy & Paste)**:
  - Mouse-driven text selection: click & drag character range, double-click for word, triple-click for line.
  - Automatic copy to clipboard on mouse drag release.
  - Keyboard shortcuts: `Ctrl + Shift + C` to copy, `Ctrl + Shift + V` and `Shift + Insert` to paste.
  - Middle-click mouse paste (`SDL_BUTTON_MIDDLE`).
  - Bracketed paste mode (`\e[?2004h` / `\e[?2004l`) for safe multi-line pasting.
  - Preserves standard `Ctrl + C` for reliable process interruption (`SIGINT`).
  - Gruvbox-themed visual selection highlighting.
- **Full-Screen Application & Alternate Buffer Support**:
  - Alternate Screen Buffer switching (`\e[?1049h` / `\e[?1049l`, `\e[?1047h`/`l`, `\e[?47h`/`l`) with clean cursor save/restore and separate screen contexts.
  - `DECSTBM` scrolling regions (`\e[top;bottomr`) and margined scrolling (`scroll_region_up` / `scroll_region_down`, Reverse Index `\eM`), cleanly supporting pinned status bars and multi-pane multiplexers (`tmux`, `screen`, `neovim`).
  - Line & character editing: Insert Line (`IL` / `\e[L`), Delete Line (`DL` / `\e[M`), Insert Character (`ICH` / `\e[@`), Delete Character (`DCH` / `\e[P`), Erase Character (`ECH` / `\e[X`), Scroll Up (`SU` / `\e[S`), Scroll Down (`SD` / `\e[T`), Backtab (`CBT` / `\e[Z`).
  - 24-bit TrueColor (`38;2;r;g;b` / `48;2;r;g;b`) and 256-color palette (`38;5;idx` / `48;5;idx`), with bold, dim, underline, and reverse video attributes.
  - Mouse tracking: Normal, Button-Event, and Any-Event tracking (`\e[?1000h`..`\e[?1003h`) with SGR 1006 encoding. Holding `Shift` bypasses app tracking for native text selection at any time.
  - Application Cursor Keys (DECCKM `\e[?1h` / `\e[?1l`), function keys `F1`–`F12`, and cursor visibility control (`\e[?25h` / `\e[?25l`).
  - Terminal queries and handshakes: CPR (`\e[6n`), DSR (`\e[5n`), DA1 (`\e[c`), DA2 (`\e[>c`), OSC 10/11 color queries.
  - Zero rogue artifacts: clean charset designator absorption (`\e(B`, `\e)0`, etc.) and DCS string handling.
- **Bottom Status Bar**:
  - Live clock and terminal dimensions display on the main buffer.
  - Automatically hidden in alternate screen buffer applications (Vim, htop, tmux, less).
- **Input System**:
  - `SDL_TEXTINPUT` for UTF-8 printable characters.
  - Special key bindings (Enter, Backspace, Tab, Escape, Arrows, Home, End, Delete, Page Up/Down, F11 for fullscreen, F1–F12).
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
