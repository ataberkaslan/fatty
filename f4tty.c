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

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#define DEFAULT_COLS 80
#define DEFAULT_ROWS 24

/* Runtime terminal dimensions — updated on resize */
static int cols = DEFAULT_COLS;
static int rows = DEFAULT_ROWS;

uint8_t fullscreen = 0;
/* Glyph cell size in pixels — set once after font load */
static int char_w = 0, char_h = 0;

typedef struct {
    char c;
} cell_t;

typedef struct {
    uint32_t x,y;
} cursor_t;

typedef struct {
    cell_t cells[DEFAULT_COLS];
} row_t;

static cell_t* term_buffer;
cursor_t cursor = {0,0};

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
        term_buffer[cursor.y * cols + i].c = 0;
}

static void erase_display(int mode) {
    if (mode == 2) {
        memset(term_buffer, 0, (size_t)cols * (size_t)rows * sizeof(cell_t));
    } else if (mode == 0) {
        /* cursor to end of screen */
        erase_line(0);
        for (int r = (int)cursor.y + 1; r < rows; r++)
            memset(&term_buffer[r * cols], 0, (size_t)cols * sizeof(cell_t));
    } else if (mode == 1) {
        /* beginning of screen to cursor */
        for (int r = 0; r < (int)cursor.y; r++)
            memset(&term_buffer[r * cols], 0, (size_t)cols * sizeof(cell_t));
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
            snprintf(title, sizeof(title), "f4tty - %s", clean);
        } else {
            snprintf(title, sizeof(title), "f4tty");
        }
        SDL_SetWindowTitle(window, title);
    }
    /* OSC 1 (icon name) and all others are silently ignored */
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

    /* ── Attributes / modes (ignored for now) ─────────────────── */
    case 'm': /* SGR — colours & attributes, not yet rendered */
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
    memmove(term_buffer,
            term_buffer + cols,
            (size_t)(rows - 1) * (size_t)cols * sizeof(cell_t));
    memset(term_buffer + (rows - 1) * cols, 0,
           (size_t)cols * sizeof(cell_t));
}

/* Recompute grid dimensions from the new window pixel size, realloc the
 * term_buffer, clamp the cursor, and notify the PTY of the new winsize. */
static void resize_terminal(int new_w, int new_h) {
    int new_cols = (new_w - 16) / char_w;
    int new_rows = (new_h - 16) / char_h;
    if (new_cols < 1) new_cols = 1;
    if (new_rows < 1) new_rows = 1;
    if (new_cols == cols && new_rows == rows) return;

    cell_t *new_buf = calloc((size_t)new_cols * (size_t)new_rows, sizeof(cell_t));
    if (!new_buf) return; /* OOM — keep old buffer */

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
        } else if (ev.type == SDL_TEXTINPUT) {
            /* Forward the UTF-8 string produced by this keypress to the PTY. */
            pty_write(ev.text.text, strlen(ev.text.text));
        } else if (ev.type == SDL_KEYDOWN) {
            SDL_Keycode sym = ev.key.keysym.sym;
            SDL_Keymod  mod = SDL_GetModState();
            int ctrl = (mod & KMOD_CTRL) != 0;

            /* Ctrl+letter → send the corresponding control byte (0x01–0x1A).
             * SDL_TEXTINPUT won't fire for these, so we handle them here. */
            if (ctrl && sym >= SDLK_a && sym <= SDLK_z) {
                char cb = (char)(sym - SDLK_a + 1);
                pty_write(&cb, 1);
                break;
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
            case SDLK_F11:
                                fullscreen = !fullscreen; 
                                 break;
            default: break;
            }
            if (seq) pty_write(seq, strlen(seq));
        }
    }
}

