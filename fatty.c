/* ── Includes ───────────────────────────────────────────────────────────── */

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
#include <time.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

/* ── Constant & Variables ───────────────────────────────────────────────── */

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

typedef struct {
    uint32_t  codepoint;    /* Unicode codepoint (U+0000 .. U+10FFFF) */
    SDL_Color fg;           /* Resolved text foreground color */
    SDL_Color bg;           /* Resolved background color */
    uint8_t   bold          : 1;
    uint8_t   dim           : 1;
    uint8_t   underline     : 1;
    uint8_t   reverse       : 1;
    uint8_t   is_default_fg : 1;
    uint8_t   is_default_bg : 1;
    uint8_t   width;        /* 1 for standard, 2 for wide lead, 0 for wide continuation */
} cell_t;

static SDL_Color current_fg;
static SDL_Color current_bg;
static uint8_t   current_bold = 0;
static uint8_t   current_dim  = 0;
static uint8_t   current_underline = 0;
static uint8_t   current_reverse = 0;
static uint8_t   current_is_default_fg = 1;
static uint8_t   current_is_default_bg = 1;

static void reset_sgr(void) {
    current_fg = default_fg;
    current_bg = default_bg;
    current_bold = 0;
    current_dim = 0;
    current_underline = 0;
    current_reverse = 0;
    current_is_default_fg = 1;
    current_is_default_bg = 1;
}

static inline cell_t make_blank_cell(void) {
    cell_t cell;
    memset(&cell, 0, sizeof(cell));
    cell.codepoint     = 0;
    cell.fg            = default_fg;
    cell.bg            = default_bg;
    cell.is_default_fg = 1;
    cell.is_default_bg = 1;
    cell.width         = 1;
    return cell;
}

static inline cell_t make_erased_cell(void) {
    cell_t cell;
    memset(&cell, 0, sizeof(cell));
    cell.codepoint     = 0;
    cell.fg            = current_fg;
    cell.bg            = current_bg;
    cell.is_default_fg = current_is_default_fg;
    cell.is_default_bg = current_is_default_bg;
    cell.width         = 1;
    return cell;
}

static SDL_Color color_from_256(int idx) {
    if (idx >= 0 && idx < 16) {
        return ansi_palette[idx];
    }
    if (idx >= 16 && idx <= 231) {
        int cube = idx - 16;
        int r_idx = cube / 36;
        int g_idx = (cube % 36) / 6;
        int b_idx = cube % 6;
        static const uint8_t cv[6] = {0, 95, 135, 175, 215, 255};
        SDL_Color c = {cv[r_idx], cv[g_idx], cv[b_idx], 255};
        return c;
    }
    if (idx >= 232 && idx <= 255) {
        uint8_t v = (uint8_t)((idx - 232) * 10 + 8);
        SDL_Color c = {v, v, v, 255};
        return c;
    }
    return default_fg;
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
    uint8_t wrapped; /* 1 if line wrapped onto next row without \n */
} scroll_row_t;

static scroll_row_t scrollback[SCROLLBACK_MAX_LINES];
static int scrollback_head  = 0;
static int scrollback_count = 0;
static int scroll_offset    = 0;
static uint8_t *row_wrapped = NULL;
static uint8_t  wrap_next   = 0;

/* ── Screen buffer abstraction ─────────────────────────────────────────── */

typedef enum {
    BUFFER_MAIN = 0,
    BUFFER_ALT  = 1
} buffer_type_t;

typedef struct {
    cell_t   *cells;        /* Grid cells for this screen buffer */
    uint8_t  *row_wrapped;  /* Wrapped line tracking */
    int       cols;         /* Column count for this buffer */
    int       rows;         /* Row count for this buffer */
    cursor_t  cursor;       /* Cursor position */
    uint8_t   wrap_next;    /* Pending wrap flag at margin */
    cursor_t  saved_cursor; /* Saved cursor for ESC 7 / ESC 8 / CSI ?1049 */
    int       scroll_top;      /* 0-based top row of scrolling region (DECSTBM) */
    int       scroll_bottom;   /* 0-based bottom row of scrolling region */
    uint8_t   cursor_visible;  /* 1 = visible (DECTCEM), 0 = hidden */
    uint8_t   auto_wrap;       /* 1 = DECAWM wrap at right margin */
    uint8_t   app_cursor_keys; /* 1 = DECCKM application cursor keys */
    uint8_t   mouse_tracking;  /* 0 = off, 1 = 1000, 2 = 1002, 3 = 1003 */
    uint8_t   mouse_sgr;       /* 1 = 1006 SGR mouse mode */
} screen_buffer_t;

static screen_buffer_t screens[2];
static buffer_type_t   active_buffer = BUFFER_MAIN;

static inline int has_status_bar(void) {
    return active_buffer == BUFFER_MAIN;
}

static inline int get_status_bar_height(void) {
    return has_status_bar() ? (char_h + 6) : 0;
}

static void resize_terminal(int new_w, int new_h);
static void switch_buffer(buffer_type_t target);
static void clear_scrollback();
static void pty_write(const char *buf, size_t len);
uint8_t running = 1, needs_render = 1;
static int master_fd = -1;

/* ── Text Selection & Clipboard ────────────────────────────────────────── */

typedef struct {
    int start_c, start_r;   /* Origin cell where mouse was pressed */
    int end_c, end_r;       /* Target cell where mouse was dragged */
    uint8_t active;         /* 1 if selection is currently active/highlighted */
    uint8_t selecting;      /* 1 while left mouse button is pressed and dragging */
    uint32_t last_click_time;
    int click_count;        /* 1 = single/drag, 2 = double (word), 3 = triple (line) */
} selection_t;

static selection_t selection = {0, 0, 0, 0, 0, 0, 0, 0};
static uint8_t bracketed_paste_mode = 0;
static const SDL_Color selection_bg = {80, 73, 69, 255}; /* Gruvbox bg2 */

/* ── ANSI parser ────────────────────────────────────────────────────────── */

typedef enum {
    ANSI_NORMAL,    /* regular text                                    */
    ANSI_ESC,       /* received ESC (0x1B)                             */
    ANSI_ESC_DROP,  /* eat 1 character after ESC (,),*,+, etc.         */
    ANSI_CSI,       /* received ESC [  — collecting params             */
    ANSI_OSC,       /* received ESC ]  — collecting OSC payload        */
    ANSI_OSC_ESC,   /* saw ESC inside OSC — peeking for ST (ESC \)     */
    ANSI_OSC_CSI,   /* skipping a CSI sequence embedded inside OSC     */
    ANSI_DCS,       /* received ESC P  — absorbing DCS payload         */
    ANSI_DCS_ESC,   /* saw ESC inside DCS — peeking for ST (ESC \)     */
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

static inline int is_blank_cell(cell_t c) {
    return (c.codepoint == 0 || c.codepoint == ' ') && c.is_default_bg && !c.reverse;
}

static void scrollback_push_row(const cell_t *row, int num_cols, uint8_t wrapped) {
    scroll_row_t *slot = &scrollback[scrollback_head];
    if (slot->cols != num_cols) {
        free(slot->cells);
        slot->cells = malloc((size_t)num_cols * sizeof(cell_t));
        slot->cols  = num_cols;
    }
    slot->wrapped = wrapped;
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
            scrollback[i].wrapped = 0;
        }
    }
    scrollback_head  = 0;
    scrollback_count = 0;
    scroll_offset    = 0;
}

static void scroll_region_up(int top, int bottom) {
    if (top < 0) top = 0;
    if (bottom >= rows) bottom = rows - 1;
    if (top >= bottom) return;

    /* Preserve top row in scrollback ring buffer (only for primary screen buffer and full-screen region) */
    if (top == 0 && bottom == rows - 1 && has_status_bar()) {
        scrollback_push_row(term_buffer, cols, row_wrapped ? row_wrapped[0] : 0);
    }

    memmove(term_buffer + top * cols,
            term_buffer + (top + 1) * cols,
            (size_t)(bottom - top) * (size_t)cols * sizeof(cell_t));
    if (row_wrapped) {
        memmove(row_wrapped + top,
                row_wrapped + top + 1,
                (size_t)(bottom - top) * sizeof(uint8_t));
        row_wrapped[bottom] = 0;
    }
    for (int i = 0; i < cols; i++) {
        term_buffer[bottom * cols + i] = make_blank_cell();
    }
}

