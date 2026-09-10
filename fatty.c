#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <pty.h>
#include <locale.h>
#include <wchar.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#define DEFAULT_COLS 80
#define DEFAULT_ROWS 24

/* Runtime terminal dimensions — updated on resize */
static int cols = DEFAULT_COLS;
static int rows = DEFAULT_ROWS;

static int win_width, win_height;

uint8_t fullscreen = 0;
/* Glyph cell size in pixels — set once after font load */
static int char_w = 0, char_h = 0;
#define DEFAULT_FONT_SIZE 22
static int font_size = DEFAULT_FONT_SIZE;
TTF_Font* font, *font_bold, *font_cjk, *font_emoji;
static void clear_glyph_cache(void);

#define ATTR_DEFAULT_FG 255
#define ATTR_DEFAULT_BG 255

typedef struct {
    uint32_t codepoint; /* Unicode codepoint (U+0000 .. U+10FFFF) */
    uint8_t  fg;        /* 0-15 or ATTR_DEFAULT_FG */
    uint8_t  bg;        /* 0-15 or ATTR_DEFAULT_BG */
    uint8_t  bold;      /* 1 if bold / high-intensity */
    uint8_t  width;     /* 1 for standard, 2 for wide lead, 0 for wide continuation */
} cell_t;

static const SDL_Color default_bg   = {28, 28, 28, 255};
static const SDL_Color default_fg   = {235, 219, 178, 255};
static const SDL_Color cursor_color = {168, 153, 132, 255};

static const SDL_Color ansi_palette[16] = {
    {28, 28, 28, 255},     /* 0:  Black          */
    {204, 36, 29, 255},    /* 1:  Red            */
    {152, 151, 26, 255},   /* 2:  Green          */
    {215, 153, 33, 255},   /* 3:  Yellow         */
    {69, 133, 136, 255},   /* 4:  Blue           */
    {177, 98, 134, 255},   /* 5:  Magenta        */
    {104, 157, 106, 255},  /* 6:  Cyan           */
    {168, 153, 132, 255},  /* 7:  White          */
    {146, 131, 116, 255},  /* 8:  Bright Black   */
    {251, 89, 68, 255},    /* 9:  Bright Red     */
    {184, 187, 38, 255},   /* 10: Bright Green   */
    {250, 189, 47, 255},   /* 11: Bright Yellow  */
    {131, 165, 152, 255},  /* 12: Bright Blue    */
    {211, 134, 155, 255},  /* 13: Bright Magenta */
    {142, 192, 124, 255},  /* 14: Bright Cyan    */
    {235, 219, 178, 255}   /* 15: Bright White   */
};

static uint8_t current_fg   = ATTR_DEFAULT_FG;
static uint8_t current_bg   = ATTR_DEFAULT_BG;
static uint8_t current_bold = 0;

static inline cell_t make_blank_cell(void) {
    cell_t cell;
    cell.codepoint = 0;
    cell.fg        = ATTR_DEFAULT_FG;
    cell.bg        = ATTR_DEFAULT_BG;
    cell.bold      = 0;
    cell.width     = 1;
    return cell;
}

typedef struct {
    uint32_t x,y;
} cursor_t;

typedef struct {
    cell_t cells[DEFAULT_COLS];
} row_t;

static cell_t* term_buffer;
cursor_t cursor = {0,0};

#define SCROLLBACK_MAX_LINES 2000

typedef struct {
    cell_t *cells;
    int     cols;
} scroll_row_t;

static scroll_row_t scrollback[SCROLLBACK_MAX_LINES];
static int scrollback_head  = 0;
static int scrollback_count = 0;
static int scroll_offset    = 0;

static void scrollback_push_row(const cell_t *row, int num_cols) {
    scroll_row_t *slot = &scrollback[scrollback_head];
    if (slot->cols != num_cols) {
        free(slot->cells);
        slot->cells = malloc((size_t)num_cols * sizeof(cell_t));
        slot->cols  = num_cols;
    }
    if (slot->cells) {
        memcpy(slot->cells, row, (size_t)num_cols * sizeof(cell_t));
    }
    scrollback_head = (scrollback_head + 1) % SCROLLBACK_MAX_LINES;
    if (scrollback_count < SCROLLBACK_MAX_LINES) {
        scrollback_count++;
    }
    if (scroll_offset > 0 && scroll_offset < scrollback_count) {
        scroll_offset++;
    }
}

static void clear_scrollback(void) {
    for (int i = 0; i < SCROLLBACK_MAX_LINES; i++) {
        if (scrollback[i].cells) {
            free(scrollback[i].cells);
            scrollback[i].cells = NULL;
            scrollback[i].cols  = 0;
        }
    }
    scrollback_head  = 0;
    scrollback_count = 0;
    scroll_offset    = 0;
}

static cell_t get_visible_cell(int r, int c) {
    if (scroll_offset == 0) {
        if (r >= 0 && r < rows && c >= 0 && c < cols)
            return term_buffer[r * cols + c];
        return make_blank_cell();
    }

    int view_index = (scrollback_count - scroll_offset) + r;
    if (view_index < 0) {
        return make_blank_cell();
    } else if (view_index < scrollback_count) {
        int oldest = (scrollback_head - scrollback_count + SCROLLBACK_MAX_LINES) % SCROLLBACK_MAX_LINES;
        int slot   = (oldest + view_index) % SCROLLBACK_MAX_LINES;
        if (c >= 0 && c < scrollback[slot].cols && scrollback[slot].cells) {
            return scrollback[slot].cells[c];
        }
        return make_blank_cell();
    } else {
        int live_r = view_index - scrollback_count;
        if (live_r >= 0 && live_r < rows && c >= 0 && c < cols) {
            return term_buffer[live_r * cols + c];
        }
        return make_blank_cell();
    }
}

uint8_t running = 1, needs_render = 1;
static int master_fd = -1;

/* ── ANSI parser ────────────────────────────────────────────────────────── */

typedef enum {
    ANSI_NORMAL,    /* regular text                                    */
    ANSI_ESC,       /* received ESC (0x1B)                             */
    ANSI_CSI,       /* received ESC [  — collecting params             */
    ANSI_OSC,       /* received ESC ]  — collecting OSC payload        */
    ANSI_OSC_ESC,   /* saw ESC inside OSC — peeking for ST (ESC \)     */
    ANSI_OSC_CSI,   /* skipping a CSI sequence embedded inside OSC     */
} ansi_state_t;

static ansi_state_t ansi_state = ANSI_NORMAL;
static char  csi_buf[64];
static int   csi_len = 0;
static char  osc_buf[512];
static int   osc_len = 0;

/* SDL window handle — global so dispatch_osc() can set the title */
static SDL_Window *window = NULL;

/* Parse up to two numeric parameters from a CSI param string.
 * Returns the number of params found. Missing params default to `def`. */
static int parse_csi_params(const char *s, int *p1, int *p2, int def) {
    *p1 = def; *p2 = def;
    /* skip leading private-mode markers like '?' */
    while (*s && (*s < '0' || *s > '9')) s++;
    int count = 0;
    if (*s) { *p1 = atoi(s); count = 1; }
    while (*s && *s != ';') s++;
    if (*s == ';') {
        s++;
        while (*s && (*s < '0' || *s > '9')) s++;
        if (*s) { *p2 = atoi(s); count = 2; }
    }
    return count;
}

static void erase_line(int mode) {
    int start = (mode == 1) ? 0    : (int)cursor.x;
    int end   = (mode == 0) ? cols : (int)cursor.x + 1;
    if (mode == 2) { start = 0; end = cols; }
    for (int i = start; i < end && i < cols; i++)
        term_buffer[cursor.y * cols + i] = make_blank_cell();
}

static void erase_display(int mode) {
    if (mode == 3) {
        /* Clear scrollback buffer (xterm ESC[3J) */
        clear_scrollback();
    } else if (mode == 2) {
        for (size_t i = 0; i < (size_t)cols * (size_t)rows; i++)
            term_buffer[i] = make_blank_cell();
    } else if (mode == 0) {
        /* cursor to end of screen */
        erase_line(0);
        for (int r = (int)cursor.y + 1; r < rows; r++)
            for (int c = 0; c < cols; c++)
                term_buffer[r * cols + c] = make_blank_cell();
    } else if (mode == 1) {
        /* beginning of screen to cursor */
        for (int r = 0; r < (int)cursor.y; r++)
            for (int c = 0; c < cols; c++)
                term_buffer[r * cols + c] = make_blank_cell();
        erase_line(1);
    }
}