/* ── PTY reader ─────────────────────────────────────────────────────────── */

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

            /* ── Handle C0 control characters ── */
            if (ch == 0x1B) {
                ansi_state = ANSI_ESC;
                continue;
            }
            if (ch == '\r') {
                cursor.x = 0;
                continue;
            }
            if (ch == '\n') {
                /* LF: move cursor down; scroll the screen if at the last row */
                if ((int)cursor.y + 1 < rows) {
                    cursor.y++;
                } else {
                    scroll_up(); /* cursor.y stays at rows-1 */
                }
                continue;
            }
            if (ch == '\b') {
                if (cursor.x > 0) cursor.x--;
                continue;
            }
            if (ch == '\t') {
                /* advance to next 8-column tab stop */
                cursor.x = (cursor.x + 8) & ~7u;
                if ((int)cursor.x >= cols) cursor.x = (uint32_t)(cols - 1);
                continue;
            }
            if (ch < 0x20 || ch == 0x7F) {
                /* other non-printable control bytes — ignore */
                continue;
            }

            /* ── Write printable character into cell buffer ── */
            if ((int)cursor.x >= cols) {
                cursor.x = 0;
                if ((int)cursor.y + 1 < rows) {
                    cursor.y++;
                } else {
                    scroll_up();
                }
            }
            if ((int)cursor.y < rows) {
                term_buffer[cursor.y * cols + cursor.x].c = (char)ch;
                cursor.x++;
            }
        }
        needs_render = 1;
        return 1;
    }
    return 0;
}

/* ── Renderer ───────────────────────────────────────────────────────────── */

void render(SDL_Renderer* renderer, SDL_Texture* text_texture, TTF_Font* font,
            uint32_t char_h, uint32_t char_w){

    (void)text_texture; /* reserved for future glyph-atlas optimisation */

    needs_render = 0;
    SDL_Color fg_color = {212, 212, 212, 255}; /* #D4D4D4 */
    SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
    SDL_RenderClear(renderer);

    char glyph[2] = {0, 0};

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            char ch = term_buffer[r * cols + c].c;
            if (ch <= ' ' || ch > 126) continue;
            glyph[0] = ch;
            SDL_Surface *surf = TTF_RenderUTF8_Blended(font, glyph, fg_color);
            if (surf) {
                SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);
                if (tex) {
                    SDL_Rect dst = {
                        (int)(8 + c * char_w),
                        (int)(8 + r * char_h),
                        surf->w,
                        surf->h
                    };
                    SDL_RenderCopy(renderer, tex, NULL, &dst);
                    SDL_DestroyTexture(tex);
                }
                SDL_FreeSurface(surf);
            }
        }
    }

    /* Draw cursor block */
    if ((int)cursor.y < rows && (int)cursor.x < cols) {
        SDL_SetRenderDrawColor(renderer, 212, 212, 212, 180);
        SDL_Rect cursor_rect = {
            (int)(8 + cursor.x * char_w),
            (int)(8 + cursor.y * char_h),
            (int)char_w,
            (int)char_h
        };
        SDL_RenderFillRect(renderer, &cursor_rect);
    }

    SDL_RenderPresent(renderer);
}

/* ── Main ───────────────────────────────────────────────────────────────── */

int main(void) {
    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) < 0){
        fprintf(stderr, "Failed to initialize SDL: %s", SDL_GetError());
        return 1;
    }
    if (TTF_Init() < 0) {
        fprintf(stderr, "TTF_Init failed: %s\n", TTF_GetError());
        SDL_Quit();
        return 1;
    }
    TTF_Font *font = TTF_OpenFont("/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf", 25);
    if (!font) {
        font = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 16);
    }
    if (!font) {
        fprintf(stderr, "Could not open monospace font: %s\n", TTF_GetError());
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    /* char_w/char_h are globals — assign them here after font load */
    TTF_SizeText(font, "M", &char_w, &char_h);
    if (char_w <= 0 || char_h <= 0) {
        char_w = 9;
        char_h = 18;
    }

    int win_width  = char_w * cols + 16;
    int win_height = char_h * rows + 16;

    window = SDL_CreateWindow(
            "f4tty",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            win_width, win_height,
            SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
            );
    if (!window) {
        fprintf(stderr, "Window creation failed: %s\n", SDL_GetError());
        return 1;
    }
    term_buffer = calloc((size_t)cols * (size_t)rows, sizeof(cell_t));
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
            render(renderer, text_texture, font, (uint32_t)char_h, (uint32_t)char_w);
        }
    }

    /* Cleanup */
    if (child_pid > 0) {
        kill(child_pid, SIGTERM);
        waitpid(child_pid, NULL, WNOHANG);
    }
    if (master_fd >= 0) close(master_fd);
    free(term_buffer);
    if (text_texture) SDL_DestroyTexture(text_texture);
    TTF_CloseFont(font);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();

    return 0;
}
