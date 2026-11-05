// A terminal window in one file: libghostty-vt does the emulation,
// SDL2 opens the window and draws the glyphs, forkpty runs the shell.
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif
#include <SDL.h>
#include <SDL_ttf.h>
#include <ghostty/vt.h>

#define COLS 100
#define ROWS 30
#define FONT "/System/Library/Fonts/Menlo.ttc"
#define FONT_SIZE 16

static int pty_fd = -1;
static int cw, ch;  // cell size in pixels
static SDL_Renderer *ren;
static TTF_Font *font;
static SDL_Texture *glyphs[0x10000];  // white glyphs, tinted when drawn

// libghostty answers queries like "where is the cursor?" through this.
static void on_write_pty(GhosttyTerminal t, void *ud, const uint8_t *d, size_t n) {
  (void)t; (void)ud;
  (void)!write(pty_fd, d, n);
}

static void draw_glyph(uint32_t cp, int x, int y, GhosttyColorRgb fg) {
  if (cp >= 0x10000 || cp == ' ') return;
  if (!glyphs[cp]) {
    SDL_Surface *s = TTF_RenderGlyph32_Blended(font, cp, (SDL_Color){255, 255, 255, 255});
    if (!s) return;
    glyphs[cp] = SDL_CreateTextureFromSurface(ren, s);
    SDL_FreeSurface(s);
  }
  int w, h;
  SDL_QueryTexture(glyphs[cp], NULL, NULL, &w, &h);
  SDL_SetTextureColorMod(glyphs[cp], fg.r, fg.g, fg.b);
  SDL_RenderCopy(ren, glyphs[cp], NULL, &(SDL_Rect){x, y, w, h});
}

static void fill(int x, int y, int w, int h, GhosttyColorRgb c) {
  SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, 255);
  SDL_RenderFillRect(ren, &(SDL_Rect){x, y, w, h});
}

// Keys that don't produce text. A real terminal would use the key encoder.
static void send_key(SDL_Keysym k) {
  const char *seq = NULL;
  switch (k.sym) {
    case SDLK_RETURN: seq = "\r"; break;
    case SDLK_BACKSPACE: seq = "\x7f"; break;
    case SDLK_TAB: seq = "\t"; break;
    case SDLK_ESCAPE: seq = "\x1b"; break;
    case SDLK_UP: seq = "\x1b[A"; break;
    case SDLK_DOWN: seq = "\x1b[B"; break;
    case SDLK_RIGHT: seq = "\x1b[C"; break;
    case SDLK_LEFT: seq = "\x1b[D"; break;
  }
  if (seq) { (void)!write(pty_fd, seq, strlen(seq)); return; }
  if ((k.mod & KMOD_CTRL) && k.sym >= SDLK_a && k.sym <= SDLK_z) {
    char c = (char)(k.sym & 0x1f);  // Ctrl+C = 0x03
    (void)!write(pty_fd, &c, 1);
  }
}