/* Handle a completed OSC (Operating System Command) sequence.
 * buf is the NUL-terminated content between ESC] and the terminator.
 * Format: Ps ; Pt   — Ps is numeric command, Pt is the payload string.
 *
 * OSC 0 : set icon name AND window title
 * OSC 1 : set icon name (ignored)
 * OSC 2 : set window title
 */
static void dispatch_osc(const char *buf) {
    int ps = 0;
    const char *pt = buf;

    while (*pt >= '0' && *pt <= '9')
        ps = ps * 10 + (*pt++ - '0');

    if (*pt != ';') return; /* malformed — no separator */
    pt++;                   /* skip ';' */

    if ((ps == 0 || ps == 2) && window) {
        /* Strip any remaining non-printable bytes (e.g. CSI param remnants) */
        char clean[512];
        int  ci = 0;
        for (const char *s = pt; *s && ci < 511; s++)
            if ((unsigned char)*s >= 0x20) clean[ci++] = *s;
        clean[ci] = '\0';

        char title[560];
        if (ci > 0) {
            snprintf(title, sizeof(title), "fatty - %s", clean);
        } else {
            snprintf(title, sizeof(title), "fatty");
        }
        SDL_SetWindowTitle(window, title);
    }
    /* OSC 1 (icon name) and all others are silently ignored */
}

/* Handle SGR (Select Graphic Rendition) attribute sequences. */
static void handle_sgr(const char *params) {
    if (!params || !*params) {
        current_fg   = ATTR_DEFAULT_FG;
        current_bg   = ATTR_DEFAULT_BG;
        current_bold = 0;
        return;
    }
    const char *p = params;
    while (*p) {
        while (*p && (*p < '0' || *p > '9')) p++;
        if (!*p) break;
        int code = atoi(p);
        while (*p >= '0' && *p <= '9') p++;

        if (code == 0) {
            current_fg   = ATTR_DEFAULT_FG;
            current_bg   = ATTR_DEFAULT_BG;
            current_bold = 0;
        } else if (code == 1) {
            current_bold = 1;
        } else if (code == 22) {
            current_bold = 0;
        } else if (code >= 30 && code <= 37) {
            current_fg = (uint8_t)(code - 30);
        } else if (code == 38) {
            /* Extended foreground: 38;5;idx */
            if (*p == ';') p++;
            int type = atoi(p);
            while (*p >= '0' && *p <= '9') p++;
            if (type == 5) {
                if (*p == ';') p++;
                int col_idx = atoi(p);
                while (*p >= '0' && *p <= '9') p++;
                if (col_idx >= 0 && col_idx < 16) {
                    current_fg = (uint8_t)col_idx;
                }
            }
        } else if (code == 39) {
            current_fg = ATTR_DEFAULT_FG;
        } else if (code >= 40 && code <= 47) {
            current_bg = (uint8_t)(code - 40);
        } else if (code == 48) {
            /* Extended background: 48;5;idx */
            if (*p == ';') p++;
            int type = atoi(p);
            while (*p >= '0' && *p <= '9') p++;
            if (type == 5) {
                if (*p == ';') p++;
                int col_idx = atoi(p);
                while (*p >= '0' && *p <= '9') p++;
                if (col_idx >= 0 && col_idx < 16) {
                    current_bg = (uint8_t)col_idx;
                }
            }
        } else if (code == 49) {
            current_bg = ATTR_DEFAULT_BG;
        } else if (code >= 90 && code <= 97) {
            current_fg = (uint8_t)(8 + (code - 90));
        } else if (code >= 100 && code <= 107) {
            current_bg = (uint8_t)(8 + (code - 100));
        }
        if (*p == ';') p++;
    }
}

/* Dispatch a completed CSI sequence.
 * `params` is the NUL-terminated parameter/intermediate string.
 * `final`  is the final byte that terminated the sequence. */
static void dispatch_csi(const char *params, char final) {
    int p1, p2;
    parse_csi_params(params, &p1, &p2, 0);

    switch (final) {
    /* ── Cursor movement ──────────────────────────────────────── */
    case 'A': /* Cursor Up */
        { int n = p1 ? p1 : 1;
          cursor.y = (cursor.y >= (uint32_t)n) ? cursor.y - n : 0; }
        break;
    case 'B': /* Cursor Down */
        { int n = p1 ? p1 : 1;
          if ((int)cursor.y + n < rows) cursor.y += n;
          else cursor.y = (uint32_t)(rows - 1); }
        break;
    case 'C': /* Cursor Forward */
        { int n = p1 ? p1 : 1;
          cursor.x = (int)cursor.x + n < cols ? cursor.x + n : (uint32_t)(cols - 1); }
        break;
    case 'D': /* Cursor Back */
        { int n = p1 ? p1 : 1;
          cursor.x = (cursor.x >= (uint32_t)n) ? cursor.x - n : 0; }
        break;
    case 'E': /* Cursor Next Line */
        { int n = p1 ? p1 : 1;
          cursor.x = 0;
          cursor.y = (int)cursor.y + n < rows ? cursor.y + n : (uint32_t)(rows - 1); }
        break;
    case 'F': /* Cursor Previous Line */
        { int n = p1 ? p1 : 1;
          cursor.x = 0;
          cursor.y = (cursor.y >= (uint32_t)n) ? cursor.y - n : 0; }
        break;
    case 'G': /* Cursor Horizontal Absolute */
        { int col = p1 ? p1 - 1 : 0;
          cursor.x = col < cols ? (uint32_t)col : (uint32_t)(cols - 1); }
        break;
    case 'H': /* Cursor Position  ESC[row;colH  (1-based) */
    case 'f': /* same as H */
        { int row = p1 ? p1 - 1 : 0;
          int col = p2 ? p2 - 1 : 0;
          cursor.y = row < rows ? (uint32_t)row : (uint32_t)(rows - 1);
          cursor.x = col < cols ? (uint32_t)col : (uint32_t)(cols - 1); }
        break;
    case 'd': /* Line Position Absolute (row, 1-based) */
        { int row = p1 ? p1 - 1 : 0;
          cursor.y = row < rows ? (uint32_t)row : (uint32_t)(rows - 1); }
        break;

    /* ── Erase ────────────────────────────────────────────────── */
    case 'J': /* Erase in Display */
        erase_display(p1);
        break;
    case 'K': /* Erase in Line */
        erase_line(p1);
        break;

    /* ── Attributes / modes ───────────────────────────────────── */
    case 'm': /* SGR — colors & attributes */
        handle_sgr(params);
        break;
    case 'h': /* Set mode / private mode on  */
    case 'l': /* Reset mode / private mode off */
        break;
    case 'r': /* DECSTBM — set scrolling region (ignored) */
        break;
    case 'n': /* Device Status Report */
        break;

    /* ── Everything else: silently consume ───────────────────── */
    default:
        break;
    }
}

/* ── Scroll & resize ────────────────────────────────────────────────────── */

/* Shift the entire screen up by one row, clearing the bottom line. */
static void scroll_up(void) {
    /* Preserve top row in scrollback ring buffer */
    scrollback_push_row(term_buffer, cols);

    memmove(term_buffer,
            term_buffer + cols,
            (size_t)(rows - 1) * (size_t)cols * sizeof(cell_t));
    for (int i = 0; i < cols; i++) {
        term_buffer[(rows - 1) * cols + i] = make_blank_cell();
    }
}

/* Recompute grid dimensions from the new window pixel size, realloc the
 * term_buffer, clamp the cursor, and notify the PTY of the new winsize. */
static void resize_terminal(int new_w, int new_h) {
    int new_cols = (new_w - 16) / char_w;
    int new_rows = (new_h - 16) / char_h;
    win_width = new_w, win_height = new_h;
    if (new_cols < 1) new_cols = 1;
    if (new_rows < 1) new_rows = 1;
    if (new_cols == cols && new_rows == rows) return;

    cell_t *new_buf = calloc((size_t)new_cols * (size_t)new_rows, sizeof(cell_t));
    if (!new_buf) return; /* OOM — keep old buffer */

    for (size_t i = 0; i < (size_t)new_cols * (size_t)new_rows; i++) {
        new_buf[i] = make_blank_cell();
    }

    /* Copy as much existing content as fits into the new grid */
    int copy_rows = rows < new_rows ? rows : new_rows;
    int copy_cols = cols < new_cols ? cols : new_cols;
    for (int r = 0; r < copy_rows; r++)
        for (int c = 0; c < copy_cols; c++)
            new_buf[r * new_cols + c] = term_buffer[r * cols + c];

    free(term_buffer);
    term_buffer = new_buf;
    cols = new_cols;
    rows = new_rows;

    /* Clamp cursor to new bounds */
    if ((int)cursor.x >= cols) cursor.x = (uint32_t)(cols - 1);
    if ((int)cursor.y >= rows) cursor.y = (uint32_t)(rows - 1);

    /* Signal PTY of new dimensions */
    struct winsize ws = {
        .ws_row    = (unsigned short)rows,
        .ws_col    = (unsigned short)cols,
        .ws_xpixel = (unsigned short)new_w,
        .ws_ypixel = (unsigned short)new_h,
    };
    ioctl(master_fd, TIOCSWINSZ, &ws);
}