static void scroll_region_down(int top, int bottom) {
    if (top < 0) top = 0;
    if (bottom >= rows) bottom = rows - 1;
    if (top >= bottom) return;

    memmove(term_buffer + (top + 1) * cols,
            term_buffer + top * cols,
            (size_t)(bottom - top) * (size_t)cols * sizeof(cell_t));
    if (row_wrapped) {
        memmove(row_wrapped + top + 1,
                row_wrapped + top,
                (size_t)(bottom - top) * sizeof(uint8_t));
        row_wrapped[top] = 0;
    }
    for (int i = 0; i < cols; i++) {
        term_buffer[top * cols + i] = make_blank_cell();
    }
}

static void scroll_up(void) {
    int top = screens[active_buffer].scroll_top;
    int bottom = screens[active_buffer].scroll_bottom;
    if (bottom <= top || bottom >= rows) {
        top = 0;
        bottom = rows - 1;
    }
    scroll_region_up(top, bottom);
}

static void erase_line(int mode) {
    int start = (mode == 1) ? 0    : (int)cursor.x;
    int end   = (mode == 0) ? cols : (int)cursor.x + 1;
    if (mode == 2) { start = 0; end = cols; }
    for (int i = start; i < end && i < cols; i++)
        term_buffer[cursor.y * cols + i] = make_erased_cell();
    if (mode == 0 || mode == 2) {
        if (row_wrapped && (int)cursor.y < rows) {
            row_wrapped[cursor.y] = 0;
        }
    }
    wrap_next = 0;
}

static void erase_display(int mode) {
    if (mode == 3) {
        /* Clear scrollback buffer (xterm ESC[3J) */
        clear_scrollback();
    } else if (mode == 2) {
        for (size_t i = 0; i < (size_t)cols * (size_t)rows; i++)
            term_buffer[i] = make_erased_cell();
        if (row_wrapped) {
            memset(row_wrapped, 0, (size_t)rows * sizeof(uint8_t));
        }
    } else if (mode == 0) {
        /* cursor to end of screen */
        erase_line(0);
        for (int r = (int)cursor.y + 1; r < rows; r++) {
            for (int c = 0; c < cols; c++)
                term_buffer[r * cols + c] = make_erased_cell();
            if (row_wrapped) row_wrapped[r] = 0;
        }
    } else if (mode == 1) {
        /* beginning of screen to cursor */
        for (int r = 0; r < (int)cursor.y; r++) {
            for (int c = 0; c < cols; c++)
                term_buffer[r * cols + c] = make_erased_cell();
            if (row_wrapped) row_wrapped[r] = 0;
        }
        erase_line(1);
    }
    wrap_next = 0;
}

/* Handle a completed OSC (Operating System Command) sequence. */
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
    } else if (ps == 10 && *pt == '?') {
        /* OSC 10: query foreground color */
        pty_write("\x1b]10;rgb:eb/db/b2\x1b\\", 19);
    } else if (ps == 11 && *pt == '?') {
        /* OSC 11: query background color */
        pty_write("\x1b]11;rgb:1c/1c/1c\x1b\\", 19);
    }
}

/* Handle SGR (Select Graphic Rendition) attribute sequences with TrueColor and 256 colors. */
static void handle_sgr(const char *params) {
    if (!params || !*params) {
        reset_sgr();
        return;
    }
    const char *p = params;
    while (*p) {
        while (*p && (*p < '0' || *p > '9') && *p != ';') p++;
        if (!*p) break;
        if (*p == ';') { p++; continue; }
        int code = atoi(p);
        while (*p >= '0' && *p <= '9') p++;

        if (code == 0) {
            reset_sgr();
        } else if (code == 1) {
            current_bold = 1;
        } else if (code == 2) {
            current_dim = 1;
        } else if (code == 4) {
            current_underline = 1;
        } else if (code == 7) {
            current_reverse = 1;
        } else if (code == 22) {
            current_bold = 0;
            current_dim = 0;
        } else if (code == 24) {
            current_underline = 0;
        } else if (code == 27) {
            current_reverse = 0;
        } else if (code >= 30 && code <= 37) {
            current_fg = ansi_palette[code - 30];
            current_is_default_fg = 0;
        } else if (code == 38) {
            /* Extended foreground: 38;5;idx OR 38;2;r;g;b */
            if (*p == ';') p++;
            int type = atoi(p);
            while (*p >= '0' && *p <= '9') p++;
            if (type == 5) {
                if (*p == ';') p++;
                int col_idx = atoi(p);
                while (*p >= '0' && *p <= '9') p++;
                current_fg = color_from_256(col_idx);
                current_is_default_fg = 0;
            } else if (type == 2) {
                if (*p == ';') p++;
                int r = atoi(p);
                while (*p >= '0' && *p <= '9') p++;
                if (*p == ';') p++;
                int g = atoi(p);
                while (*p >= '0' && *p <= '9') p++;
                if (*p == ';') p++;
                int b = atoi(p);
                while (*p >= '0' && *p <= '9') p++;
                if (r < 0) { r = 0; } else if (r > 255) { r = 255; }
                if (g < 0) { g = 0; } else if (g > 255) { g = 255; }
                if (b < 0) { b = 0; } else if (b > 255) { b = 255; }
                current_fg = (SDL_Color){(uint8_t)r, (uint8_t)g, (uint8_t)b, 255};
                current_is_default_fg = 0;
            }
        } else if (code == 39) {
            current_fg = default_fg;
            current_is_default_fg = 1;
        } else if (code >= 40 && code <= 47) {
            current_bg = ansi_palette[code - 40];
            current_is_default_bg = 0;
        } else if (code == 48) {
            /* Extended background: 48;5;idx OR 48;2;r;g;b */
            if (*p == ';') p++;
            int type = atoi(p);
            while (*p >= '0' && *p <= '9') p++;
            if (type == 5) {
                if (*p == ';') p++;
                int col_idx = atoi(p);
                while (*p >= '0' && *p <= '9') p++;
                current_bg = color_from_256(col_idx);
                current_is_default_bg = 0;
            } else if (type == 2) {
                if (*p == ';') p++;
                int r = atoi(p);
                while (*p >= '0' && *p <= '9') p++;
                if (*p == ';') p++;
                int g = atoi(p);
                while (*p >= '0' && *p <= '9') p++;
                if (*p == ';') p++;
                int b = atoi(p);
                while (*p >= '0' && *p <= '9') p++;
                if (r < 0) { r = 0; } else if (r > 255) { r = 255; }
                if (g < 0) { g = 0; } else if (g > 255) { g = 255; }
                if (b < 0) { b = 0; } else if (b > 255) { b = 255; }
                current_bg = (SDL_Color){(uint8_t)r, (uint8_t)g, (uint8_t)b, 255};
                current_is_default_bg = 0;
            }
        } else if (code == 49) {
            current_bg = default_bg;
            current_is_default_bg = 1;
        } else if (code >= 90 && code <= 97) {
            current_fg = ansi_palette[8 + (code - 90)];
            current_is_default_fg = 0;
        } else if (code >= 100 && code <= 107) {
            current_bg = ansi_palette[8 + (code - 100)];
            current_is_default_bg = 0;
        }
        if (*p == ';') p++;
    }
}