int main(void) {
  // 1. A shell in a pseudo-terminal.
  struct winsize ws = {.ws_row = ROWS, .ws_col = COLS};
  pid_t pid = forkpty(&pty_fd, NULL, NULL, &ws);
  if (pid < 0) return 1;
  if (pid == 0) {
    setenv("TERM", "xterm-256color", 1);
    const char *sh = getenv("SHELL") ? getenv("SHELL") : "/bin/sh";
    execl(sh, sh, "-l", (char *)NULL);  // a login shell, like real terminals
    _exit(127);
  }
  fcntl(pty_fd, F_SETFL, O_NONBLOCK);

  // 2. The terminal, a render state, and iterators over rows and cells.
  GhosttyTerminal term;
  GhosttyRenderState rs;
  GhosttyRenderStateRowIterator rows;
  GhosttyRenderStateRowCells cells;
  ghostty_terminal_new(NULL, &term, COLS, ROWS);
  ghostty_terminal_set(term, GHOSTTY_TERMINAL_OPT_WRITE_PTY, (const void *)on_write_pty);
  ghostty_render_state_new(NULL, &rs);
  ghostty_render_state_row_iterator_new(NULL, &rows);
  ghostty_render_state_row_cells_new(NULL, &cells);

  // 3. A window, sized in cells. Fonts at full resolution on Retina.
  SDL_Init(SDL_INIT_VIDEO);
  TTF_Init();
  SDL_Window *win = SDL_CreateWindow("libghostty", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                     800, 600, SDL_WINDOW_ALLOW_HIGHDPI);
  ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  int pw, ww;
  SDL_GetRendererOutputSize(ren, &pw, NULL);
  SDL_GetWindowSize(win, &ww, NULL);
  int scale = pw / ww;
  font = TTF_OpenFont(FONT, FONT_SIZE * scale);
  if (!font) return 1;
  TTF_GlyphMetrics32(font, 'M', NULL, NULL, NULL, NULL, &cw);
  ch = TTF_FontLineSkip(font);
  SDL_SetWindowSize(win, COLS * cw / scale, ROWS * ch / scale);
  SDL_StartTextInput();

  uint8_t buf[65536];
  for (bool running = true; running;) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) running = false;
      // 4. Keys out: typed text straight to the shell, the rest by hand.
      if (e.type == SDL_TEXTINPUT) (void)!write(pty_fd, e.text.text, strlen(e.text.text));
      if (e.type == SDL_KEYDOWN) send_key(e.key.keysym);  // \r, \x7f, \x1b[A, Ctrl & 0x1f
    }

    // 5. Bytes in. This one line replaces the whole escape-code parser.
    ssize_t n;
    while ((n = read(pty_fd, buf, sizeof buf)) > 0)
      ghostty_terminal_vt_write(term, buf, (size_t)n);
    if (n == 0 || (n < 0 && errno != EAGAIN)) running = false;  // shell exited

    // 6. Draw: take a snapshot, skip the frame if nothing changed.
    ghostty_render_state_update(rs, term);
    GhosttyRenderStateDirty dirty;
    ghostty_render_state_get(rs, GHOSTTY_RENDER_STATE_DATA_DIRTY, &dirty);
    if (dirty == GHOSTTY_RENDER_STATE_DIRTY_FALSE) { SDL_Delay(8); continue; }

    GhosttyRenderStateColors colors = GHOSTTY_INIT_SIZED(GhosttyRenderStateColors);
    ghostty_render_state_get(rs, GHOSTTY_RENDER_STATE_DATA_COLORS, &colors);
    fill(0, 0, COLS * cw, ROWS * ch, colors.background);

    ghostty_render_state_get(rs, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &rows);
    for (int y = 0; ghostty_render_state_row_iterator_next(rows); y++) {
      ghostty_render_state_row_get(rows, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &cells);
      for (int x = 0; ghostty_render_state_row_cells_next(cells); x++) {
        // libghostty resolves palette colors; no value means "default".
        GhosttyColorRgb fg = colors.foreground, bg = colors.background;
        ghostty_render_state_row_cells_get(cells,
            GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR, &fg);
        bool has_bg = ghostty_render_state_row_cells_get(cells,
            GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR, &bg) == GHOSTTY_SUCCESS;
        GhosttyStyle style = GHOSTTY_INIT_SIZED(GhosttyStyle);
        ghostty_render_state_row_cells_get(cells,
            GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &style);
        if (style.inverse) { GhosttyColorRgb t = fg; fg = bg; bg = t; has_bg = true; }
        if (has_bg) fill(x * cw, y * ch, cw, ch, bg);

        uint32_t len = 0, cps[16];
        ghostty_render_state_row_cells_get(cells,
            GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN, &len);
        if (len == 0 || len > 16) continue;
        ghostty_render_state_row_cells_get(cells,
            GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF, cps);
        draw_glyph(cps[0], x * cw, y * ch, fg);
      }
    }

    GhosttyRenderStateCursor cur = GHOSTTY_INIT_SIZED(GhosttyRenderStateCursor);
    ghostty_render_state_get(rs, GHOSTTY_RENDER_STATE_DATA_CURSOR, &cur);
    if (cur.visible && cur.viewport_has_value)
      fill(cur.viewport_x * cw, cur.viewport_y * ch + ch - 3 * scale, cw, 3 * scale, colors.foreground);

    ghostty_render_state_clean(rs);
    SDL_RenderPresent(ren);
  }

  kill(pid, SIGHUP);
  ghostty_terminal_free(term);
  SDL_Quit();
  return 0;
}