/* Adjust font size and recalculate terminal grid dimensions.
 * delta > 0: zoom in, delta < 0: zoom out, delta == 0: reset zoom */
static void zoom_terminal(int delta) {
    int new_size;
    if (delta == 0) {
        new_size = DEFAULT_FONT_SIZE;
    } else {
        new_size = font_size + delta;
    }
    if (new_size < 8) new_size = 8;
    if (new_size > 72) new_size = 72;
    if (new_size == font_size) return;

    font_size = new_size;
    if (font) TTF_SetFontSize(font, font_size);
    if (font_bold) {
        TTF_SetFontSize(font_bold, font_size);
        TTF_SetFontStyle(font_bold, TTF_STYLE_BOLD);
    }
    if (font_cjk) TTF_SetFontSize(font_cjk, font_size);
    if (font_emoji) TTF_SetFontSize(font_emoji, font_size);
    clear_glyph_cache();
    TTF_SizeText(font, "M", &char_w, &char_h);
    if (char_w <= 0 || char_h <= 0) {
        char_w = 9;
        char_h = 18;
    }

    int w = 0, h = 0;
    if (window) {
        SDL_GetWindowSize(window, &w, &h);
    }
    if (w <= 0 || h <= 0) {
        w = win_width;
        h = win_height;
    }
    if (w > 0 && h > 0) {
        resize_terminal(w, h);
    }
    needs_render = 1;
}

/* ── PTY write ──────────────────────────────────────────────────────────── */

static void pty_write(const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(master_fd, buf, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        buf += (size_t)n;
        len -= (size_t)n;
    }
}

/* ── Event handler ──────────────────────────────────────────────────────── */

void event_handler(){

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            running = 0;
            break;

        } else if(ev.type == SDL_WINDOWEVENT){
            if(ev.window.event == SDL_WINDOWEVENT_CLOSE){
                running = 0;
                break;
            } else if (ev.window.event == SDL_WINDOWEVENT_RESIZED) {
                resize_terminal(ev.window.data1, ev.window.data2);
                needs_render = 1;
            } 
        } else if (ev.type == SDL_MOUSEWHEEL) {
            int delta = ev.wheel.y;
            if (delta != 0) {
                scroll_offset += delta * 3;
                if (scroll_offset > scrollback_count) scroll_offset = scrollback_count;
                if (scroll_offset < 0) scroll_offset = 0;
                needs_render = 1;
            }
        } else if (ev.type == SDL_TEXTINPUT) {
            /* Drop text events produced while Ctrl is held (zoom shortcuts, Ctrl-combos) */
            if (SDL_GetModState() & KMOD_CTRL) {
                continue;
            }
            /* Reset scrollback on typing to snap view to live bottom */
            if (scroll_offset > 0) {
                scroll_offset = 0;
                needs_render = 1;
            }
            /* Forward the UTF-8 string produced by this keypress to the PTY. */
            pty_write(ev.text.text, strlen(ev.text.text));
        } else if (ev.type == SDL_KEYDOWN) {
            SDL_Keycode sym = ev.key.keysym.sym;
            SDL_Keymod  mod = SDL_GetModState();
            int ctrl  = (mod & KMOD_CTRL)  != 0;
            int shift = (mod & KMOD_SHIFT) != 0;

            /* Shift+PageUp / Shift+PageDown / Shift+Up / Shift+Down for viewport scroll */
            if (shift && sym == SDLK_PAGEUP) {
                scroll_offset += rows / 2;
                if (scroll_offset > scrollback_count) scroll_offset = scrollback_count;
                needs_render = 1;
                continue;
            }
            if (shift && sym == SDLK_PAGEDOWN) {
                scroll_offset -= rows / 2;
                if (scroll_offset < 0) scroll_offset = 0;
                needs_render = 1;
                continue;
            }
            if (shift && sym == SDLK_UP) {
                scroll_offset += 1;
                if (scroll_offset > scrollback_count) scroll_offset = scrollback_count;
                needs_render = 1;
                continue;
            }
            if (shift && sym == SDLK_DOWN) {
                scroll_offset -= 1;
                if (scroll_offset < 0) scroll_offset = 0;
                needs_render = 1;
                continue;
            }
            /* Zoom in: Ctrl + Plus / Ctrl + Equals */
            if (ctrl && (sym == SDLK_PLUS || sym == SDLK_EQUALS || sym == SDLK_KP_PLUS)) {
                zoom_terminal(2);
                continue;
            }
            /* Zoom out: Ctrl + Minus */
            if (ctrl && (sym == SDLK_MINUS || sym == SDLK_KP_MINUS)) {
                zoom_terminal(-2);
                continue;
            }
            /* Reset zoom: Ctrl + 0 */
            if (ctrl && (sym == SDLK_0 || sym == SDLK_KP_0)) {
                zoom_terminal(0);
                continue;
            }
            /* Reset scroll offset on active typing/command entry */
            if (scroll_offset > 0 && !shift) {
                scroll_offset = 0;
                needs_render = 1;
            }

            /* Ctrl+letter → send the corresponding control byte (0x01–0x1A).
             * SDL_TEXTINPUT won't fire for these, so we handle them here. */
            if (ctrl && sym >= SDLK_a && sym <= SDLK_z) {
                char cb = (char)(sym - SDLK_a + 1);
                pty_write(&cb, 1);
                continue;
            }

            /* Special / non-printing keys that produce no SDL_TEXTINPUT event. */
            const char *seq = NULL;
            switch (sym) {
            case SDLK_RETURN:
            case SDLK_KP_ENTER: seq = "\r";       break; /* CR */
            case SDLK_BACKSPACE: seq = "\x7f";    break; /* DEL (PTY erase char) */
            case SDLK_TAB:       seq = "\t";      break;
            case SDLK_ESCAPE:    seq = "\x1b";    break;
            case SDLK_UP:        seq = "\x1b[A";  break;
            case SDLK_DOWN:      seq = "\x1b[B";  break;
            case SDLK_RIGHT:     seq = "\x1b[C";  break;
            case SDLK_LEFT:      seq = "\x1b[D";  break;
            case SDLK_HOME:      seq = "\x1b[H";  break;
            case SDLK_END:       seq = "\x1b[F";  break;
            case SDLK_DELETE:    seq = "\x1b[3~"; break;
            case SDLK_PAGEUP:    seq = "\x1b[5~"; break;
            case SDLK_PAGEDOWN:  seq = "\x1b[6~"; break;
            case SDLK_F11:       fullscreen = !fullscreen; break;
            default: break;
            }
            if (seq) pty_write(seq, strlen(seq));
        }
    }
}

/* ── PTY reader ─────────────────────────────────────────────────────────── */

static uint32_t utf8_codepoint = 0;
static int      utf8_expected  = 0;