static int has_param(const char *params, int code) {
    if (!params) return 0;
    const char *p = params;
    while (*p) {
        while (*p && !(*p >= '0' && *p <= '9')) p++;
        if (!*p) break;
        int val = atoi(p);
        if (val == code) return 1;
        while (*p >= '0' && *p <= '9') p++;
    }
    return 0;
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
        wrap_next = 0;
        { int n = p1 ? p1 : 1;
          cursor.y = (cursor.y >= (uint32_t)n) ? cursor.y - n : 0; }
        break;
    case 'B': /* Cursor Down */
        wrap_next = 0;
        { int n = p1 ? p1 : 1;
          if ((int)cursor.y + n < rows) cursor.y += n;
          else cursor.y = (uint32_t)(rows - 1); }
        break;
    case 'C': /* Cursor Forward */
        wrap_next = 0;
        { int n = p1 ? p1 : 1;
          cursor.x = (int)cursor.x + n < cols ? cursor.x + n : (uint32_t)(cols - 1); }
        break;
    case 'D': /* Cursor Back */
        wrap_next = 0;
        { int n = p1 ? p1 : 1;
          cursor.x = (cursor.x >= (uint32_t)n) ? cursor.x - n : 0; }
        break;
    case 'E': /* Cursor Next Line */
        wrap_next = 0;
        { int n = p1 ? p1 : 1;
          cursor.x = 0;
          cursor.y = (int)cursor.y + n < rows ? cursor.y + n : (uint32_t)(rows - 1); }
        break;
    case 'F': /* Cursor Previous Line */
        wrap_next = 0;
        { int n = p1 ? p1 : 1;
          cursor.x = 0;
          cursor.y = (cursor.y >= (uint32_t)n) ? cursor.y - n : 0; }
        break;
    case 'G': /* Cursor Horizontal Absolute */
    case '`': /* HPA */
        wrap_next = 0;
        { int col = p1 ? p1 - 1 : 0;
          cursor.x = col < cols ? (uint32_t)col : (uint32_t)(cols - 1); }
        break;
    case 'H': /* Cursor Position  ESC[row;colH  (1-based) */
    case 'f': /* same as H */
        wrap_next = 0;
        { int row = p1 ? p1 - 1 : 0;
          int col = p2 ? p2 - 1 : 0;
          if (row < 0) row = 0;
          if (col < 0) col = 0;
          cursor.y = row < rows ? (uint32_t)row : (uint32_t)(rows - 1);
          cursor.x = col < cols ? (uint32_t)col : (uint32_t)(cols - 1); }
        break;
    case 'd': /* Line Position Absolute (row, 1-based) */
        wrap_next = 0;
        { int row = p1 ? p1 - 1 : 0;
          if (row < 0) row = 0;
          cursor.y = row < rows ? (uint32_t)row : (uint32_t)(rows - 1); }
        break;
    case 'I': /* Cursor Forward Tab */
        wrap_next = 0;
        { int n = p1 ? p1 : 1;
          for (int i = 0; i < n; i++) {
              cursor.x = (cursor.x + 8) & ~7u;
              if ((int)cursor.x >= cols) { cursor.x = (uint32_t)(cols - 1); break; }
          }
        }
        break;
    case 'Z': /* Cursor Backward Tab (CBT) */
        wrap_next = 0;
        { int n = p1 ? p1 : 1;
          for (int i = 0; i < n && cursor.x > 0; i++) {
              cursor.x = (cursor.x > 0) ? ((cursor.x - 1) & ~7u) : 0;
          }
        }
        break;

    /* ── Erase & Edit ─────────────────────────────────────────── */
    case 'J': /* Erase in Display */
        erase_display(p1);
        break;
    case 'K': /* Erase in Line */
        erase_line(p1);
        break;
    case 'X': /* Erase Characters (ECH) */
        {
            int n = p1 ? p1 : 1;
            int limit = (int)cursor.x + n;
            if (limit > cols) limit = cols;
            for (int i = (int)cursor.x; i < limit; i++) {
                term_buffer[cursor.y * cols + i] = make_erased_cell();
            }
            wrap_next = 0;
        }
        break;
    case '@': /* Insert Characters (ICH) */
        {
            int n = p1 ? p1 : 1;
            if (n > cols - (int)cursor.x) n = cols - (int)cursor.x;
            if (n > 0) {
                int r = (int)cursor.y;
                int c = (int)cursor.x;
                int count = cols - c - n;
                if (count > 0) {
                    memmove(term_buffer + r * cols + c + n,
                            term_buffer + r * cols + c,
                            (size_t)count * sizeof(cell_t));
                }
                for (int i = 0; i < n; i++) {
                    term_buffer[r * cols + c + i] = make_erased_cell();
                }
                wrap_next = 0;
            }
        }
        break;
    case 'P': /* Delete Characters (DCH) */
        {
            int n = p1 ? p1 : 1;
            if (n > cols - (int)cursor.x) n = cols - (int)cursor.x;
            if (n > 0) {
                int r = (int)cursor.y;
                int c = (int)cursor.x;
                int count = cols - c - n;
                if (count > 0) {
                    memmove(term_buffer + r * cols + c,
                            term_buffer + r * cols + c + n,
                            (size_t)count * sizeof(cell_t));
                }
                for (int i = 0; i < n; i++) {
                    term_buffer[r * cols + (cols - n + i)] = make_erased_cell();
                }
                wrap_next = 0;
            }
        }
        break;
    case 'L': /* Insert Line (IL) */
        {
            int n = p1 ? p1 : 1;
            int r = (int)cursor.y;
            int top = screens[active_buffer].scroll_top;
            int bottom = screens[active_buffer].scroll_bottom;
            if (r >= top && r <= bottom) {
                for (int i = 0; i < n; i++) {
                    if (bottom > r) {
                        memmove(term_buffer + (r + 1) * cols,
                                term_buffer + r * cols,
                                (size_t)(bottom - r) * (size_t)cols * sizeof(cell_t));
                    }
                    for (int c = 0; c < cols; c++) {
                        term_buffer[r * cols + c] = make_erased_cell();
                    }
                }
                wrap_next = 0;
            }
        }
        break;
    case 'M': /* Delete Line (DL) */
        {
            int n = p1 ? p1 : 1;
            int r = (int)cursor.y;
            int top = screens[active_buffer].scroll_top;
            int bottom = screens[active_buffer].scroll_bottom;
            if (r >= top && r <= bottom) {
                for (int i = 0; i < n; i++) {
                    if (bottom > r) {
                        memmove(term_buffer + r * cols,
                                term_buffer + (r + 1) * cols,
                                (size_t)(bottom - r) * (size_t)cols * sizeof(cell_t));
                    }
                    for (int c = 0; c < cols; c++) {
                        term_buffer[bottom * cols + c] = make_erased_cell();
                    }
                }
                wrap_next = 0;
            }
        }
        break;
    case 'S': /* Scroll Up (SU) */
        {
            int n = p1 ? p1 : 1;
            int top = screens[active_buffer].scroll_top;
            int bottom = screens[active_buffer].scroll_bottom;
            for (int i = 0; i < n; i++) {
                scroll_region_up(top, bottom);
            }
        }
        break;
    case 'T': /* Scroll Down (SD) */
        {
            int n = p1 ? p1 : 1;
            int top = screens[active_buffer].scroll_top;
            int bottom = screens[active_buffer].scroll_bottom;
            for (int i = 0; i < n; i++) {
                scroll_region_down(top, bottom);
            }
        }
        break;

    /* ── Attributes / modes ───────────────────────────────────── */
    case 'm': /* SGR — colors & attributes */
        handle_sgr(params);
        break;
    case 'h': /* Set mode / private mode on  */
        if (has_param(params, 1049)) {
            screens[BUFFER_MAIN].saved_cursor = cursor;
        }
        if (has_param(params, 1049) || has_param(params, 1047) || has_param(params, 47)) {
            switch_buffer(BUFFER_ALT);
            if (has_param(params, 1049)) {
                for (size_t i = 0; i < (size_t)cols * (size_t)rows; i++) {
                    term_buffer[i] = make_blank_cell();
                }
                if (row_wrapped) memset(row_wrapped, 0, (size_t)rows);
                cursor.x = 0;
                cursor.y = 0;
                wrap_next = 0;
                screens[active_buffer].scroll_top = 0;
                screens[active_buffer].scroll_bottom = rows - 1;
            }
        }
        if (has_param(params, 2004)) bracketed_paste_mode = 1;
        if (has_param(params, 25))   screens[active_buffer].cursor_visible = 1;
        if (has_param(params, 1))    screens[active_buffer].app_cursor_keys = 1;
        if (has_param(params, 7))    screens[active_buffer].auto_wrap = 1;
        if (has_param(params, 1000)) screens[active_buffer].mouse_tracking = 1;
        if (has_param(params, 1002)) screens[active_buffer].mouse_tracking = 2;
        if (has_param(params, 1003)) screens[active_buffer].mouse_tracking = 3;
        if (has_param(params, 1006)) screens[active_buffer].mouse_sgr = 1;
        break;
    case 'l': /* Reset mode / private mode off */
        if (has_param(params, 1049) || has_param(params, 1047) || has_param(params, 47)) {
            switch_buffer(BUFFER_MAIN);
            if (has_param(params, 1049)) {
                cursor = screens[BUFFER_MAIN].saved_cursor;
                if ((int)cursor.x >= cols) cursor.x = (uint32_t)(cols - 1);
                if ((int)cursor.y >= rows) cursor.y = (uint32_t)(rows - 1);
                wrap_next = 0;
            }
        }
        if (has_param(params, 2004)) bracketed_paste_mode = 0;
        if (has_param(params, 25))   screens[active_buffer].cursor_visible = 0;
        if (has_param(params, 1))    screens[active_buffer].app_cursor_keys = 0;
        if (has_param(params, 7))    screens[active_buffer].auto_wrap = 0;
        if (has_param(params, 1000) || has_param(params, 1002) || has_param(params, 1003)) {
            screens[active_buffer].mouse_tracking = 0;
        }
        if (has_param(params, 1006)) screens[active_buffer].mouse_sgr = 0;
        break;
    case 's': /* Save cursor position */
        screens[active_buffer].saved_cursor = cursor;
        break;
    case 'u': /* Restore cursor position */
        cursor = screens[active_buffer].saved_cursor;
        if ((int)cursor.x >= cols) cursor.x = (uint32_t)(cols - 1);
        if ((int)cursor.y >= rows) cursor.y = (uint32_t)(rows - 1);
        wrap_next = 0;
        break;
    case 'r': /* DECSTBM — set scrolling region */
        {
            int top = p1 ? p1 - 1 : 0;
            int bottom = p2 ? p2 - 1 : rows - 1;
            if (top < 0) top = 0;
            if (bottom >= rows) bottom = rows - 1;
            if (top < bottom) {
                screens[active_buffer].scroll_top = top;
                screens[active_buffer].scroll_bottom = bottom;
            } else {
                screens[active_buffer].scroll_top = 0;
                screens[active_buffer].scroll_bottom = rows - 1;
            }
            cursor.x = 0;
            cursor.y = 0;
            wrap_next = 0;
        }
        break;
    case 'n': /* Device Status Report */
        if (p1 == 6) {
            char cpr[32];
            snprintf(cpr, sizeof(cpr), "\x1b[%d;%dR", (int)cursor.y + 1, (int)cursor.x + 1);
            pty_write(cpr, strlen(cpr));
        } else if (p1 == 5) {
            pty_write("\x1b[0n", 4);
        }
        break;
    case 'c': /* Device Attributes */
        if (params && strchr(params, '>')) {
            /* Secondary DA (DA2) */
            pty_write("\x1b[>0;10;0c", 10);
        } else {
            /* Primary DA (DA1) */
            pty_write("\x1b[?1;2c", 7);
        }
        break;

    /* ── Everything else: silently consume ───────────────────── */
    default:
        break;
    }
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
        int oldest = ((scrollback_head - scrollback_count) % SCROLLBACK_MAX_LINES + SCROLLBACK_MAX_LINES) % SCROLLBACK_MAX_LINES;
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