uint8_t read_pty(char* pty_buffer){
    ssize_t n = read(master_fd, pty_buffer, 4095);
    if (n > 0) {
        pty_buffer[n] = '\0';

        for(ssize_t i = 0; i < n; i++){
            unsigned char ch = (unsigned char)pty_buffer[i];

            /* ── Feed the ANSI state machine ── */
            switch (ansi_state) {

            case ANSI_ESC:
                if (ch == '[') {
                    ansi_state = ANSI_CSI;
                    csi_len = 0;
                } else if (ch == ']') {          /* OSC — Operating System Command */
                    ansi_state = ANSI_OSC;
                    osc_len = 0;
                } else if (ch == 'c') {          /* RIS — full reset */
                    erase_display(2);
                    cursor.x = cursor.y = 0;
                    utf8_expected = 0;
                    utf8_codepoint = 0;
                    ansi_state = ANSI_NORMAL;
                } else if (ch == 'M') {          /* RI — reverse index (cursor up) */
                    if (cursor.y > 0) cursor.y--;
                    ansi_state = ANSI_NORMAL;
                } else if (ch == '=' || ch == '>') { /* keypad mode, ignore */
                    ansi_state = ANSI_NORMAL;
                } else if (ch == '(' || ch == ')') { /* charset designator — drop next byte */
                    ansi_state = ANSI_NORMAL;
                } else if (ch == '\\') {         /* ST (String Terminator) after OSC — ignore lone ST */
                    ansi_state = ANSI_NORMAL;
                } else {
                    ansi_state = ANSI_NORMAL; /* unknown two-char ESC seq */
                }
                continue;

            case ANSI_OSC:
                if (ch == 0x07) {                /* BEL — terminate OSC */
                    osc_buf[osc_len] = '\0';
                    dispatch_osc(osc_buf);
                    ansi_state = ANSI_NORMAL;
                } else if (ch == 0x1B) {         /* ESC inside OSC — peek for ST */
                    ansi_state = ANSI_OSC_ESC;
                } else {
                    if (osc_len < 511) osc_buf[osc_len++] = (char)ch;
                }
                continue;

            case ANSI_OSC_ESC:
                if (ch == '\\') {               /* ESC \ — proper String Terminator */
                    osc_buf[osc_len] = '\0';
                    dispatch_osc(osc_buf);
                    ansi_state = ANSI_NORMAL;
                } else if (ch == '[') {         /* ESC [ inside OSC — a CSI color code, skip it */
                    ansi_state = ANSI_OSC_CSI;
                } else {
                    /* Other ESC-x inside OSC — drop the ESC, put this byte back */
                    if (osc_len < 511) osc_buf[osc_len++] = (char)ch;
                    ansi_state = ANSI_OSC;
                }
                continue;

            case ANSI_OSC_CSI:
                /* Consume CSI param/intermediate bytes until the final byte (0x40–0x7E) */
                if (ch >= 0x40 && ch <= 0x7E)
                    ansi_state = ANSI_OSC; /* CSI done — resume OSC collection */
                continue;

            case ANSI_CSI:
                /* Collect parameter/intermediate bytes (0x20–0x3F).
                 * The final byte is in range 0x40–0x7E. */
                if (ch >= 0x20 && ch <= 0x3F) {
                    if (csi_len < 63) csi_buf[csi_len++] = (char)ch;
                    continue;
                }
                if (ch >= 0x40 && ch <= 0x7E) {
                    csi_buf[csi_len] = '\0';
                    dispatch_csi(csi_buf, (char)ch);
                }
                /* anything else (malformed) — return to normal */
                ansi_state = ANSI_NORMAL;
                continue;

            case ANSI_NORMAL:
                break; /* fall through to character handling below */
            }

            /* ── Handle C0 control characters and ESC ── */
            if (ch == 0x1B) {
                utf8_expected = 0;
                utf8_codepoint = 0;
                ansi_state = ANSI_ESC;
                continue;
            }
            if (ch == '\r') {
                utf8_expected = 0;
                utf8_codepoint = 0;
                cursor.x = 0;
                continue;
            }
            if (ch == '\n') {
                utf8_expected = 0;
                utf8_codepoint = 0;
                /* LF: move cursor down; scroll the screen if at the last row */
                if ((int)cursor.y + 1 < rows) {
                    cursor.y++;
                } else {
                    scroll_up(); /* cursor.y stays at rows-1 */
                }
                continue;
            }
            if (ch == '\b') {
                utf8_expected = 0;
                utf8_codepoint = 0;
                if (cursor.x > 0) {
                    cursor.x--;
                    /* If backed up onto trailing cell of wide char, back up one more */
                    if ((int)cursor.y < rows && cursor.x > 0) {
                        cell_t prev = term_buffer[cursor.y * cols + cursor.x];
                        if (prev.width == 0) cursor.x--;
                    }
                }
                continue;
            }
            if (ch == '\t') {
                utf8_expected = 0;
                utf8_codepoint = 0;
                /* advance to next 8-column tab stop */
                cursor.x = (cursor.x + 8) & ~7u;
                if ((int)cursor.x >= cols) cursor.x = (uint32_t)(cols - 1);
                continue;
            }
            if (ch < 0x20 || ch == 0x7F) {
                /* other non-printable control bytes — reset UTF-8 state and ignore */
                utf8_expected = 0;
                utf8_codepoint = 0;
                continue;
            }

            /* ── UTF-8 decoding state machine ── */
            uint32_t cp = 0;

            if (utf8_expected > 0) {
                if ((ch & 0xC0) == 0x80) {
                    /* Continuation byte */
                    utf8_codepoint = (utf8_codepoint << 6) | (ch & 0x3F);
                    utf8_expected--;
                    if (utf8_expected == 0) {
                        cp = utf8_codepoint;
                    } else {
                        continue; /* Waiting for more continuation bytes */
                    }
                } else {
                    /* Interrupted sequence: reset and reprocess this byte */
                    utf8_expected = 0;
                    utf8_codepoint = 0;
                }
            }

            if (cp == 0) {
                if (ch < 0x80) {
                    /* Single-byte ASCII */
                    cp = (uint32_t)ch;
                } else if ((ch & 0xE0) == 0xC0 && ch >= 0xC2) {
                    /* 2-byte sequence lead */
                    utf8_codepoint = ch & 0x1F;
                    utf8_expected  = 1;
                    continue;
                } else if ((ch & 0xF0) == 0xE0) {
                    /* 3-byte sequence lead */
                    utf8_codepoint = ch & 0x0F;
                    utf8_expected  = 2;
                    continue;
                } else if ((ch & 0xF8) == 0xF0 && ch <= 0xF4) {
                    /* 4-byte sequence lead */
                    utf8_codepoint = ch & 0x07;
                    utf8_expected  = 3;
                    continue;
                } else {
                    /* Invalid lead or orphan continuation byte — drop */
                    continue;
                }
            }

            /* Replace surrogate halves or out-of-range codepoints */
            if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
                cp = 0xFFFD; /* Unicode replacement character */
            }

            /* Determine character column width */
            int w = wcwidth((wchar_t)cp);
            if (w < 0) {
                w = 1; /* Fallback for unclassified symbols / private use (e.g. Nerd Fonts / Powerline) */
            }
            if (w == 0) {
                /* Zero-width / combining mark: do not advance cursor */
                continue;
            }

            /* ── Write codepoint into cell buffer ── */
            if (w == 2) {
                /* Double-width character: occupies 2 terminal columns */
                if ((int)cursor.x + 1 >= cols) {
                    /* Cannot fit at end of row: pad with blank and wrap */
                    if ((int)cursor.x < cols && (int)cursor.y < rows) {
                        term_buffer[cursor.y * cols + cursor.x] = make_blank_cell();
                    }
                    cursor.x = 0;
                    if ((int)cursor.y + 1 < rows) {
                        cursor.y++;
                    } else {
                        scroll_up();
                    }
                }
                if ((int)cursor.y < rows) {
                    cell_t c1;
                    c1.codepoint = cp;
                    c1.fg        = current_fg;
                    c1.bg        = current_bg;
                    c1.bold      = current_bold;
                    c1.width     = 2;
                    term_buffer[cursor.y * cols + cursor.x] = c1;

                    cell_t c2;
                    c2.codepoint = 0;
                    c2.fg        = current_fg;
                    c2.bg        = current_bg;
                    c2.bold      = current_bold;
                    c2.width     = 0; /* trailing half */
                    term_buffer[cursor.y * cols + cursor.x + 1] = c2;

                    cursor.x += 2;
                }
            } else {
                /* Standard single-column character */
                if ((int)cursor.x >= cols) {
                    cursor.x = 0;
                    if ((int)cursor.y + 1 < rows) {
                        cursor.y++;
                    } else {
                        scroll_up();
                    }
                }
                if ((int)cursor.y < rows) {
                    cell_t cell;
                    cell.codepoint = cp;
                    cell.fg        = current_fg;
                    cell.bg        = current_bg;
                    cell.bold      = current_bold;
                    cell.width     = 1;
                    term_buffer[cursor.y * cols + cursor.x] = cell;
                    cursor.x++;
                }
            }
        }
        needs_render = 1;
        return 1;
    }
    return 0;
}

/* ── Procedural Block Element Renderer ──────────────────────────────────── */

static int render_block_element(SDL_Renderer *renderer, uint32_t cp, SDL_Rect cell_rect, SDL_Color fg) {
    if (cp < 0x2580 || cp > 0x259F) return 0;

    int x = cell_rect.x;
    int y = cell_rect.y;
    int w = cell_rect.w;
    int h = cell_rect.h;
    int half_w = w / 2;
    int half_h = h / 2;

    SDL_SetRenderDrawColor(renderer, fg.r, fg.g, fg.b, 255);

    switch (cp) {
    case 0x2588: /* Full block █ */
        SDL_RenderFillRect(renderer, &cell_rect);
        return 1;
    case 0x2580: { /* Upper half block ▀ */
        SDL_Rect r = { x, y, w, half_h };
        SDL_RenderFillRect(renderer, &r);
        return 1;
    }
    case 0x2584: { /* Lower half block ▄ */
        SDL_Rect r = { x, y + half_h, w, h - half_h };
        SDL_RenderFillRect(renderer, &r);
        return 1;
    }
    case 0x258C: { /* Left half block ▌ */
        SDL_Rect r = { x, y, half_w, h };
        SDL_RenderFillRect(renderer, &r);
        return 1;
    }
    case 0x2590: { /* Right half block ▐ */
        SDL_Rect r = { x + half_w, y, w - half_w, h };
        SDL_RenderFillRect(renderer, &r);
        return 1;
    }
    case 0x2581: case 0x2582: case 0x2583:
    case 0x2585: case 0x2586: case 0x2587: {
        int n = (int)(cp - 0x2580);
        int bh = (h * n + 4) / 8;
        if (bh < 1) bh = 1;
        SDL_Rect r = { x, y + h - bh, w, bh };
        SDL_RenderFillRect(renderer, &r);
        return 1;
    }
    case 0x2589: case 0x258A: case 0x258B:
    case 0x258D: case 0x258E: case 0x258F: {
        int n = (int)(0x2590 - cp);
        int bw = (w * n + 4) / 8;
        if (bw < 1) bw = 1;
        SDL_Rect r = { x, y, bw, h };
        SDL_RenderFillRect(renderer, &r);
        return 1;
    }
    case 0x2594: { /* Upper 1/8 block ▔ */
        int bh = (h * 1 + 4) / 8;
        if (bh < 1) bh = 1;
        SDL_Rect r = { x, y, w, bh };
        SDL_RenderFillRect(renderer, &r);
        return 1;
    }
    case 0x2595: { /* Right 1/8 block ▕ */
        int bw = (w * 1 + 4) / 8;
        if (bw < 1) bw = 1;
        SDL_Rect r = { x + w - bw, y, bw, h };
        SDL_RenderFillRect(renderer, &r);
        return 1;
    }
    case 0x2596: case 0x2597: case 0x2598: case 0x2599:
    case 0x259A: case 0x259B: case 0x259C: case 0x259D:
    case 0x259E: case 0x259F: {
        int mask = 0;
        switch (cp) {
            case 0x2596: mask = 0x4; break;
            case 0x2597: mask = 0x8; break;
            case 0x2598: mask = 0x1; break;
            case 0x2599: mask = 0xD; break;
            case 0x259A: mask = 0x9; break;
            case 0x259B: mask = 0x7; break;
            case 0x259C: mask = 0xB; break;
            case 0x259D: mask = 0x2; break;
            case 0x259E: mask = 0x6; break;
            case 0x259F: mask = 0xE; break;
        }
        if (mask & 0x1) { SDL_Rect r = { x, y, half_w, half_h }; SDL_RenderFillRect(renderer, &r); }
        if (mask & 0x2) { SDL_Rect r = { x + half_w, y, w - half_w, half_h }; SDL_RenderFillRect(renderer, &r); }
        if (mask & 0x4) { SDL_Rect r = { x, y + half_h, half_w, h - half_h }; SDL_RenderFillRect(renderer, &r); }
        if (mask & 0x8) { SDL_Rect r = { x + half_w, y + half_h, w - half_w, h - half_h }; SDL_RenderFillRect(renderer, &r); }
        return 1;
    }
    case 0x2591: case 0x2592: case 0x2593: {
        Uint8 alpha = (cp == 0x2591) ? 64 : (cp == 0x2592 ? 128 : 192);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, fg.r, fg.g, fg.b, alpha);
        SDL_RenderFillRect(renderer, &cell_rect);
        return 1;
    }
    default:
        return 0;
    }
}

/* ── Procedural Box-Drawing Line Renderer ───────────────────────────────── */

enum BoxLineStyle {
    BOX_NONE = 0,
    BOX_LIGHT = 1,
    BOX_HEAVY = 2,
    BOX_DOUBLE = 3
};

typedef struct {
    uint8_t up, down, left, right, rounded;
} box_spec_t;