/* ── Selection & Clipboard Operations ──────────────────────────────────── */

static int utf8_encode(uint32_t cp, char *out) {
    if (cp <= 0x7F) {
        out[0] = (char)cp;
        return 1;
    } else if (cp <= 0x7FF) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp <= 0xFFFF) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else if (cp <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

static void pixel_to_cell(int px, int py, int *out_c, int *out_r) {
    if (char_w == 0 || char_h == 0) {
        *out_c = 0;
        *out_r = 0;
        return;
    }
    int max_py = win_height - get_status_bar_height();
    if (py >= max_py) py = max_py - 1;
    if (py < 8) py = 8;
    if (px < 8) px = 8;
    int max_px = 8 + cols * (int)char_w;
    if (px >= max_px) px = max_px - 1;

    int c = (px - 8) / (int)char_w;
    int r = (py - 8) / (int)char_h;
    if (c < 0) c = 0;
    if (c >= cols) c = cols - 1;
    if (r < 0) r = 0;
    if (r >= rows) r = rows - 1;

    *out_c = c;
    *out_r = r;
}

static void normalize_selection(int *out_r1, int *out_c1, int *out_r2, int *out_c2) {
    if (selection.start_r < selection.end_r ||
       (selection.start_r == selection.end_r && selection.start_c <= selection.end_c)) {
        *out_r1 = selection.start_r; *out_c1 = selection.start_c;
        *out_r2 = selection.end_r;   *out_c2 = selection.end_c;
    } else {
        *out_r1 = selection.end_r;   *out_c1 = selection.end_c;
        *out_r2 = selection.start_r; *out_c2 = selection.start_c;
    }
}

static int is_cell_selected(int r, int c) {
    if (!selection.active) return 0;
    int r1, c1, r2, c2;
    normalize_selection(&r1, &c1, &r2, &c2);
    if (r < r1 || r > r2) return 0;
    if (r == r1 && r == r2) return (c >= c1 && c <= c2);
    if (r == r1) return (c >= c1);
    if (r == r2) return (c <= c2);
    return 1;
}

static int is_word_char(uint32_t cp) {
    if (cp == 0 || cp == ' ' || cp == '\t') return 0;
    if (cp == '"' || cp == '\'' || cp == '`' || cp == '(' || cp == ')' ||
        cp == '[' || cp == ']'  || cp == '{' || cp == '}' || cp == '<' ||
        cp == '>' || cp == ';'  || cp == ',') {
        return 0;
    }
    return 1;
}

static void select_word_at(int c, int r) {
    cell_t cell = get_visible_cell(r, c);
    int is_word = is_word_char(cell.codepoint);

    int start_c = c;
    while (start_c > 0) {
        cell_t prev = get_visible_cell(r, start_c - 1);
        if (is_word_char(prev.codepoint) != is_word) break;
        start_c--;
    }

    int end_c = c;
    while (end_c < cols - 1) {
        cell_t next = get_visible_cell(r, end_c + 1);
        if (is_word_char(next.codepoint) != is_word) break;
        end_c++;
    }

    selection.start_c = start_c;
    selection.start_r = r;
    selection.end_c   = end_c;
    selection.end_r   = r;
    selection.active  = 1;
    needs_render      = 1;
}

static void select_line_at(int r) {
    selection.start_c = 0;
    selection.start_r = r;
    selection.end_c   = cols - 1;
    selection.end_r   = r;
    selection.active  = 1;
    needs_render      = 1;
}

static void clear_selection(void) {
    if (selection.active || selection.selecting) {
        selection.active = 0;
        selection.selecting = 0;
        needs_render = 1;
    }
}

static void copy_selection_to_clipboard(void) {
    if (!selection.active) return;
    int r1, c1, r2, c2;
    normalize_selection(&r1, &c1, &r2, &c2);

    size_t cap = (size_t)(r2 - r1 + 1) * (size_t)cols * 4 + 64;
    char *buf = malloc(cap);
    if (!buf) return;

    size_t len = 0;
    for (int r = r1; r <= r2; r++) {
        int sc = (r == r1) ? c1 : 0;
        int ec = (r == r2) ? c2 : cols - 1;

        int last_nb = sc - 1;
        for (int c = ec; c >= sc; c--) {
            cell_t cell = get_visible_cell(r, c);
            if (!is_blank_cell(cell)) {
                last_nb = c;
                break;
            }
        }

        for (int c = sc; c <= last_nb; c++) {
            cell_t cell = get_visible_cell(r, c);
            if (cell.width == 0) continue; /* Skip trailing half of wide char */
            uint32_t cp = cell.codepoint ? cell.codepoint : ' ';
            len += (size_t)utf8_encode(cp, buf + len);
        }

        /* If not at the last selected line, add newline */
        if (r < r2) {
            buf[len++] = '\n';
        }
    }
    buf[len] = '\0';

    if (len > 0) {
        SDL_SetClipboardText(buf);
    }
    free(buf);
}

static void paste_from_clipboard(void) {
    if (!SDL_HasClipboardText()) return;
    char *text = SDL_GetClipboardText();
    if (!text || !*text) {
        if (text) SDL_free(text);
        return;
    }

    if (bracketed_paste_mode) {
        pty_write("\x1b[200~", 6);
        pty_write(text, strlen(text));
        pty_write("\x1b[201~", 6);
    } else {
        pty_write(text, strlen(text));
    }
    SDL_free(text);
}

/* Recompute grid dimensions from the new window pixel size, anchor active content
 * when height shrinks, copy existing cells, and notify the PTY. */
static void resize_terminal(int new_w, int new_h) {
    clear_selection();
    int sb_h = get_status_bar_height();
    int new_cols = (new_w > 16 && char_w > 0) ? (new_w - 16) / char_w : 1;
    int new_rows = (new_h > 16 + sb_h && char_h > 0) ? (new_h - 16 - sb_h) / char_h : 1;
    win_width = new_w, win_height = new_h;
    if (new_cols < 1) new_cols = 1;
    if (new_rows < 1) new_rows = 1;
    if (new_cols == cols && new_rows == rows) return;

    if (!term_buffer) {
        cols = new_cols;
        rows = new_rows;
        return;
    }

    /* 1. If height is shrinking (new_rows < rows) and the cursor/prompt would fall
     * below the new bottom of the screen, scroll up by the excess rows so the
     * active prompt remains visible at the bottom.
     * The rows pushed off the top are preserved safely in scrollback. */
    if (new_rows < rows && (int)cursor.y >= new_rows) {
        int slide = (int)cursor.y - (new_rows - 1);
        for (int i = 0; i < slide; i++) {
            scroll_up();
        }
        cursor.y = (uint32_t)(new_rows - 1);
    }

    /* 2. Allocate the new terminal buffer and wrap tracking array */
    cell_t *new_buf = calloc((size_t)new_cols * (size_t)new_rows, sizeof(cell_t));
    uint8_t *new_wrapped = calloc((size_t)new_rows, sizeof(uint8_t));
    if (!new_buf || !new_wrapped) {
        if (new_buf) free(new_buf);
        if (new_wrapped) free(new_wrapped);
        return;
    }
    for (size_t i = 0; i < (size_t)new_cols * (size_t)new_rows; i++) {
        new_buf[i] = make_blank_cell();
    }

    /* 3. Copy existing cells into the new grid */
    int copy_rows = (rows < new_rows) ? rows : new_rows;
    int copy_cols = (cols < new_cols) ? cols : new_cols;
    for (int r = 0; r < copy_rows; r++) {
        memcpy(new_buf + r * new_cols, term_buffer + r * cols, (size_t)copy_cols * sizeof(cell_t));
        if (row_wrapped) {
            new_wrapped[r] = row_wrapped[r];
        }
        /* Boundary protection: if copy_cols cut a wide char in half at the margin,
         * replace the dangling first half with a blank cell */
        if (copy_cols == new_cols && new_cols > 0 && new_buf[r * new_cols + (new_cols - 1)].width == 2) {
            new_buf[r * new_cols + (new_cols - 1)] = make_blank_cell();
        }
    }

    /* 4. Free old buffer and replace with new buffer */
    free(term_buffer);
    if (row_wrapped) free(row_wrapped);
    term_buffer = new_buf;
    row_wrapped = new_wrapped;
    cols = new_cols;
    rows = new_rows;

    /* 5. Clamp cursor to valid grid bounds */
    if ((int)cursor.x >= cols) cursor.x = (uint32_t)(cols - 1);
    if ((int)cursor.y >= rows) cursor.y = (uint32_t)(rows - 1);
    wrap_next = 0;

    /* Clamp scroll offset to valid history range */
    if (scroll_offset > scrollback_count) scroll_offset = scrollback_count;
    if (scroll_offset < 0) scroll_offset = 0;

    /* Sync screen buffer state */
    screens[active_buffer].cells       = new_buf;
    screens[active_buffer].row_wrapped = new_wrapped;
    screens[active_buffer].cols        = new_cols;
    screens[active_buffer].rows        = new_rows;
    screens[active_buffer].cursor      = cursor;
    screens[active_buffer].wrap_next   = wrap_next;
    screens[active_buffer].scroll_top    = 0;
    screens[active_buffer].scroll_bottom = new_rows - 1;

    /* 6. Notify the PTY of the new dimensions (triggers SIGWINCH in child shell) */
    struct winsize ws = {
        .ws_row    = (unsigned short)rows,
        .ws_col    = (unsigned short)cols,
        .ws_xpixel = (unsigned short)new_w,
        .ws_ypixel = (unsigned short)new_h,
    };
    if (master_fd >= 0) {
        ioctl(master_fd, TIOCSWINSZ, &ws);
    }
}

/* Switch between primary screen buffer and alternate screen buffer */
static void switch_buffer(buffer_type_t target) {
    if (active_buffer == target) return;

    /* Save current active buffer state */
    screens[active_buffer].cells       = term_buffer;
    screens[active_buffer].row_wrapped = row_wrapped;
    screens[active_buffer].cols        = cols;
    screens[active_buffer].rows        = rows;
    screens[active_buffer].cursor      = cursor;
    screens[active_buffer].wrap_next   = wrap_next;

    active_buffer = target;

    /* If target buffer not yet allocated, allocate with current window capacity */
    if (!screens[active_buffer].cells) {
        int sb_h = get_status_bar_height();
        int target_cols = (win_width > 16 && char_w > 0) ? (win_width - 16) / char_w : cols;
        int target_rows = (win_height > 16 + sb_h && char_h > 0) ? (win_height - 16 - sb_h) / char_h : rows;
        if (target_cols < 1) target_cols = 1;
        if (target_rows < 1) target_rows = 1;

        screens[active_buffer].cells = calloc((size_t)target_cols * (size_t)target_rows, sizeof(cell_t));
        screens[active_buffer].row_wrapped = calloc((size_t)target_rows, sizeof(uint8_t));
        if (screens[active_buffer].cells) {
            for (size_t i = 0; i < (size_t)target_cols * (size_t)target_rows; i++) {
                screens[active_buffer].cells[i] = make_blank_cell();
            }
        }
        screens[active_buffer].cols        = target_cols;
        screens[active_buffer].rows        = target_rows;
        screens[active_buffer].cursor.x    = 0;
        screens[active_buffer].cursor.y    = 0;
        screens[active_buffer].wrap_next   = 0;
        screens[active_buffer].saved_cursor.x = 0;
        screens[active_buffer].saved_cursor.y = 0;
        screens[active_buffer].scroll_top     = 0;
        screens[active_buffer].scroll_bottom  = target_rows - 1;
        screens[active_buffer].cursor_visible = 1;
        screens[active_buffer].auto_wrap      = 1;
        screens[active_buffer].app_cursor_keys= 0;
        screens[active_buffer].mouse_tracking = 0;
        screens[active_buffer].mouse_sgr      = 0;
    }

    term_buffer = screens[active_buffer].cells;
    row_wrapped = screens[active_buffer].row_wrapped;
    cols        = screens[active_buffer].cols;
    rows        = screens[active_buffer].rows;
    cursor      = screens[active_buffer].cursor;
    wrap_next   = screens[active_buffer].wrap_next;

    /* Recompute grid dimensions because status bar visibility changed */
    if (win_width > 0 && win_height > 0) {
        resize_terminal(win_width, win_height);
        if (master_fd >= 0) {
            struct winsize ws = {
                .ws_row    = (unsigned short)rows,
                .ws_col    = (unsigned short)cols,
                .ws_xpixel = (unsigned short)win_width,
                .ws_ypixel = (unsigned short)win_height,
            };
            ioctl(master_fd, TIOCSWINSZ, &ws);
        }
    }
    needs_render = 1;
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
            } else if (ev.window.event == SDL_WINDOWEVENT_RESIZED ||
                       ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                resize_terminal(ev.window.data1, ev.window.data2);
                needs_render = 1;
            } else if (ev.window.event == SDL_WINDOWEVENT_EXPOSED) {
                needs_render = 1;
            } 
        } else if (ev.type == SDL_MOUSEWHEEL) {
            int shift = (SDL_GetModState() & KMOD_SHIFT) != 0;
            if (screens[active_buffer].mouse_tracking && !shift) {
                int c, r;
                int mx, my;
                SDL_GetMouseState(&mx, &my);
                pixel_to_cell(mx, my, &c, &r);
                int btn = (ev.wheel.y > 0) ? 64 : 65;
                char buf[32];
                snprintf(buf, sizeof(buf), "\x1b[<%d;%d;%dM", btn, c + 1, r + 1);
                pty_write(buf, strlen(buf));
            } else {
                int delta = ev.wheel.y;
                if (delta != 0 && has_status_bar()) {
                    scroll_offset += delta * 3;
                    if (scroll_offset > scrollback_count) scroll_offset = scrollback_count;
                    if (scroll_offset < 0) scroll_offset = 0;
                    needs_render = 1;
                }
            }
        } else if (ev.type == SDL_MOUSEBUTTONDOWN) {
            int shift = (SDL_GetModState() & KMOD_SHIFT) != 0;
            if (screens[active_buffer].mouse_tracking && !shift) {
                int c, r;
                pixel_to_cell(ev.button.x, ev.button.y, &c, &r);
                int btn = 0;
                if (ev.button.button == SDL_BUTTON_LEFT)   btn = 0;
                if (ev.button.button == SDL_BUTTON_MIDDLE) btn = 1;
                if (ev.button.button == SDL_BUTTON_RIGHT)  btn = 2;
                char buf[32];
                snprintf(buf, sizeof(buf), "\x1b[<%d;%d;%dM", btn, c + 1, r + 1);
                pty_write(buf, strlen(buf));
            } else {
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    int c, r;
                    pixel_to_cell(ev.button.x, ev.button.y, &c, &r);

                    uint32_t now = SDL_GetTicks();
                    if (now - selection.last_click_time < 400) {
                        selection.click_count++;
                    } else {
                        selection.click_count = 1;
                    }
                    selection.last_click_time = now;

                    if (selection.click_count == 2) {
                        select_word_at(c, r);
                        copy_selection_to_clipboard();
                    } else if (selection.click_count >= 3) {
                        select_line_at(r);
                        copy_selection_to_clipboard();
                    } else {
                        selection.start_c   = c;
                        selection.start_r   = r;
                        selection.end_c     = c;
                        selection.end_r     = r;
                        selection.selecting = 1;
                        selection.active    = 0;
                        needs_render        = 1;
                    }
                } else if (ev.button.button == SDL_BUTTON_MIDDLE) {
                    paste_from_clipboard();
                }
            }
        } else if (ev.type == SDL_MOUSEMOTION) {
            int shift = (SDL_GetModState() & KMOD_SHIFT) != 0;
            if (screens[active_buffer].mouse_tracking && !shift) {
                if (screens[active_buffer].mouse_tracking >= 2) {
                    int c, r;
                    pixel_to_cell(ev.motion.x, ev.motion.y, &c, &r);
                    int btn = 32;
                    if (ev.motion.state & SDL_BUTTON_LMASK)      btn += 0;
                    else if (ev.motion.state & SDL_BUTTON_MMASK) btn += 1;
                    else if (ev.motion.state & SDL_BUTTON_RMASK) btn += 2;
                    else btn += 3;
                    if (screens[active_buffer].mouse_tracking == 3 || (ev.motion.state & (SDL_BUTTON_LMASK | SDL_BUTTON_MMASK | SDL_BUTTON_RMASK))) {
                        char buf[32];
                        snprintf(buf, sizeof(buf), "\x1b[<%d;%d;%dM", btn, c + 1, r + 1);
                        pty_write(buf, strlen(buf));
                    }
                }
            } else {
                if (selection.selecting && (ev.motion.state & SDL_BUTTON_LMASK)) {
                    int c, r;
                    pixel_to_cell(ev.motion.x, ev.motion.y, &c, &r);
                    if (c != selection.end_c || r != selection.end_r) {
                        selection.end_c  = c;
                        selection.end_r  = r;
                        selection.active = 1;
                        needs_render     = 1;
                    }
                }
            }
        } else if (ev.type == SDL_MOUSEBUTTONUP) {
            int shift = (SDL_GetModState() & KMOD_SHIFT) != 0;
            if (screens[active_buffer].mouse_tracking && !shift) {
                int c, r;
                pixel_to_cell(ev.button.x, ev.button.y, &c, &r);
                int btn = 0;
                if (ev.button.button == SDL_BUTTON_LEFT)   btn = 0;
                if (ev.button.button == SDL_BUTTON_MIDDLE) btn = 1;
                if (ev.button.button == SDL_BUTTON_RIGHT)  btn = 2;
                char buf[32];
                snprintf(buf, sizeof(buf), "\x1b[<%d;%d;%dm", btn, c + 1, r + 1);
                pty_write(buf, strlen(buf));
            } else {
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    if (selection.selecting) {
                        selection.selecting = 0;
                        if (selection.active && (selection.start_c != selection.end_c || selection.start_r != selection.end_r)) {
                            copy_selection_to_clipboard();
                        } else if (selection.click_count == 1) {
                            clear_selection();
                        }
                    }
                }
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

            /* Ctrl+Shift+C: Copy selected text to clipboard */
            if (ctrl && shift && sym == SDLK_c) {
                copy_selection_to_clipboard();
                continue;
            }
            /* Ctrl+Shift+V: Paste text from clipboard */
            if (ctrl && shift && sym == SDLK_v) {
                paste_from_clipboard();
                continue;
            }
            /* Shift+Insert: Paste text from clipboard */
            if (shift && sym == SDLK_INSERT) {
                paste_from_clipboard();
                continue;
            }

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
            int app_cursor = screens[active_buffer].app_cursor_keys;
            switch (sym) {
            case SDLK_RETURN:
            case SDLK_KP_ENTER: seq = "\r";       break; /* CR */
            case SDLK_BACKSPACE: seq = "\x7f";    break; /* DEL (PTY erase char) */
            case SDLK_TAB:       seq = "\t";      break;
            case SDLK_ESCAPE:    seq = "\x1b";    break;
            case SDLK_UP:        seq = app_cursor ? "\x1bOA" : "\x1b[A";  break;
            case SDLK_DOWN:      seq = app_cursor ? "\x1bOB" : "\x1b[B";  break;
            case SDLK_RIGHT:     seq = app_cursor ? "\x1bOC" : "\x1b[C";  break;
            case SDLK_LEFT:      seq = app_cursor ? "\x1bOD" : "\x1b[D";  break;
            case SDLK_HOME:      seq = app_cursor ? "\x1bOH" : "\x1b[H";  break;
            case SDLK_END:       seq = app_cursor ? "\x1bOF" : "\x1b[F";  break;
            case SDLK_DELETE:    seq = "\x1b[3~"; break;
            case SDLK_PAGEUP:    seq = "\x1b[5~"; break;
            case SDLK_PAGEDOWN:  seq = "\x1b[6~"; break;
            case SDLK_F1:        seq = "\x1bOP";  break;
            case SDLK_F2:        seq = "\x1bOQ";  break;
            case SDLK_F3:        seq = "\x1bOR";  break;
            case SDLK_F4:        seq = "\x1bOS";  break;
            case SDLK_F5:        seq = "\x1b[15~"; break;
            case SDLK_F6:        seq = "\x1b[17~"; break;
            case SDLK_F7:        seq = "\x1b[18~"; break;
            case SDLK_F8:        seq = "\x1b[19~"; break;
            case SDLK_F9:        seq = "\x1b[20~"; break;
            case SDLK_F10:       seq = "\x1b[21~"; break;
            case SDLK_F11:
                if (shift) {
                    seq = "\x1b[23~";
                } else {
                    fullscreen = !fullscreen;
                }
                break;
            case SDLK_F12:       seq = "\x1b[24~"; break;
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
                } else if (ch == 'P' || ch == '_' || ch == '^') { /* DCS / APC / PM — absorb payload */
                    ansi_state = ANSI_DCS;
                } else if (ch == '(' || ch == ')' || ch == '*' || ch == '+' ||
                           ch == '-' || ch == '.' || ch == '/' || ch == '%' || ch == '#') {
                    /* Charset designator / DEC mode — absorb and drop the next byte */
                    ansi_state = ANSI_ESC_DROP;
                } else if (ch == 'c') {          /* RIS — full reset */
                    erase_display(2);
                    cursor.x = cursor.y = 0;
                    utf8_expected = 0;
                    utf8_codepoint = 0;
                    wrap_next = 0;
                    reset_sgr();
                    screens[active_buffer].scroll_top = 0;
                    screens[active_buffer].scroll_bottom = rows - 1;
                    ansi_state = ANSI_NORMAL;
                } else if (ch == 'M') {          /* RI — reverse index */
                    wrap_next = 0;
                    int top = screens[active_buffer].scroll_top;
                    int bottom = screens[active_buffer].scroll_bottom;
                    if ((int)cursor.y == top) {
                        scroll_region_down(top, bottom);
                    } else if (cursor.y > 0) {
                        cursor.y--;
                    }
                    ansi_state = ANSI_NORMAL;
                } else if (ch == '=' || ch == '>') { /* keypad mode, ignore */
                    ansi_state = ANSI_NORMAL;
                } else if (ch == '7') {          /* DECSC — save cursor */
                    screens[active_buffer].saved_cursor = cursor;
                    ansi_state = ANSI_NORMAL;
                } else if (ch == '8') {          /* DECRC — restore cursor */
                    cursor = screens[active_buffer].saved_cursor;
                    if ((int)cursor.x >= cols) cursor.x = (uint32_t)(cols - 1);
                    if ((int)cursor.y >= rows) cursor.y = (uint32_t)(rows - 1);
                    wrap_next = 0;
                    ansi_state = ANSI_NORMAL;
                } else if (ch == '\\') {         /* ST (String Terminator) */
                    ansi_state = ANSI_NORMAL;
                } else {
                    ansi_state = ANSI_NORMAL; /* unknown two-char ESC seq */
                }
                continue;

            case ANSI_ESC_DROP:
                /* Consume the single trailing byte (e.g. 'B', '0') and return to normal */
                ansi_state = ANSI_NORMAL;
                continue;

            case ANSI_DCS:
                if (ch == 0x07) {
                    ansi_state = ANSI_NORMAL;
                } else if (ch == 0x1B) {
                    ansi_state = ANSI_DCS_ESC;
                }
                continue;

            case ANSI_DCS_ESC:
                if (ch == '\\') {
                    ansi_state = ANSI_NORMAL;
                } else if (ch == 0x1B) {
                    ansi_state = ANSI_DCS_ESC;
                } else {
                    ansi_state = ANSI_DCS;
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
                wrap_next = 0;
                ansi_state = ANSI_ESC;
                continue;
            }
            if (ch == '\r') {
                utf8_expected = 0;
                utf8_codepoint = 0;
                wrap_next = 0;
                cursor.x = 0;
                continue;
            }
            if (ch == '\n') {
                utf8_expected = 0;
                utf8_codepoint = 0;
                wrap_next = 0;
                if (row_wrapped && (int)cursor.y < rows) {
                    row_wrapped[cursor.y] = 0;
                }
                int top = screens[active_buffer].scroll_top;
                int bottom = screens[active_buffer].scroll_bottom;
                if ((int)cursor.y == bottom) {
                    scroll_region_up(top, bottom);
                } else if ((int)cursor.y + 1 < rows) {
                    cursor.y++;
                }
                continue;
            }
            if (ch == '\b') {
                utf8_expected = 0;
                utf8_codepoint = 0;
                wrap_next = 0;
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
                wrap_next = 0;
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
            if (wrap_next) {
                if (!screens[active_buffer].auto_wrap) {
                    cursor.x = (uint32_t)(cols - 1);
                    wrap_next = 0;
                } else {
                    if (row_wrapped && (int)cursor.y < rows) {
                        row_wrapped[cursor.y] = 1;
                    }
                    cursor.x = 0;
                    int top = screens[active_buffer].scroll_top;
                    int bottom = screens[active_buffer].scroll_bottom;
                    if ((int)cursor.y == bottom) {
                        scroll_region_up(top, bottom);
                    } else if ((int)cursor.y + 1 < rows) {
                        cursor.y++;
                    }
                    wrap_next = 0;
                }
            }

            if (w == 2) {
                /* Double-width character: occupies 2 terminal columns */
                if ((int)cursor.x + 1 >= cols) {
                    /* Cannot fit at end of row: pad with blank and wrap */
                    if ((int)cursor.x < cols && (int)cursor.y < rows) {
                        term_buffer[cursor.y * cols + cursor.x] = make_blank_cell();
                    }
                    if (row_wrapped && (int)cursor.y < rows) {
                        row_wrapped[cursor.y] = 1;
                    }
                    cursor.x = 0;
                    int top = screens[active_buffer].scroll_top;
                    int bottom = screens[active_buffer].scroll_bottom;
                    if ((int)cursor.y == bottom) {
                        scroll_region_up(top, bottom);
                    } else if ((int)cursor.y + 1 < rows) {
                        cursor.y++;
                    }
                }
                if ((int)cursor.y < rows) {
                    cell_t c1;
                    memset(&c1, 0, sizeof(c1));
                    c1.codepoint     = cp;
                    c1.fg            = current_fg;
                    c1.bg            = current_bg;
                    c1.bold          = current_bold;
                    c1.dim           = current_dim;
                    c1.underline     = current_underline;
                    c1.reverse       = current_reverse;
                    c1.is_default_fg = current_is_default_fg;
                    c1.is_default_bg = current_is_default_bg;
                    c1.width         = 2;
                    term_buffer[cursor.y * cols + cursor.x] = c1;

                    cell_t c2;
                    memset(&c2, 0, sizeof(c2));
                    c2.codepoint     = 0;
                    c2.fg            = current_fg;
                    c2.bg            = current_bg;
                    c2.bold          = current_bold;
                    c2.dim           = current_dim;
                    c2.underline     = current_underline;
                    c2.reverse       = current_reverse;
                    c2.is_default_fg = current_is_default_fg;
                    c2.is_default_bg = current_is_default_bg;
                    c2.width         = 0; /* trailing half */
                    term_buffer[cursor.y * cols + cursor.x + 1] = c2;

                    if ((int)cursor.x + 2 >= cols) {
                        wrap_next = 1;
                        cursor.x = (uint32_t)(cols - 1);
                    } else {
                        cursor.x += 2;
                    }
                }
            } else {
                /* Standard single-column character */
                if ((int)cursor.y < rows) {
                    cell_t cell;
                    memset(&cell, 0, sizeof(cell));
                    cell.codepoint     = cp;
                    cell.fg            = current_fg;
                    cell.bg            = current_bg;
                    cell.bold          = current_bold;
                    cell.dim           = current_dim;
                    cell.underline     = current_underline;
                    cell.reverse       = current_reverse;
                    cell.is_default_fg = current_is_default_fg;
                    cell.is_default_bg = current_is_default_bg;
                    cell.width         = 1;
                    term_buffer[cursor.y * cols + cursor.x] = cell;

                    if ((int)cursor.x + 1 >= cols) {
                        wrap_next = 1;
                    } else {
                        cursor.x++;
                    }
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

/* ── Bottom Status Bar (Main Buffer Only) ──────────────────────────────── */

static void draw_status_text(SDL_Renderer *renderer, int x, int y, const char *str, SDL_Color color, uint8_t bold) {
    int cur_x = x;
    while (*str) {
        uint32_t cp = (unsigned char)*str++;
        int dw = 0, dh = 0;
        SDL_Texture *tex = get_cached_glyph(renderer, cp, bold, color, &dw, &dh);
        if (tex) {
            int off_x = ((int)char_w > dw) ? ((int)char_w - dw) / 2 : 0;
            int off_y = ((int)char_h > dh) ? ((int)char_h - dh) / 2 : 0;
            SDL_Rect dst = { cur_x + off_x, y + off_y, dw, dh };
            SDL_RenderCopy(renderer, tex, NULL, &dst);
        }
        cur_x += char_w;
    }
}

static void render_status_bar(SDL_Renderer *renderer) {
    if (!has_status_bar()) return;

    int sb_h = get_status_bar_height();
    if (sb_h <= 0 || win_height < sb_h) return;

    int sb_y = win_height - sb_h;

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

    /* 1. Status bar background (Gruvbox dark statusline) */
    SDL_Rect bg_rect = { 0, sb_y, win_width, sb_h };
    SDL_SetRenderDrawColor(renderer, 40, 40, 40, 255);
    SDL_RenderFillRect(renderer, &bg_rect);

    /* 2. Top separator border line */
    //SDL_Rect sep_rect = { 0, sb_y, win_width, 1 };
    //SDL_SetRenderDrawColor(renderer, 60, 56, 54, 255);
    // SDL_RenderFillRect(renderer, &sep_rect);

    int text_y = sb_y + 3;

    /* 3. Left badge: fatty */
    const char *badge = " fatty ";
    int badge_len = (int)strlen(badge);
    int badge_w = badge_len * char_w;
    SDL_Rect badge_rect = { 8, sb_y , badge_w, sb_h };
    SDL_SetRenderDrawColor(renderer, 60, 56, 54, 255);
    SDL_RenderFillRect(renderer, &badge_rect);
    draw_status_text(renderer, 8, text_y, badge, ansi_palette[11], 1); /* Bright Yellow, bold */

    /* 4. Left info: cols x rows */
    char dim_str[32];
    snprintf(dim_str, sizeof(dim_str), " %dx%d ", cols, rows);
    int dim_x = 8 + badge_w + 4;
    draw_status_text(renderer, dim_x, text_y, dim_str, ansi_palette[7], 0); /* White/fg4 */

    /* 5. Left info: Scroll offset indicator if scrolled up */
    int cur_left = dim_x + (int)strlen(dim_str) * char_w + 4;
    if (scroll_offset > 0) {
        char scroll_str[32];
        snprintf(scroll_str, sizeof(scroll_str), " [SCROLL +%d] ", scroll_offset);
        draw_status_text(renderer, cur_left, text_y, scroll_str, ansi_palette[9], 1); /* Bright Red, bold */
        cur_left += (int)strlen(scroll_str) * char_w + 4;
    }

    /* 6. Right side: Live clock (HH:MM:SS) */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_str[32] = {0};
    if (tm_info) {
        strftime(time_str, sizeof(time_str), " %a %b %d, %H:%M ", tm_info);
    } else {
        snprintf(time_str, sizeof(time_str), " --- --- --, --:-- ");
    }
    int time_len = (int)strlen(time_str);
    int time_w = time_len * char_w;
    int time_x = win_width - 8 - time_w;
    if (time_x > cur_left) {
        SDL_Rect clock_bg = { time_x, sb_y, time_w, sb_h };
        SDL_SetRenderDrawColor(renderer, 50, 48, 47, 255);
        SDL_RenderFillRect(renderer, &clock_bg);
        draw_status_text(renderer, time_x, text_y, time_str, default_fg, 0);
    }
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

            int is_cursor = (scroll_offset == 0 && r == (int)cursor.y && c == (int)cursor.x && screens[active_buffer].cursor_visible);
            int is_selected = is_cell_selected(r, c);

            /* If cursor rests on a 2-width cell, expand cursor block */
            if (is_cursor && cell.width == 2) {
                cell_rect.w = (int)(2 * char_w);
            }

            SDL_Color fg_col = cell.fg;
            SDL_Color bg_col = cell.bg;
            if (cell.reverse) {
                SDL_Color tmp = fg_col;
                fg_col = bg_col;
                bg_col = tmp;
            }
            if (cell.bold && !cell.reverse && cell.is_default_fg) {
                fg_col = ansi_palette[15];
            } else if (cell.dim) {
                fg_col.r = (uint8_t)(fg_col.r * 2 / 3);
                fg_col.g = (uint8_t)(fg_col.g * 2 / 3);
                fg_col.b = (uint8_t)(fg_col.b * 2 / 3);
            }

            /* Render cell background if not default (or cursor block / selection) */
            if (is_cursor) {
                SDL_SetRenderDrawColor(renderer, cursor_color.r, cursor_color.g, cursor_color.b, cursor_color.a);
                SDL_RenderFillRect(renderer, &cell_rect);
                fg_col = default_bg; /* Invert text color so character remains visible on cursor */
            } else if (is_selected) {
                SDL_SetRenderDrawColor(renderer, selection_bg.r, selection_bg.g, selection_bg.b, selection_bg.a);
                SDL_RenderFillRect(renderer, &cell_rect);
                fg_col = ansi_palette[15]; /* Crisp Bright White on selection highlight */
            } else if (!cell.is_default_bg || cell.reverse) {
                SDL_SetRenderDrawColor(renderer, bg_col.r, bg_col.g, bg_col.b, bg_col.a);
                SDL_RenderFillRect(renderer, &cell_rect);
            }

            /* Draw underline */
            if (cell.underline) {
                SDL_SetRenderDrawColor(renderer, fg_col.r, fg_col.g, fg_col.b, fg_col.a);
                SDL_RenderDrawLine(renderer, cell_rect.x, cell_rect.y + cell_rect.h - 1,
                                   cell_rect.x + cell_rect.w, cell_rect.y + cell_rect.h - 1);
            }

            /* Skip trailing half of wide character */
            if (cell.width == 0) continue;
            /* Skip empty or space cell */
            if (cell.codepoint <= ' ') continue;

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

    render_status_bar(renderer);

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
    win_height = char_h * rows + 16 + get_status_bar_height();

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
    SDL_SetWindowMinimumSize(window,900,600);
    term_buffer = calloc((size_t)cols * (size_t)rows, sizeof(cell_t));
    row_wrapped = calloc((size_t)rows, sizeof(uint8_t));
    for (size_t i = 0; i < (size_t)cols * (size_t)rows; i++) {
        term_buffer[i] = make_blank_cell();
    }
    screens[BUFFER_MAIN].cells       = term_buffer;
    screens[BUFFER_MAIN].row_wrapped = row_wrapped;
    screens[BUFFER_MAIN].cols        = cols;
    screens[BUFFER_MAIN].rows        = rows;
    screens[BUFFER_MAIN].cursor.x    = 0;
    screens[BUFFER_MAIN].cursor.y    = 0;
    screens[BUFFER_MAIN].wrap_next   = 0;
    screens[BUFFER_MAIN].saved_cursor.x = 0;
    screens[BUFFER_MAIN].saved_cursor.y = 0;
    screens[BUFFER_MAIN].scroll_top     = 0;
    screens[BUFFER_MAIN].scroll_bottom  = rows - 1;
    screens[BUFFER_MAIN].cursor_visible = 1;
    screens[BUFFER_MAIN].auto_wrap      = 1;
    screens[BUFFER_MAIN].app_cursor_keys= 0;
    screens[BUFFER_MAIN].mouse_tracking = 0;
    screens[BUFFER_MAIN].mouse_sgr      = 0;
    reset_sgr();
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

    time_t last_clock_tick = 0;

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

        /* 1-second live clock tick update for status bar */
        if (has_status_bar()) {
            time_t now = time(NULL);
            if (now != last_clock_tick) {
                last_clock_tick = now;
                needs_render = 1;
            }
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
    if (screens[BUFFER_MAIN].cells && screens[BUFFER_MAIN].cells != term_buffer) {
        free(screens[BUFFER_MAIN].cells);
        if (screens[BUFFER_MAIN].row_wrapped) free(screens[BUFFER_MAIN].row_wrapped);
    }
    if (screens[BUFFER_ALT].cells && screens[BUFFER_ALT].cells != term_buffer) {
        free(screens[BUFFER_ALT].cells);
        if (screens[BUFFER_ALT].row_wrapped) free(screens[BUFFER_ALT].row_wrapped);
    }
    free(term_buffer);
    if (row_wrapped) free(row_wrapped);
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