static box_spec_t get_box_spec(uint32_t cp) {
    box_spec_t s = {0, 0, 0, 0, 0};
    switch (cp) {
    case 0x2500: s.left = BOX_LIGHT; s.right = BOX_LIGHT; break; /* ─ */
    case 0x2501: s.left = BOX_HEAVY; s.right = BOX_HEAVY; break; /* ━ */
    case 0x2502: s.up   = BOX_LIGHT; s.down  = BOX_LIGHT; break; /* │ */
    case 0x2503: s.up   = BOX_HEAVY; s.down  = BOX_HEAVY; break; /* ┃ */

    case 0x250C: s.down = BOX_LIGHT; s.right = BOX_LIGHT; break; /* ┌ */
    case 0x250D: s.down = BOX_LIGHT; s.right = BOX_HEAVY; break;
    case 0x250E: s.down = BOX_HEAVY; s.right = BOX_LIGHT; break;
    case 0x250F: s.down = BOX_HEAVY; s.right = BOX_HEAVY; break; /* ┏ */

    case 0x2510: s.down = BOX_LIGHT; s.left  = BOX_LIGHT; break; /* ┐ */
    case 0x2511: s.down = BOX_LIGHT; s.left  = BOX_HEAVY; break;
    case 0x2512: s.down = BOX_HEAVY; s.left  = BOX_LIGHT; break;
    case 0x2513: s.down = BOX_HEAVY; s.left  = BOX_HEAVY; break; /* ┓ */

    case 0x2514: s.up   = BOX_LIGHT; s.right = BOX_LIGHT; break; /* └ */
    case 0x2515: s.up   = BOX_LIGHT; s.right = BOX_HEAVY; break;
    case 0x2516: s.up   = BOX_HEAVY; s.right = BOX_LIGHT; break;
    case 0x2517: s.up   = BOX_HEAVY; s.right = BOX_HEAVY; break; /* ┗ */

    case 0x2518: s.up   = BOX_LIGHT; s.left  = BOX_LIGHT; break; /* ┘ */
    case 0x2519: s.up   = BOX_LIGHT; s.left  = BOX_HEAVY; break;
    case 0x251A: s.up   = BOX_HEAVY; s.left  = BOX_LIGHT; break;
    case 0x251B: s.up   = BOX_HEAVY; s.left  = BOX_HEAVY; break; /* ┛ */

    case 0x251C: s.up = BOX_LIGHT; s.down = BOX_LIGHT; s.right = BOX_LIGHT; break; /* ├ */
    case 0x2524: s.up = BOX_LIGHT; s.down = BOX_LIGHT; s.left  = BOX_LIGHT; break; /* ┤ */
    case 0x252C: s.left = BOX_LIGHT; s.right = BOX_LIGHT; s.down = BOX_LIGHT; break; /* ┬ */
    case 0x2534: s.left = BOX_LIGHT; s.right = BOX_LIGHT; s.up   = BOX_LIGHT; break; /* ┴ */
    case 0x253C: s.up = BOX_LIGHT; s.down = BOX_LIGHT; s.left = BOX_LIGHT; s.right = BOX_LIGHT; break; /* ┼ */

    /* Mixed light / heavy tees and crosses */
    case 0x251D: s.up = BOX_LIGHT; s.down = BOX_LIGHT; s.right = BOX_HEAVY; break;
    case 0x251E: case 0x251F: case 0x2520: case 0x2521: case 0x2522: case 0x2523:
        s.up = BOX_HEAVY; s.down = BOX_HEAVY; s.right = BOX_HEAVY; break;
    case 0x2525: s.up = BOX_LIGHT; s.down = BOX_LIGHT; s.left = BOX_HEAVY; break;
    case 0x2526: case 0x2527: case 0x2528: case 0x2529: case 0x252A: case 0x252B:
        s.up = BOX_HEAVY; s.down = BOX_HEAVY; s.left = BOX_HEAVY; break;
    case 0x252D: case 0x252E: case 0x252F: case 0x2530: case 0x2531: case 0x2532: case 0x2533:
        s.left = BOX_HEAVY; s.right = BOX_HEAVY; s.down = BOX_HEAVY; break;
    case 0x2535: case 0x2536: case 0x2537: case 0x2538: case 0x2539: case 0x253A: case 0x253B:
        s.left = BOX_HEAVY; s.right = BOX_HEAVY; s.up = BOX_HEAVY; break;
    case 0x253D: case 0x253E: case 0x253F: case 0x2540: case 0x2541: case 0x2542:
    case 0x2543: case 0x2544: case 0x2545: case 0x2546: case 0x2547: case 0x2548:
    case 0x2549: case 0x254A: case 0x254B:
        s.up = BOX_HEAVY; s.down = BOX_HEAVY; s.left = BOX_HEAVY; s.right = BOX_HEAVY; break;

    /* Double lines & double corners */
    case 0x2550: s.left = BOX_DOUBLE; s.right = BOX_DOUBLE; break; /* ═ */
    case 0x2551: s.up   = BOX_DOUBLE; s.down  = BOX_DOUBLE; break; /* ║ */
    case 0x2552: s.down = BOX_LIGHT;  s.right = BOX_DOUBLE; break; /* ╒ */
    case 0x2553: s.down = BOX_DOUBLE; s.right = BOX_LIGHT;  break; /* ╓ */
    case 0x2554: s.down = BOX_DOUBLE; s.right = BOX_DOUBLE; break; /* ╔ */
    case 0x2555: s.down = BOX_LIGHT;  s.left  = BOX_DOUBLE; break; /* ╕ */
    case 0x2556: s.down = BOX_DOUBLE; s.left  = BOX_LIGHT;  break; /* ╖ */
    case 0x2557: s.down = BOX_DOUBLE; s.left  = BOX_DOUBLE; break; /* ╗ */
    case 0x2558: s.up   = BOX_LIGHT;  s.right = BOX_DOUBLE; break; /* ╘ */
    case 0x2559: s.up   = BOX_DOUBLE; s.right = BOX_LIGHT;  break; /* ╙ */
    case 0x255A: s.up   = BOX_DOUBLE; s.right = BOX_DOUBLE; break; /* ╚ */
    case 0x255B: s.up   = BOX_LIGHT;  s.left  = BOX_DOUBLE; break; /* ╛ */
    case 0x255C: s.up   = BOX_DOUBLE; s.left  = BOX_LIGHT;  break; /* ╜ */
    case 0x255D: s.up   = BOX_DOUBLE; s.left  = BOX_DOUBLE; break; /* ╝ */
    case 0x255E: case 0x255F: case 0x2560: s.up = BOX_DOUBLE; s.down = BOX_DOUBLE; s.right = BOX_DOUBLE; break; /* ╠ */
    case 0x2561: case 0x2562: case 0x2563: s.up = BOX_DOUBLE; s.down = BOX_DOUBLE; s.left  = BOX_DOUBLE; break; /* ╣ */
    case 0x2564: case 0x2565: case 0x2566: s.left = BOX_DOUBLE; s.right = BOX_DOUBLE; s.down = BOX_DOUBLE; break; /* ╦ */
    case 0x2567: case 0x2568: case 0x2569: s.left = BOX_DOUBLE; s.right = BOX_DOUBLE; s.up   = BOX_DOUBLE; break; /* ╩ */
    case 0x256A: case 0x256B: case 0x256C: s.up = BOX_DOUBLE; s.down = BOX_DOUBLE; s.left = BOX_DOUBLE; s.right = BOX_DOUBLE; break; /* ╬ */

    /* Rounded corners */
    case 0x256D: s.down = BOX_LIGHT; s.right = BOX_LIGHT; s.rounded = 1; break; /* ╭ */
    case 0x256E: s.down = BOX_LIGHT; s.left  = BOX_LIGHT; s.rounded = 1; break; /* ╮ */
    case 0x256F: s.up   = BOX_LIGHT; s.left  = BOX_LIGHT; s.rounded = 1; break; /* ╯ */
    case 0x2570: s.up   = BOX_LIGHT; s.right = BOX_LIGHT; s.rounded = 1; break; /* ╰ */

    /* Dashed lines (render continuous for solid grid connections) */
    case 0x2504: case 0x2505: case 0x2508: case 0x2509: case 0x250A: case 0x250B: case 0x254C: case 0x254D:
        s.left = BOX_LIGHT; s.right = BOX_LIGHT; break;
    case 0x2506: case 0x2507: case 0x254E: case 0x254F:
        s.up = BOX_LIGHT; s.down = BOX_LIGHT; break;

    /* Half lines */
    case 0x2574: s.left  = BOX_LIGHT; break;
    case 0x2575: s.up    = BOX_LIGHT; break;
    case 0x2576: s.right = BOX_LIGHT; break;
    case 0x2577: s.down  = BOX_LIGHT; break;
    case 0x2578: s.left  = BOX_HEAVY; break;
    case 0x2579: s.up    = BOX_HEAVY; break;
    case 0x257A: s.right = BOX_HEAVY; break;
    case 0x257B: s.down  = BOX_HEAVY; break;
    case 0x257C: s.left = BOX_LIGHT; s.right = BOX_HEAVY; break;
    case 0x257D: s.up   = BOX_LIGHT; s.down  = BOX_HEAVY; break;
    case 0x257E: s.left = BOX_HEAVY; s.right = BOX_LIGHT; break;
    case 0x257F: s.up   = BOX_HEAVY; s.down  = BOX_LIGHT; break;

    default: break;
    }
    return s;
}

static int render_box_drawing(SDL_Renderer *ren, uint32_t cp, SDL_Rect cr, SDL_Color fg) {
    if (cp < 0x2500 || cp > 0x257F) return 0;

    /* Diagonals */
    if (cp == 0x2571) { /* ╱ */
        SDL_SetRenderDrawColor(ren, fg.r, fg.g, fg.b, 255);
        SDL_RenderDrawLine(ren, cr.x + cr.w - 1, cr.y, cr.x, cr.y + cr.h - 1);
        return 1;
    }
    if (cp == 0x2572) { /* ╲ */
        SDL_SetRenderDrawColor(ren, fg.r, fg.g, fg.b, 255);
        SDL_RenderDrawLine(ren, cr.x, cr.y, cr.x + cr.w - 1, cr.y + cr.h - 1);
        return 1;
    }
    if (cp == 0x2573) { /* ╳ */
        SDL_SetRenderDrawColor(ren, fg.r, fg.g, fg.b, 255);
        SDL_RenderDrawLine(ren, cr.x + cr.w - 1, cr.y, cr.x, cr.y + cr.h - 1);
        SDL_RenderDrawLine(ren, cr.x, cr.y, cr.x + cr.w - 1, cr.y + cr.h - 1);
        return 1;
    }

    box_spec_t s = get_box_spec(cp);
    if (!s.up && !s.down && !s.left && !s.right) return 0;

    int cx = cr.x + cr.w / 2;
    int cy = cr.y + cr.h / 2;
    int tl = (cr.w >= 16) ? 2 : 1;     /* light thickness */
    int th = tl * 2;                   /* heavy thickness */
    int d_off = (cr.w >= 14) ? 2 : 1;  /* offset for double lines */

    SDL_SetRenderDrawColor(ren, fg.r, fg.g, fg.b, 255);

    /* Rounded corner handling */
    if (s.rounded) {
        if (s.down && s.right) { /* ╭ */
            SDL_Rect rd = { cx - tl/2, cy + 1, tl, (cr.y + cr.h) - (cy + 1) };
            SDL_Rect rr = { cx + 1, cy - tl/2, (cr.x + cr.w) - (cx + 1), tl };
            SDL_RenderFillRect(ren, &rd);
            SDL_RenderFillRect(ren, &rr);
            SDL_RenderDrawLine(ren, cx - tl/2, cy + 1, cx + 1, cy - tl/2);
            return 1;
        } else if (s.down && s.left) { /* ╮ */
            SDL_Rect rd = { cx - tl/2, cy + 1, tl, (cr.y + cr.h) - (cy + 1) };
            SDL_Rect rl = { cr.x, cy - tl/2, cx - cr.x, tl };
            SDL_RenderFillRect(ren, &rd);
            SDL_RenderFillRect(ren, &rl);
            SDL_RenderDrawLine(ren, cx + tl/2, cy + 1, cx - 1, cy - tl/2);
            return 1;
        } else if (s.up && s.left) { /* ╯ */
            SDL_Rect ru = { cx - tl/2, cr.y, tl, cy - cr.y };
            SDL_Rect rl = { cr.x, cy - tl/2, cx - cr.x, tl };
            SDL_RenderFillRect(ren, &ru);
            SDL_RenderFillRect(ren, &rl);
            SDL_RenderDrawLine(ren, cx + tl/2, cy - 1, cx - 1, cy + tl/2);
            return 1;
        } else if (s.up && s.right) { /* ╰ */
            SDL_Rect ru = { cx - tl/2, cr.y, tl, cy - cr.y };
            SDL_Rect rr = { cx + 1, cy - tl/2, (cr.x + cr.w) - (cx + 1), tl };
            SDL_RenderFillRect(ren, &ru);
            SDL_RenderFillRect(ren, &rr);
            SDL_RenderDrawLine(ren, cx - tl/2, cy - 1, cx + 1, cy + tl/2);
            return 1;
        }
    }

    /* Left */
    if (s.left == BOX_LIGHT) {
        SDL_Rect r = { cr.x, cy - tl/2, (cx - cr.x) + tl, tl };
        SDL_RenderFillRect(ren, &r);
    } else if (s.left == BOX_HEAVY) {
        SDL_Rect r = { cr.x, cy - th/2, (cx - cr.x) + th, th };
        SDL_RenderFillRect(ren, &r);
    } else if (s.left == BOX_DOUBLE) {
        SDL_Rect r1 = { cr.x, cy - d_off - tl/2, cx - cr.x + d_off, tl };
        SDL_Rect r2 = { cr.x, cy + d_off - tl/2, cx - cr.x + d_off, tl };
        SDL_RenderFillRect(ren, &r1);
        SDL_RenderFillRect(ren, &r2);
    }

    /* Right */
    if (s.right == BOX_LIGHT) {
        SDL_Rect r = { cx - tl/2, cy - tl/2, (cr.x + cr.w) - cx + tl/2, tl };
        SDL_RenderFillRect(ren, &r);
    } else if (s.right == BOX_HEAVY) {
        SDL_Rect r = { cx - th/2, cy - th/2, (cr.x + cr.w) - cx + th/2, th };
        SDL_RenderFillRect(ren, &r);
    } else if (s.right == BOX_DOUBLE) {
        SDL_Rect r1 = { cx - d_off, cy - d_off - tl/2, (cr.x + cr.w) - cx + d_off, tl };
        SDL_Rect r2 = { cx - d_off, cy + d_off - tl/2, (cr.x + cr.w) - cx + d_off, tl };
        SDL_RenderFillRect(ren, &r1);
        SDL_RenderFillRect(ren, &r2);
    }

    /* Up */
    if (s.up == BOX_LIGHT) {
        SDL_Rect r = { cx - tl/2, cr.y, tl, (cy - cr.y) + tl };
        SDL_RenderFillRect(ren, &r);
    } else if (s.up == BOX_HEAVY) {
        SDL_Rect r = { cx - th/2, cr.y, th, (cy - cr.y) + th };
        SDL_RenderFillRect(ren, &r);
    } else if (s.up == BOX_DOUBLE) {
        SDL_Rect r1 = { cx - d_off - tl/2, cr.y, tl, cy - cr.y + d_off };
        SDL_Rect r2 = { cx + d_off - tl/2, cr.y, tl, cy - cr.y + d_off };
        SDL_RenderFillRect(ren, &r1);
        SDL_RenderFillRect(ren, &r2);
    }

    /* Down */
    if (s.down == BOX_LIGHT) {
        SDL_Rect r = { cx - tl/2, cy - tl/2, tl, (cr.y + cr.h) - cy + tl/2 };
        SDL_RenderFillRect(ren, &r);
    } else if (s.down == BOX_HEAVY) {
        SDL_Rect r = { cx - th/2, cy - th/2, th, (cr.y + cr.h) - cy + th/2 };
        SDL_RenderFillRect(ren, &r);
    } else if (s.down == BOX_DOUBLE) {
        SDL_Rect r1 = { cx - d_off - tl/2, cy - d_off, tl, (cr.y + cr.h) - cy + d_off };
        SDL_Rect r2 = { cx + d_off - tl/2, cy - d_off, tl, (cr.y + cr.h) - cy + d_off };
        SDL_RenderFillRect(ren, &r1);
        SDL_RenderFillRect(ren, &r2);
    }

    return 1;
}

/* ── Glyph Texture Cache (Milestone 3 Optimization) ─────────────────────── */

#define GLYPH_CACHE_SIZE 1024

typedef struct {
    uint32_t     codepoint;
    uint32_t     color_rgba;
    uint8_t      bold;
    uint8_t      occupied;
    int          draw_w;
    int          draw_h;
    SDL_Texture *texture;
    uint32_t     last_used;
} glyph_cache_entry_t;

static glyph_cache_entry_t glyph_cache[GLYPH_CACHE_SIZE];
static uint32_t cache_frame_counter = 0;

static void clear_glyph_cache(void) {
    for (int i = 0; i < GLYPH_CACHE_SIZE; i++) {
        if (glyph_cache[i].occupied && glyph_cache[i].texture) {
            SDL_DestroyTexture(glyph_cache[i].texture);
        }
        glyph_cache[i].occupied = 0;
        glyph_cache[i].texture = NULL;
    }
}

static SDL_Texture* get_cached_glyph(SDL_Renderer *renderer,
                                     uint32_t cp, uint8_t bold, SDL_Color fg,
                                     int *out_w, int *out_h) {
    uint32_t color_key = ((uint32_t)fg.r << 24) | ((uint32_t)fg.g << 16) | ((uint32_t)fg.b << 8) | (uint32_t)fg.a;
    uint32_t h = (cp ^ (color_key * 31) ^ (bold ? 0x55555555 : 0)) % GLYPH_CACHE_SIZE;

    int free_slot = -1;
    int oldest_slot = -1;
    uint32_t oldest_time = 0xFFFFFFFF;

    for (int i = 0; i < 16; i++) {
        int idx = (int)((h + i) % GLYPH_CACHE_SIZE);
        if (glyph_cache[idx].occupied) {
            if (glyph_cache[idx].codepoint == cp &&
                glyph_cache[idx].color_rgba == color_key &&
                glyph_cache[idx].bold == bold) {
                glyph_cache[idx].last_used = cache_frame_counter;
                *out_w = glyph_cache[idx].draw_w;
                *out_h = glyph_cache[idx].draw_h;
                return glyph_cache[idx].texture;
            }
            if (glyph_cache[idx].last_used < oldest_time) {
                oldest_time = glyph_cache[idx].last_used;
                oldest_slot = idx;
            }
        } else if (free_slot == -1) {
            free_slot = idx;
        }
    }

    /* Cache miss: pick font from fallback chain */
    TTF_Font *f = NULL;
    if (bold && TTF_GlyphIsProvided32(font_bold, cp)) {
        f = font_bold;
    } else if (TTF_GlyphIsProvided32(font, cp)) {
        f = font;
    } else if (font_cjk && TTF_GlyphIsProvided32(font_cjk, cp)) {
        f = font_cjk;
    } else if (font_emoji && TTF_GlyphIsProvided32(font_emoji, cp)) {
        f = font_emoji;
    }

    if (!f) return NULL;

    SDL_Surface *surf = TTF_RenderGlyph32_Blended(f, cp, fg);
    if (!surf) return NULL;

    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);
    if (!tex) {
        SDL_FreeSurface(surf);
        return NULL;
    }

    int dw = surf->w;
    int dh = surf->h;
    SDL_FreeSurface(surf);

    int target_slot = (free_slot != -1) ? free_slot : oldest_slot;
    if (target_slot < 0) target_slot = (int)h;

    if (glyph_cache[target_slot].occupied && glyph_cache[target_slot].texture) {
        SDL_DestroyTexture(glyph_cache[target_slot].texture);
    }

    glyph_cache[target_slot].codepoint = cp;
    glyph_cache[target_slot].color_rgba = color_key;
    glyph_cache[target_slot].bold = bold;
    glyph_cache[target_slot].draw_w = dw;
    glyph_cache[target_slot].draw_h = dh;
    glyph_cache[target_slot].texture = tex;
    glyph_cache[target_slot].occupied = 1;
    glyph_cache[target_slot].last_used = cache_frame_counter;

    *out_w = dw;
    *out_h = dh;
    return tex;
}

/* ── Renderer ───────────────────────────────────────────────────────────── */

void render(SDL_Renderer* renderer, SDL_Texture* text_texture, TTF_Font* font, TTF_Font* font_bold, 
            uint32_t char_h, uint32_t char_w){

    (void)text_texture;
    (void)font;
    (void)font_bold;

    needs_render = 0;
    cache_frame_counter++;
    SDL_SetRenderDrawColor(renderer, default_bg.r, default_bg.g, default_bg.b, default_bg.a);
    SDL_RenderClear(renderer);

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            cell_t cell = get_visible_cell(r, c);
            SDL_Rect cell_rect = {
                (int)(8 + c * char_w),
                (int)(8 + r * char_h),
                (int)char_w,
                (int)char_h
            };

            int is_cursor = (scroll_offset == 0 && r == (int)cursor.y && c == (int)cursor.x);

            /* If cursor rests on a 2-width cell, expand cursor block */
            if (is_cursor && cell.width == 2) {
                cell_rect.w = (int)(2 * char_w);
            }

            /* Render cell background if not default (or cursor block) */
            if (is_cursor) {
                SDL_Color cur_col = cursor_color;
                uint8_t fg_idx = cell.fg;
                if (cell.bold && fg_idx < 8) fg_idx += 8;
                if (fg_idx < 16 && fg_idx != 15 && fg_idx != ATTR_DEFAULT_FG) {
                    cur_col = ansi_palette[fg_idx];
                }
                SDL_SetRenderDrawColor(renderer, cur_col.r, cur_col.g, cur_col.b, cur_col.a);
                SDL_RenderFillRect(renderer, &cell_rect);
            } else if (cell.bg != ATTR_DEFAULT_BG && cell.bg < 16) {
                SDL_Color bg_col = ansi_palette[cell.bg];
                SDL_SetRenderDrawColor(renderer, bg_col.r, bg_col.g, bg_col.b, bg_col.a);
                SDL_RenderFillRect(renderer, &cell_rect);
            }

            /* Skip trailing half of wide character */
            if (cell.width == 0) continue;
            /* Skip empty or space cell */
            if (cell.codepoint <= ' ') continue;

            uint8_t fg_idx = cell.fg;
            if (cell.bold && fg_idx < 8) fg_idx += 8;
            SDL_Color fg_col;
            if (is_cursor) {
                fg_col = default_bg; /* Invert text color so character remains visible on cursor */
            } else {
                fg_col = (fg_idx == ATTR_DEFAULT_FG || fg_idx >= 16) ? default_fg : ansi_palette[fg_idx];
            }

            /* 1. Procedural block elements (█, ▀, ▄, ▌, ▐, fractions, shades) */
            if (render_block_element(renderer, cell.codepoint, cell_rect, fg_col)) {
                continue;
            }

            /* 2. Procedural box-drawing characters (─, │, ┌, ┐, └, ┘, ├, ┤, ┬, ┴, ┼, ═, ║, etc.) */
            if (render_box_drawing(renderer, cell.codepoint, cell_rect, fg_col)) {
                continue;
            }

            /* 3. Cached font glyph rendering (Milestone 3 optimization) */
            int draw_w = 0, draw_h = 0;
            SDL_Texture *tex = get_cached_glyph(renderer, cell.codepoint, cell.bold, fg_col, &draw_w, &draw_h);
            if (tex) {
                int max_w = (cell.width == 2) ? (int)(2 * char_w) : (int)char_w;
                if (draw_w > max_w) draw_w = max_w;
                if (draw_h > (int)char_h) draw_h = (int)char_h;
                int off_x = (max_w > draw_w) ? (max_w - draw_w) / 2 : 0;
                int off_y = ((int)char_h > draw_h) ? ((int)char_h - draw_h) / 2 : 0;
                SDL_Rect dst = {
                    cell_rect.x + off_x,
                    cell_rect.y + off_y,
                    draw_w,
                    draw_h
                };
                SDL_RenderCopy(renderer, tex, NULL, &dst);
            }
        }
    }

    /* Draw subtle scroll indicator if scrolled into history */
    if (scroll_offset > 0 && scrollback_count > 0) {
        int total_lines = scrollback_count + rows;
        int track_h = rows * char_h;
        int thumb_h = (int)((float)rows / (float)total_lines * (float)track_h);
        if (thumb_h < 12) thumb_h = 12;

        int view_top = scrollback_count - scroll_offset;
        int thumb_y  = (int)(8 + ((float)view_top / (float)scrollback_count) * (float)(track_h - thumb_h));

        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 146, 131, 116, 180);
        SDL_Rect thumb_rect = {
            (int)(8 + cols * char_w + 3),
            thumb_y,
            3,
            thumb_h
        };
        SDL_RenderFillRect(renderer, &thumb_rect);
    }

    SDL_RenderPresent(renderer);
}

/* ── Main ───────────────────────────────────────────────────────────────── */

int main(void) {
    setlocale(LC_ALL, "");
    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) < 0){
        fprintf(stderr, "Failed to initialize SDL: %s", SDL_GetError());
        return 1;
    }
    if (TTF_Init() < 0) {
        fprintf(stderr, "TTF_Init failed: %s\n", TTF_GetError());
        SDL_Quit();
        return 1;
    }
       font = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", font_size);
       font_bold = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf", font_size);
    if (!font || !font_bold) {
        if (font) TTF_CloseFont(font);
        if (font_bold) TTF_CloseFont(font_bold);
        font = TTF_OpenFont("/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf", 32);
        font_bold = TTF_OpenFont("/usr/share/fonts/truetype/ubuntu/UbuntuMono-B.ttf", 32);
    }
    if (!font || !font_bold) {
        fprintf(stderr, "Could not open monospace font or bold font: %s\n", TTF_GetError());
        TTF_Quit();
        SDL_Quit();
        return 1;
    }
    TTF_SetFontStyle(font_bold, TTF_STYLE_BOLD);

    /* Fallback fonts for CJK characters and Emoji */
    const char *cjk_paths[] = {
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc",
        NULL
    };
    for (int i = 0; cjk_paths[i]; i++) {
        font_cjk = TTF_OpenFont(cjk_paths[i], font_size);
        if (font_cjk) break;
    }

    const char *emoji_paths[] = {
        "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf",
        NULL
    };
    for (int i = 0; emoji_paths[i]; i++) {
        font_emoji = TTF_OpenFont(emoji_paths[i], font_size);
        if (font_emoji) break;
    }

    /* char_w/char_h are globals — assign them here after font load */
    TTF_SizeText(font, "M", &char_w, &char_h);
    if (char_w <= 0 || char_h <= 0) {
        char_w = 9;
        char_h = 18;
    }

    win_width  = char_w * cols + 16;
    win_height = char_h * rows + 16;

    window = SDL_CreateWindow(
            "fatty",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            win_width, win_height,
            SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
            );
    if (!window) {
        fprintf(stderr, "Window creation failed: %s\n", SDL_GetError());
        return 1;
    }
    term_buffer = calloc((size_t)cols * (size_t)rows, sizeof(cell_t));
    for (size_t i = 0; i < (size_t)cols * (size_t)rows; i++) {
        term_buffer[i] = make_blank_cell();
    }
    SDL_Texture *text_texture = NULL;

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    SDL_StartTextInput();

    /* Spawn child shell via PTY */
    struct winsize ws = {
        .ws_row    = DEFAULT_ROWS,
        .ws_col    = DEFAULT_COLS,
        .ws_xpixel = (unsigned short)win_width,
        .ws_ypixel = (unsigned short)win_height
    };

    pid_t child_pid = forkpty(&master_fd, NULL, NULL, &ws);
    if (child_pid < 0) {
        perror("forkpty failed");
        return 1;
    }

    if (child_pid == 0) {
        setenv("TERM", "xterm-256color", 1);
        char *shell = getenv("SHELL");
        if (!shell) shell = "/bin/bash";
        execlp(shell, shell, NULL);
        perror("execlp");
        _exit(1);
    }

    /* Non-blocking reads on master_fd */
    int flags = fcntl(master_fd, F_GETFL, 0);
    fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);

    char pty_buffer[4096];

    while (running) {
        event_handler();
        if (!running) break;
        SDL_SetWindowFullscreen(window,fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        struct pollfd fds = { .fd = master_fd, .events = POLLIN };
        int ret = poll(&fds, 1, 10);
        if (ret < 0 && errno != EINTR) break;

        /* Check if child process died */
        int status;
        if (waitpid(child_pid, &status, WNOHANG) != 0) {
            break;
        }

        /* Read bytes emitted by the shell process */
        if (fds.revents & POLLIN) {
            if(!read_pty(pty_buffer)) break;
        }
        if(needs_render){
            render(renderer, text_texture, font, font_bold, (uint32_t)char_h, (uint32_t)char_w);
        }
    }

    /* Cleanup */
    if (child_pid > 0) {
        kill(child_pid, SIGTERM);
        waitpid(child_pid, NULL, WNOHANG);
    }
    if (master_fd >= 0) close(master_fd);
    free(term_buffer);
    clear_scrollback();
    if (text_texture) SDL_DestroyTexture(text_texture);
    clear_glyph_cache();
    TTF_CloseFont(font);
    TTF_CloseFont(font_bold);
    if (font_cjk) TTF_CloseFont(font_cjk);
    if (font_emoji) TTF_CloseFont(font_emoji);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();

    return 0;
}
