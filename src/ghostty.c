/*
 * ghostty.c - Janet bindings for libghostty-vt plus a few POSIX PTY helpers.
 *
 * This module is the thin native layer for grassy-terrain. It exposes:
 *
 *   - PTY spawn/read/write/resize helpers (forkpty + exec of the user shell)
 *   - libghostty-vt terminal lifecycle, options, and effect callbacks
 *   - libghostty-vt key/mouse/focus encoders
 *   - libghostty-vt render-state extraction into plain Janet data
 *
 * The terminal event loop, windowing, and drawing all live in Janet
 * (main.janet) using Jaylib. Everything here is deliberately low-level.
 *
 * Why a native module instead of pure FFI? libghostty-vt takes C function
 * pointers for its "effect" callbacks (write-to-pty, size reports, title
 * changes, ...). Janet's FFI cannot manufacture C function pointers, so the
 * callbacks must be compiled. This module stores the Janet callbacks inside
 * the terminal abstract and dispatches to them from the C trampolines.
 */

#include <janet.h>
#include <ghostty/vt.h>

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

/* ------------------------------------------------------------------ */
/* Abstract types                                                     */
/* ------------------------------------------------------------------ */

/* One live terminal plus everything the effect trampolines need. */
typedef struct {
    GhosttyTerminal term;
    Janet callbacks; /* dictionary of effect callbacks, or nil */
    uint16_t cols;
    uint16_t rows;
    uint32_t cell_w;
    uint32_t cell_h;
} TermCtx;

typedef struct {
    GhosttyKeyEncoder encoder;
    GhosttyKeyEvent event;
} KeyCtx;

typedef struct {
    GhosttyMouseEncoder encoder;
    GhosttyMouseEvent event;
    uint32_t screen_w;
    uint32_t screen_h;
    uint32_t cell_w;
    uint32_t cell_h;
    uint32_t pad;
} MouseCtx;

typedef struct {
    GhosttyRenderState state;
    GhosttyRenderStateRowIterator rows;
    GhosttyRenderStateRowCells cells;
} RenderCtx;

static int term_gc(void *data, size_t len);
static int term_gcmark(void *data, size_t len);
static int key_gc(void *data, size_t len);
static int mouse_gc(void *data, size_t len);
static int render_gc(void *data, size_t len);

static const JanetAbstractType AT_Terminal = {
    "grassy/terminal", term_gc, term_gcmark, JANET_ATEND_GCMARK
};

static const JanetAbstractType AT_Key = {
    "grassy/key-encoder", key_gc, JANET_ATEND_GC
};

static const JanetAbstractType AT_Mouse = {
    "grassy/mouse-encoder", mouse_gc, JANET_ATEND_GC
};

static const JanetAbstractType AT_Render = {
    "grassy/render-state", render_gc, JANET_ATEND_GC
};

static int term_gc(void *data, size_t len) {
    (void) len;
    TermCtx *ctx = data;
    if (ctx->term) ghostty_terminal_free(ctx->term);
    ctx->term = NULL;
    return 0;
}

static int term_gcmark(void *data, size_t len) {
    (void) len;
    TermCtx *ctx = data;
    if (!janet_checktype(ctx->callbacks, JANET_NIL)) janet_mark(ctx->callbacks);
    return 0;
}

static int key_gc(void *data, size_t len) {
    (void) len;
    KeyCtx *ctx = data;
    if (ctx->event) ghostty_key_event_free(ctx->event);
    if (ctx->encoder) ghostty_key_encoder_free(ctx->encoder);
    ctx->event = NULL;
    ctx->encoder = NULL;
    return 0;
}

static int mouse_gc(void *data, size_t len) {
    (void) len;
    MouseCtx *ctx = data;
    if (ctx->event) ghostty_mouse_event_free(ctx->event);
    if (ctx->encoder) ghostty_mouse_encoder_free(ctx->encoder);
    ctx->event = NULL;
    ctx->encoder = NULL;
    return 0;
}

static int render_gc(void *data, size_t len) {
    (void) len;
    RenderCtx *ctx = data;
    if (ctx->cells) ghostty_render_state_row_cells_free(ctx->cells);
    if (ctx->rows) ghostty_render_state_row_iterator_free(ctx->rows);
    if (ctx->state) ghostty_render_state_free(ctx->state);
    ctx->cells = NULL;
    ctx->rows = NULL;
    ctx->state = NULL;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Call a Janet callback protected by janet_pcall. Errors are swallowed so a
 * misbehaving callback can never unwind through libghostty's C frames. */
static void call_callback(JanetFunction *fn, int32_t argc, const Janet *argv) {
    Janet out;
    JanetFiber *fiber = NULL;
    JanetSignal sig = janet_pcall(fn, argc, argv, &out, &fiber);
    if (sig != JANET_SIGNAL_OK)
        fprintf(stderr, "grassy: terminal callback failed (signal %d)\n", (int) sig);
}

static Janet callback_fn(TermCtx *ctx, const char *name) {
    if (!janet_checktypes(ctx->callbacks, (1 << JANET_TABLE) | (1 << JANET_STRUCT)))
        return janet_wrap_nil();
    return janet_get(ctx->callbacks, janet_ckeywordv(name));
}

/* ------------------------------------------------------------------ */
/* Effect trampolines installed on the terminal                       */
/* ------------------------------------------------------------------ */

static void cb_write_pty(GhosttyTerminal terminal, void *userdata,
                         const uint8_t *data, size_t len) {
    (void) terminal;
    TermCtx *ctx = userdata;
    Janet fn = callback_fn(ctx, "write-pty");
    if (!janet_checktype(fn, JANET_FUNCTION)) return;
    Janet arg = janet_stringv(data, len);
    call_callback(janet_unwrap_function(fn), 1, &arg);
}

static bool cb_size(GhosttyTerminal terminal, void *userdata,
                    GhosttySizeReportSize *out) {
    (void) terminal;
    TermCtx *ctx = userdata;
    out->rows = ctx->rows;
    out->columns = ctx->cols;
    out->cell_width = ctx->cell_w;
    out->cell_height = ctx->cell_h;
    return true;
}

static bool cb_device_attributes(GhosttyTerminal terminal, void *userdata,
                                 GhosttyDeviceAttributes *out) {
    (void) terminal;
    (void) userdata;
    out->primary.conformance_level = GHOSTTY_DA_CONFORMANCE_VT220;
    out->primary.features[0] = GHOSTTY_DA_FEATURE_COLUMNS_132;
    out->primary.features[1] = GHOSTTY_DA_FEATURE_SELECTIVE_ERASE;
    out->primary.features[2] = GHOSTTY_DA_FEATURE_ANSI_COLOR;
    out->primary.num_features = 3;
    out->secondary.device_type = GHOSTTY_DA_DEVICE_TYPE_VT220;
    out->secondary.firmware_version = 1;
    out->secondary.rom_cartridge = 0;
    out->tertiary.unit_id = 0;
    return true;
}

static GhosttyString cb_xtversion(GhosttyTerminal terminal, void *userdata) {
    (void) terminal;
    (void) userdata;
    static const char name[] = "grassy-terrain";
    return (GhosttyString){ .ptr = (const uint8_t *) name, .len = sizeof(name) - 1 };
}

static bool cb_color_scheme(GhosttyTerminal terminal, void *userdata,
                            GhosttyColorScheme *out) {
    (void) terminal;
    (void) userdata;
    (void) out;
    return false;
}

static void cb_title_changed(GhosttyTerminal terminal, void *userdata) {
    TermCtx *ctx = userdata;
    Janet fn = callback_fn(ctx, "title-changed");
    if (!janet_checktype(fn, JANET_FUNCTION)) return;
    GhosttyString title = {0};
    if (ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_TITLE, &title) != GHOSTTY_SUCCESS)
        return;
    Janet arg = janet_stringv(title.ptr, title.len);
    call_callback(janet_unwrap_function(fn), 1, &arg);
}

/* ------------------------------------------------------------------ */
/* Build info                                                         */
/* ------------------------------------------------------------------ */

static Janet cfun_build_info(int32_t argc, Janet *argv) {
    (void) argv;
    janet_fixarity(argc, 0);

    bool simd = false;
    ghostty_build_info(GHOSTTY_BUILD_INFO_SIMD, &simd);

    GhosttyOptimizeMode opt = GHOSTTY_OPTIMIZE_DEBUG;
    ghostty_build_info(GHOSTTY_BUILD_INFO_OPTIMIZE, &opt);

    const char *opt_str;
    switch (opt) {
        case GHOSTTY_OPTIMIZE_DEBUG: opt_str = "debug"; break;
        case GHOSTTY_OPTIMIZE_RELEASE_SAFE: opt_str = "release-safe"; break;
        case GHOSTTY_OPTIMIZE_RELEASE_SMALL: opt_str = "release-small"; break;
        case GHOSTTY_OPTIMIZE_RELEASE_FAST: opt_str = "release-fast"; break;
        default: opt_str = "unknown"; break;
    }

    JanetTable *t = janet_table(2);
    janet_table_put(t, janet_ckeywordv("simd"), janet_wrap_boolean(simd));
    janet_table_put(t, janet_ckeywordv("optimize"), janet_ckeywordv(opt_str));
    return janet_wrap_table(t);
}

/* ------------------------------------------------------------------ */
/* PTY helpers                                                        */
/* ------------------------------------------------------------------ */

static Janet cfun_pty_spawn(int32_t argc, Janet *argv) {
    janet_arity(argc, 4, 5);
    uint16_t cols = (uint16_t) janet_getinteger(argv, 0);
    uint16_t rows = (uint16_t) janet_getinteger(argv, 1);
    int cell_w = janet_getinteger(argv, 2);
    int cell_h = janet_getinteger(argv, 3);
    const char *shell_opt = janet_optcstring(argv, argc, 4, NULL);

    struct winsize ws = {
        .ws_row = rows,
        .ws_col = cols,
        .ws_xpixel = (unsigned short) (cols * cell_w),
        .ws_ypixel = (unsigned short) (rows * cell_h),
    };

    int pty_fd = -1;
    pid_t child = forkpty(&pty_fd, NULL, NULL, &ws);
    if (child < 0) {
        janet_panicf("forkpty failed: %s", strerror(errno));
    }

    if (child == 0) {
        /* Child: exec the user's shell on the slave side. */
        const char *shell = shell_opt;
        if (!shell || !*shell) shell = getenv("SHELL");
        if (!shell || !*shell) {
            struct passwd *pw = getpwuid(getuid());
            if (pw && pw->pw_shell && pw->pw_shell[0]) shell = pw->pw_shell;
            else shell = "/bin/sh";
        }
        const char *name = strrchr(shell, '/');
        name = name ? name + 1 : shell;
        setenv("TERM", "xterm-256color", 1);
        execl(shell, name, NULL);
        _exit(127);
    }

    /* Parent: non-blocking master so the render loop never stalls. */
    int flags = fcntl(pty_fd, F_GETFL);
    if (flags < 0 || fcntl(pty_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(pty_fd);
        janet_panicf("fcntl O_NONBLOCK failed: %s", strerror(errno));
    }

    JanetTable *t = janet_table(2);
    janet_table_put(t, janet_ckeywordv("fd"), janet_wrap_integer(pty_fd));
    janet_table_put(t, janet_ckeywordv("pid"), janet_wrap_integer(child));
    return janet_wrap_table(t);
}

static Janet cfun_pty_resize(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 5);
    int fd = janet_getinteger(argv, 0);
    uint16_t cols = (uint16_t) janet_getinteger(argv, 1);
    uint16_t rows = (uint16_t) janet_getinteger(argv, 2);
    int cell_w = janet_getinteger(argv, 3);
    int cell_h = janet_getinteger(argv, 4);
    struct winsize ws = {
        .ws_row = rows,
        .ws_col = cols,
        .ws_xpixel = (unsigned short) (cols * cell_w),
        .ws_ypixel = (unsigned short) (rows * cell_h),
    };
    ioctl(fd, TIOCSWINSZ, &ws);
    return janet_wrap_nil();
}

/* Drain the master fd. Returns a buffer with data, nil when there is
 * nothing right now (EAGAIN), or :eof when the child closed the pty. */
static Janet cfun_pty_read(int32_t argc, Janet *argv) {
    janet_arity(argc, 1, 2);
    int fd = janet_getinteger(argv, 0);
    int32_t max = janet_optinteger(argv, argc, 1, 65536);
    if (max < 1) max = 1;

    JanetBuffer *buf = janet_buffer(max < 4096 ? max : 4096);
    uint8_t tmp[4096];
    for (;;) {
        ssize_t n = read(fd, tmp, sizeof(tmp));
        if (n > 0) {
            janet_buffer_push_bytes(buf, tmp, (int32_t) n);
            if ((int32_t) buf->count >= max) break;
        } else if (n == 0) {
            if (buf->count == 0) return janet_ckeywordv("eof");
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            if (errno == EIO) {
                if (buf->count == 0) return janet_ckeywordv("eof");
                break;
            }
            janet_panicf("pty read failed: %s", strerror(errno));
        }
    }
    if (buf->count == 0) return janet_wrap_nil();
    return janet_wrap_buffer(buf);
}

static Janet cfun_pty_write(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 2);
    int fd = janet_getinteger(argv, 0);
    JanetByteView bv = janet_getbytes(argv, 1);
    size_t off = 0;
    while (off < (size_t) bv.len) {
        ssize_t n = write(fd, bv.bytes + off, (size_t) bv.len - off);
        if (n > 0) {
            off += (size_t) n;
        } else if (n < 0) {
            if (errno == EINTR) continue;
            break; /* EAGAIN / closed: drop the rest, like a real terminal */
        }
    }
    return janet_wrap_integer((int32_t) off);
}

static Janet cfun_pty_close(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);
    int fd = janet_getinteger(argv, 0);
    if (fd >= 0) close(fd);
    return janet_wrap_nil();
}

/* Non-blocking reap. Returns nil while the child is alive, otherwise its
 * exit status (128 + signal for signal deaths). */
static Janet cfun_pty_wait(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);
    pid_t pid = (pid_t) janet_getinteger(argv, 0);
    int status = 0;
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r <= 0) return janet_wrap_nil();
    if (WIFEXITED(status)) return janet_wrap_integer(WEXITSTATUS(status));
    if (WIFSIGNALED(status)) return janet_wrap_integer(128 + WTERMSIG(status));
    return janet_wrap_integer(status);
}

static Janet cfun_pty_kill(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 2);
    pid_t pid = (pid_t) janet_getinteger(argv, 0);
    int sig = janet_getinteger(argv, 1);
    return janet_wrap_boolean(kill(pid, sig) == 0);
}

/* ------------------------------------------------------------------ */
/* Terminal                                                           */
/* ------------------------------------------------------------------ */

static Janet cfun_terminal_new(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 2);
    uint16_t cols = (uint16_t) janet_getinteger(argv, 0);
    uint16_t rows = (uint16_t) janet_getinteger(argv, 1);

    TermCtx *ctx = janet_abstract(&AT_Terminal, sizeof(TermCtx));
    memset(ctx, 0, sizeof(*ctx));
    ctx->callbacks = janet_wrap_nil();
    ctx->cols = cols;
    ctx->rows = rows;

    GhosttyResult err = ghostty_terminal_new(NULL, &ctx->term, cols, rows);
    if (err != GHOSTTY_SUCCESS) {
        janet_panicf("ghostty_terminal_new failed (%d)", (int) err);
    }

    /* Effect callbacks are process-global in spirit but installed per
     * terminal. The userdata pointer is our abstract data. */
    ghostty_terminal_set(ctx->term, GHOSTTY_TERMINAL_OPT_USERDATA, ctx);
    ghostty_terminal_set(ctx->term, GHOSTTY_TERMINAL_OPT_WRITE_PTY,
                         (const void *) cb_write_pty);
    ghostty_terminal_set(ctx->term, GHOSTTY_TERMINAL_OPT_SIZE,
                         (const void *) cb_size);
    ghostty_terminal_set(ctx->term, GHOSTTY_TERMINAL_OPT_DEVICE_ATTRIBUTES,
                         (const void *) cb_device_attributes);
    ghostty_terminal_set(ctx->term, GHOSTTY_TERMINAL_OPT_XTVERSION,
                         (const void *) cb_xtversion);
    ghostty_terminal_set(ctx->term, GHOSTTY_TERMINAL_OPT_COLOR_SCHEME,
                         (const void *) cb_color_scheme);
    ghostty_terminal_set(ctx->term, GHOSTTY_TERMINAL_OPT_TITLE_CHANGED,
                         (const void *) cb_title_changed);

    return janet_wrap_abstract(ctx);
}

static Janet cfun_terminal_write(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 2);
    TermCtx *ctx = janet_getabstract(argv, 0, &AT_Terminal);
    JanetByteView bv = janet_getbytes(argv, 1);
    ghostty_terminal_vt_write(ctx->term, bv.bytes, (size_t) bv.len);
    return janet_wrap_nil();
}

static Janet cfun_terminal_resize(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 5);
    TermCtx *ctx = janet_getabstract(argv, 0, &AT_Terminal);
    ctx->cols = (uint16_t) janet_getinteger(argv, 1);
    ctx->rows = (uint16_t) janet_getinteger(argv, 2);
    ctx->cell_w = (uint32_t) janet_getinteger(argv, 3);
    ctx->cell_h = (uint32_t) janet_getinteger(argv, 4);
    GhosttyResult err = ghostty_terminal_resize(ctx->term, ctx->cols, ctx->rows,
                                                ctx->cell_w, ctx->cell_h);
    if (err != GHOSTTY_SUCCESS) {
        janet_panicf("ghostty_terminal_resize failed (%d)", (int) err);
    }
    return janet_wrap_nil();
}

static Janet cfun_terminal_scroll(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 2);
    TermCtx *ctx = janet_getabstract(argv, 0, &AT_Terminal);
    intptr_t delta = (intptr_t) janet_getinteger64(argv, 1);
    GhosttyTerminalScrollViewport sv = {
        .tag = GHOSTTY_SCROLL_VIEWPORT_DELTA,
        .value = { .delta = delta },
    };
    ghostty_terminal_scroll_viewport(ctx->term, sv);
    return janet_wrap_nil();
}

static Janet cfun_terminal_set_scrollback(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 2);
    TermCtx *ctx = janet_getabstract(argv, 0, &AT_Terminal);
    size_t n = (size_t) janet_getinteger64(argv, 1);
    ghostty_terminal_set(ctx->term, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_LINES, &n);
    return janet_wrap_nil();
}

static Janet cfun_terminal_set_effects(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 2);
    TermCtx *ctx = janet_getabstract(argv, 0, &AT_Terminal);
    ctx->callbacks = argv[1];
    return janet_wrap_nil();
}

static Janet cfun_terminal_title(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);
    TermCtx *ctx = janet_getabstract(argv, 0, &AT_Terminal);
    GhosttyString title = {0};
    if (ghostty_terminal_get(ctx->term, GHOSTTY_TERMINAL_DATA_TITLE, &title) != GHOSTTY_SUCCESS)
        return janet_cstringv("");
    return janet_stringv(title.ptr, title.len);
}

static Janet cfun_terminal_mouse_tracking(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);
    TermCtx *ctx = janet_getabstract(argv, 0, &AT_Terminal);
    bool v = false;
    ghostty_terminal_get(ctx->term, GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING, &v);
    return janet_wrap_boolean(v);
}

static Janet cfun_terminal_focus_event(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);
    TermCtx *ctx = janet_getabstract(argv, 0, &AT_Terminal);
    GhosttyTerminalModeConfig mode = { .mode = GHOSTTY_MODE_FOCUS_EVENT, .value = false };
    if (ghostty_terminal_get(ctx->term, GHOSTTY_TERMINAL_DATA_MODE, &mode) != GHOSTTY_SUCCESS)
        return janet_wrap_boolean(false);
    return janet_wrap_boolean(mode.value);
}

static Janet cfun_terminal_scrollbar(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);
    TermCtx *ctx = janet_getabstract(argv, 0, &AT_Terminal);
    GhosttyTerminalScrollbar sb = {0};
    if (ghostty_terminal_get(ctx->term, GHOSTTY_TERMINAL_DATA_SCROLLBAR, &sb) != GHOSTTY_SUCCESS)
        return janet_wrap_nil();
    JanetTable *t = janet_table(3);
    janet_table_put(t, janet_ckeywordv("total"), janet_wrap_number((double) sb.total));
    janet_table_put(t, janet_ckeywordv("offset"), janet_wrap_number((double) sb.offset));
    janet_table_put(t, janet_ckeywordv("len"), janet_wrap_number((double) sb.len));
    return janet_wrap_table(t);
}

static Janet cfun_terminal_cursor_visible(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);
    TermCtx *ctx = janet_getabstract(argv, 0, &AT_Terminal);
    bool v = false;
    ghostty_terminal_get(ctx->term, GHOSTTY_TERMINAL_DATA_CURSOR_VISIBLE, &v);
    return janet_wrap_boolean(v);
}

/* ------------------------------------------------------------------ */
/* Key encoding                                                       */
/* ------------------------------------------------------------------ */

static Janet cfun_key_encoder_new(int32_t argc, Janet *argv) {
    (void) argv;
    janet_fixarity(argc, 0);
    KeyCtx *ctx = janet_abstract(&AT_Key, sizeof(KeyCtx));
    memset(ctx, 0, sizeof(*ctx));
    if (ghostty_key_encoder_new(NULL, &ctx->encoder) != GHOSTTY_SUCCESS ||
        ghostty_key_event_new(NULL, &ctx->event) != GHOSTTY_SUCCESS) {
        janet_panic("failed to create key encoder");
    }
    return janet_wrap_abstract(ctx);
}

/* (key-encode encoder terminal key action mods unshifted-cp consumed-mods text) */
static Janet cfun_key_encode(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 8);
    KeyCtx *kctx = janet_getabstract(argv, 0, &AT_Key);
    TermCtx *tctx = janet_getabstract(argv, 1, &AT_Terminal);
    GhosttyKey key = (GhosttyKey) janet_getinteger(argv, 2);
    GhosttyKeyAction action = (GhosttyKeyAction) janet_getinteger(argv, 3);
    GhosttyMods mods = (GhosttyMods) janet_getinteger(argv, 4);
    uint32_t unshifted = (uint32_t) janet_getinteger(argv, 5);
    GhosttyMods consumed = (GhosttyMods) janet_getinteger(argv, 6);

    ghostty_key_encoder_setopt_from_terminal(kctx->encoder, tctx->term);
    ghostty_key_event_set_key(kctx->event, key);
    ghostty_key_event_set_action(kctx->event, action);
    ghostty_key_event_set_mods(kctx->event, mods);
    ghostty_key_event_set_unshifted_codepoint(kctx->event, unshifted);
    ghostty_key_event_set_consumed_mods(kctx->event, consumed);

    if (!janet_checktype(argv[7], JANET_NIL)) {
        JanetByteView bv = janet_getbytes(argv, 7);
        ghostty_key_event_set_utf8(kctx->event, (const char *) bv.bytes, (size_t) bv.len);
    } else {
        ghostty_key_event_set_utf8(kctx->event, NULL, 0);
    }

    char buf[256];
    size_t written = 0;
    GhosttyResult res = ghostty_key_encoder_encode(kctx->encoder, kctx->event,
                                                   buf, sizeof(buf), &written);
    if (res == GHOSTTY_OUT_OF_SPACE) {
        char *big = janet_smalloc(written);
        res = ghostty_key_encoder_encode(kctx->encoder, kctx->event,
                                         big, written, &written);
        if (res == GHOSTTY_SUCCESS && written > 0) {
            Janet out = janet_stringv((const uint8_t *) big, written);
            janet_sfree(big);
            return out;
        }
        janet_sfree(big);
        return janet_wrap_nil();
    }
    if (res != GHOSTTY_SUCCESS || written == 0) return janet_wrap_nil();
    return janet_stringv((const uint8_t *) buf, written);
}

/* ------------------------------------------------------------------ */
/* Mouse encoding                                                     */
/* ------------------------------------------------------------------ */

static Janet cfun_mouse_encoder_new(int32_t argc, Janet *argv) {
    (void) argv;
    janet_fixarity(argc, 0);
    MouseCtx *ctx = janet_abstract(&AT_Mouse, sizeof(MouseCtx));
    memset(ctx, 0, sizeof(*ctx));
    if (ghostty_mouse_encoder_new(NULL, &ctx->encoder) != GHOSTTY_SUCCESS ||
        ghostty_mouse_event_new(NULL, &ctx->event) != GHOSTTY_SUCCESS) {
        janet_panic("failed to create mouse encoder");
    }
    return janet_wrap_abstract(ctx);
}

/* (mouse-set-geometry encoder screen-w screen-h cell-w cell-h pad) */
static Janet cfun_mouse_set_geometry(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 6);
    MouseCtx *ctx = janet_getabstract(argv, 0, &AT_Mouse);
    ctx->screen_w = (uint32_t) janet_getinteger(argv, 1);
    ctx->screen_h = (uint32_t) janet_getinteger(argv, 2);
    ctx->cell_w = (uint32_t) janet_getinteger(argv, 3);
    ctx->cell_h = (uint32_t) janet_getinteger(argv, 4);
    ctx->pad = (uint32_t) janet_getinteger(argv, 5);
    return janet_wrap_nil();
}

/* (mouse-encode encoder terminal action button x y mods any-pressed) */
static Janet cfun_mouse_encode(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 8);
    MouseCtx *mctx = janet_getabstract(argv, 0, &AT_Mouse);
    TermCtx *tctx = janet_getabstract(argv, 1, &AT_Terminal);
    GhosttyMouseAction action = (GhosttyMouseAction) janet_getinteger(argv, 2);
    int button = janet_getinteger(argv, 3);
    double x = janet_getnumber(argv, 4);
    double y = janet_getnumber(argv, 5);
    GhosttyMods mods = (GhosttyMods) janet_getinteger(argv, 6);
    bool any_pressed = janet_getboolean(argv, 7);

    ghostty_mouse_encoder_setopt_from_terminal(mctx->encoder, tctx->term);

    GhosttyMouseEncoderSize size = {
        .size = sizeof(GhosttyMouseEncoderSize),
        .screen_width = mctx->screen_w,
        .screen_height = mctx->screen_h,
        .cell_width = mctx->cell_w ? mctx->cell_w : 1,
        .cell_height = mctx->cell_h ? mctx->cell_h : 1,
        .padding_top = mctx->pad,
        .padding_bottom = mctx->pad,
        .padding_left = mctx->pad,
        .padding_right = mctx->pad,
    };
    ghostty_mouse_encoder_setopt(mctx->encoder, GHOSTTY_MOUSE_ENCODER_OPT_SIZE, &size);
    ghostty_mouse_encoder_setopt(mctx->encoder,
                                 GHOSTTY_MOUSE_ENCODER_OPT_ANY_BUTTON_PRESSED, &any_pressed);
    bool track = true;
    ghostty_mouse_encoder_setopt(mctx->encoder,
                                 GHOSTTY_MOUSE_ENCODER_OPT_TRACK_LAST_CELL, &track);

    ghostty_mouse_event_set_mods(mctx->event, mods);
    ghostty_mouse_event_set_position(mctx->event, (GhosttyMousePosition){ .x = x, .y = y });
    ghostty_mouse_event_set_action(mctx->event, action);
    if (button <= 0) ghostty_mouse_event_clear_button(mctx->event);
    else ghostty_mouse_event_set_button(mctx->event, (GhosttyMouseButton) button);

    char buf[256];
    size_t written = 0;
    GhosttyResult res = ghostty_mouse_encoder_encode(mctx->encoder, mctx->event,
                                                     buf, sizeof(buf), &written);
    if (res == GHOSTTY_OUT_OF_SPACE) {
        char *big = janet_smalloc(written);
        res = ghostty_mouse_encoder_encode(mctx->encoder, mctx->event,
                                           big, written, &written);
        if (res == GHOSTTY_SUCCESS && written > 0) {
            Janet out = janet_stringv((const uint8_t *) big, written);
            janet_sfree(big);
            return out;
        }
        janet_sfree(big);
        return janet_wrap_nil();
    }
    if (res != GHOSTTY_SUCCESS || written == 0) return janet_wrap_nil();
    return janet_stringv((const uint8_t *) buf, written);
}

/* ------------------------------------------------------------------ */
/* Focus encoding                                                     */
/* ------------------------------------------------------------------ */

static Janet cfun_focus_encode(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);
    bool gained = janet_getboolean(argv, 0);
    GhosttyFocusEvent ev = gained ? GHOSTTY_FOCUS_GAINED : GHOSTTY_FOCUS_LOST;
    char buf[16];
    size_t written = 0;
    if (ghostty_focus_encode(ev, buf, sizeof(buf), &written) != GHOSTTY_SUCCESS)
        return janet_wrap_nil();
    return janet_stringv((const uint8_t *) buf, written);
}

/* ------------------------------------------------------------------ */
/* Render state                                                       */
/* ------------------------------------------------------------------ */

static Janet cfun_render_new(int32_t argc, Janet *argv) {
    (void) argv;
    janet_fixarity(argc, 0);
    RenderCtx *ctx = janet_abstract(&AT_Render, sizeof(RenderCtx));
    memset(ctx, 0, sizeof(*ctx));
    if (ghostty_render_state_new(NULL, &ctx->state) != GHOSTTY_SUCCESS ||
        ghostty_render_state_row_iterator_new(NULL, &ctx->rows) != GHOSTTY_SUCCESS ||
        ghostty_render_state_row_cells_new(NULL, &ctx->cells) != GHOSTTY_SUCCESS) {
        janet_panic("failed to create render state");
    }
    return janet_wrap_abstract(ctx);
}

static Janet cfun_render_update(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 2);
    RenderCtx *rctx = janet_getabstract(argv, 0, &AT_Render);
    TermCtx *tctx = janet_getabstract(argv, 1, &AT_Terminal);
    if (ghostty_render_state_update(rctx->state, tctx->term) != GHOSTTY_SUCCESS)
        janet_panic("ghostty_render_state_update failed");
    return janet_wrap_nil();
}

static Janet rgb_tuple(GhosttyColorRgb c) {
    Janet *t = janet_tuple_begin(3);
    t[0] = janet_wrap_integer(c.r);
    t[1] = janet_wrap_integer(c.g);
    t[2] = janet_wrap_integer(c.b);
    return janet_wrap_tuple(janet_tuple_end(t));
}

static Janet cfun_render_colors(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);
    RenderCtx *ctx = janet_getabstract(argv, 0, &AT_Render);
    GhosttyRenderStateColors colors = GHOSTTY_INIT_SIZED(GhosttyRenderStateColors);
    if (ghostty_render_state_get(ctx->state, GHOSTTY_RENDER_STATE_DATA_COLORS,
                                 &colors) != GHOSTTY_SUCCESS)
        return janet_wrap_nil();
    JanetTable *t = janet_table(4);
    janet_table_put(t, janet_ckeywordv("bg"), rgb_tuple(colors.background));
    janet_table_put(t, janet_ckeywordv("fg"), rgb_tuple(colors.foreground));
    if (colors.cursor_has_value)
        janet_table_put(t, janet_ckeywordv("cursor"), rgb_tuple(colors.cursor));
    janet_table_put(t, janet_ckeywordv("cursor?"), janet_wrap_boolean(colors.cursor_has_value));
    return janet_wrap_table(t);
}

static Janet cfun_render_cursor(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);
    RenderCtx *ctx = janet_getabstract(argv, 0, &AT_Render);
    bool visible = false;
    bool has_value = false;
    uint16_t x = 0, y = 0;
    ghostty_render_state_get(ctx->state, GHOSTTY_RENDER_STATE_DATA_CURSOR_VISIBLE, &visible);
    ghostty_render_state_get(ctx->state, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_HAS_VALUE, &has_value);
    ghostty_render_state_get(ctx->state, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_X, &x);
    ghostty_render_state_get(ctx->state, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_Y, &y);
    JanetTable *t = janet_table(4);
    janet_table_put(t, janet_ckeywordv("visible"), janet_wrap_boolean(visible));
    janet_table_put(t, janet_ckeywordv("has-value"), janet_wrap_boolean(has_value));
    janet_table_put(t, janet_ckeywordv("x"), janet_wrap_integer(x));
    janet_table_put(t, janet_ckeywordv("y"), janet_wrap_integer(y));
    return janet_wrap_table(t);
}

/* Extract the whole visible grid as nested arrays of tables. Each cell:
 *   {:text string? :fg [r g b]? :bg [r g b]? :bold bool :italic bool :inverse bool}
 */
static Janet cfun_render_rows(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);
    RenderCtx *ctx = janet_getabstract(argv, 0, &AT_Render);

    JanetArray *out = janet_array(32);
    if (ghostty_render_state_get(ctx->state, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR,
                                 &ctx->rows) != GHOSTTY_SUCCESS)
        return janet_wrap_array(out);

    while (ghostty_render_state_row_iterator_next(ctx->rows)) {
        if (ghostty_render_state_row_get(ctx->rows, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS,
                                         &ctx->cells) != GHOSTTY_SUCCESS)
            continue;

        JanetArray *row = janet_array(80);

        while (ghostty_render_state_row_cells_next(ctx->cells)) {
            JanetTable *cell = janet_table(6);

            uint32_t glen = 0;
            ghostty_render_state_row_cells_get(ctx->cells,
                GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN, &glen);
            if (glen > 0) {
                char text[256];
                GhosttyBuffer gb = {
                    .ptr = (uint8_t *) text,
                    .cap = sizeof(text),
                    .len = 0,
                };
                GhosttyResult r = ghostty_render_state_row_cells_get(ctx->cells,
                    GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8, &gb);
                if (r == GHOSTTY_SUCCESS && gb.len > 0)
                    janet_table_put(cell, janet_ckeywordv("text"),
                                    janet_stringv((const uint8_t *) text, gb.len));
            }

            GhosttyColorRgb fg;
            if (ghostty_render_state_row_cells_get(ctx->cells,
                    GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR, &fg) == GHOSTTY_SUCCESS)
                janet_table_put(cell, janet_ckeywordv("fg"), rgb_tuple(fg));

            GhosttyColorRgb bg;
            if (ghostty_render_state_row_cells_get(ctx->cells,
                    GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR, &bg) == GHOSTTY_SUCCESS)
                janet_table_put(cell, janet_ckeywordv("bg"), rgb_tuple(bg));

            GhosttyStyle style = GHOSTTY_INIT_SIZED(GhosttyStyle);
            if (ghostty_render_state_row_cells_get(ctx->cells,
                    GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &style) == GHOSTTY_SUCCESS) {
                if (style.bold) janet_table_put(cell, janet_ckeywordv("bold"), janet_wrap_true());
                if (style.italic) janet_table_put(cell, janet_ckeywordv("italic"), janet_wrap_true());
                if (style.inverse) janet_table_put(cell, janet_ckeywordv("inverse"), janet_wrap_true());
            }

            janet_array_push(row, janet_wrap_table(cell));
        }

        /* Clear the row's dirty flag so the next update only reports changes. */
        bool clean = false;
        ghostty_render_state_row_set(ctx->rows, GHOSTTY_RENDER_STATE_ROW_OPTION_DIRTY, &clean);

        janet_array_push(out, janet_wrap_array(row));
    }

    return janet_wrap_array(out);
}

static Janet cfun_render_clear_dirty(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);
    RenderCtx *ctx = janet_getabstract(argv, 0, &AT_Render);
    GhosttyRenderStateDirty clean = GHOSTTY_RENDER_STATE_DIRTY_FALSE;
    ghostty_render_state_set(ctx->state, GHOSTTY_RENDER_STATE_OPTION_DIRTY, &clean);
    return janet_wrap_nil();
}

/* ------------------------------------------------------------------ */
/* Module registration                                                */
/* ------------------------------------------------------------------ */

static const JanetReg cfuns[] = {
    {"build-info", cfun_build_info, "(ghostty/build-info)\n\nReturn build configuration."},

    {"pty-spawn", cfun_pty_spawn, "(ghostty/pty-spawn cols rows cell-w cell-h &opt shell)\n\nFork the shell on a new pty. Returns {:fd :pid}."},
    {"pty-resize", cfun_pty_resize, "(ghostty/pty-resize fd cols rows cell-w cell-h)"},
    {"pty-read", cfun_pty_read, "(ghostty/pty-read fd &opt max)\n\nBuffer of data, nil if none, or :eof."},
    {"pty-write", cfun_pty_write, "(ghostty/pty-write fd data)"},
    {"pty-close", cfun_pty_close, "(ghostty/pty-close fd)"},
    {"pty-wait", cfun_pty_wait, "(ghostty/pty-wait pid)\n\nnil while alive, else exit status."},
    {"pty-kill", cfun_pty_kill, "(ghostty/pty-kill pid signal)"},

    {"terminal-new", cfun_terminal_new, "(ghostty/terminal-new cols rows)"},
    {"terminal-write", cfun_terminal_write, "(ghostty/terminal-write term data)"},
    {"terminal-resize", cfun_terminal_resize, "(ghostty/terminal-resize term cols rows cell-w cell-h)"},
    {"terminal-scroll", cfun_terminal_scroll, "(ghostty/terminal-scroll term delta)"},
    {"terminal-set-scrollback", cfun_terminal_set_scrollback, "(ghostty/terminal-set-scrollback term lines)"},
    {"terminal-set-effects", cfun_terminal_set_effects, "(ghostty/terminal-set-effects term callbacks)\n\ncallbacks: {:write-pty (fn [bytes]) :title-changed (fn [title])}"},
    {"terminal-title", cfun_terminal_title, "(ghostty/terminal-title term)"},
    {"terminal-mouse-tracking?", cfun_terminal_mouse_tracking, "(ghostty/terminal-mouse-tracking? term)"},
    {"terminal-focus-event?", cfun_terminal_focus_event, "(ghostty/terminal-focus-event? term)"},
    {"terminal-scrollbar", cfun_terminal_scrollbar, "(ghostty/terminal-scrollbar term)"},
    {"terminal-cursor-visible?", cfun_terminal_cursor_visible, "(ghostty/terminal-cursor-visible? term)"},

    {"key-encoder-new", cfun_key_encoder_new, "(ghostty/key-encoder-new)"},
    {"key-encode", cfun_key_encode, "(ghostty/key-encode enc term key action mods unshifted-cp consumed-mods text)"},

    {"mouse-encoder-new", cfun_mouse_encoder_new, "(ghostty/mouse-encoder-new)"},
    {"mouse-set-geometry", cfun_mouse_set_geometry, "(ghostty/mouse-set-geometry enc screen-w screen-h cell-w cell-h pad)"},
    {"mouse-encode", cfun_mouse_encode, "(ghostty/mouse-encode enc term action button x y mods any-pressed)"},

    {"focus-encode", cfun_focus_encode, "(ghostty/focus-encode gained?)"},

    {"render-new", cfun_render_new, "(ghostty/render-new)"},
    {"render-update", cfun_render_update, "(ghostty/render-update render term)"},
    {"render-colors", cfun_render_colors, "(ghostty/render-colors render)"},
    {"render-cursor", cfun_render_cursor, "(ghostty/render-cursor render)"},
    {"render-rows", cfun_render_rows, "(ghostty/render-rows render)"},
    {"render-clear-dirty", cfun_render_clear_dirty, "(ghostty/render-clear-dirty render)"},

    {NULL, NULL, NULL}
};

/* Keyword -> GhosttyKey table for the keys the emulator maps. */
typedef struct { const char *name; int code; } KeyDef;

static const KeyDef key_table[] = {
    {"a", GHOSTTY_KEY_A}, {"b", GHOSTTY_KEY_B}, {"c", GHOSTTY_KEY_C},
    {"d", GHOSTTY_KEY_D}, {"e", GHOSTTY_KEY_E}, {"f", GHOSTTY_KEY_F},
    {"g", GHOSTTY_KEY_G}, {"h", GHOSTTY_KEY_H}, {"i", GHOSTTY_KEY_I},
    {"j", GHOSTTY_KEY_J}, {"k", GHOSTTY_KEY_K}, {"l", GHOSTTY_KEY_L},
    {"m", GHOSTTY_KEY_M}, {"n", GHOSTTY_KEY_N}, {"o", GHOSTTY_KEY_O},
    {"p", GHOSTTY_KEY_P}, {"q", GHOSTTY_KEY_Q}, {"r", GHOSTTY_KEY_R},
    {"s", GHOSTTY_KEY_S}, {"t", GHOSTTY_KEY_T}, {"u", GHOSTTY_KEY_U},
    {"v", GHOSTTY_KEY_V}, {"w", GHOSTTY_KEY_W}, {"x", GHOSTTY_KEY_X},
    {"y", GHOSTTY_KEY_Y}, {"z", GHOSTTY_KEY_Z},
    {"0", GHOSTTY_KEY_DIGIT_0}, {"1", GHOSTTY_KEY_DIGIT_1},
    {"2", GHOSTTY_KEY_DIGIT_2}, {"3", GHOSTTY_KEY_DIGIT_3},
    {"4", GHOSTTY_KEY_DIGIT_4}, {"5", GHOSTTY_KEY_DIGIT_5},
    {"6", GHOSTTY_KEY_DIGIT_6}, {"7", GHOSTTY_KEY_DIGIT_7},
    {"8", GHOSTTY_KEY_DIGIT_8}, {"9", GHOSTTY_KEY_DIGIT_9},
    {"space", GHOSTTY_KEY_SPACE}, {"enter", GHOSTTY_KEY_ENTER},
    {"tab", GHOSTTY_KEY_TAB}, {"backspace", GHOSTTY_KEY_BACKSPACE},
    {"delete", GHOSTTY_KEY_DELETE}, {"escape", GHOSTTY_KEY_ESCAPE},
    {"up", GHOSTTY_KEY_ARROW_UP}, {"down", GHOSTTY_KEY_ARROW_DOWN},
    {"left", GHOSTTY_KEY_ARROW_LEFT}, {"right", GHOSTTY_KEY_ARROW_RIGHT},
    {"home", GHOSTTY_KEY_HOME}, {"end", GHOSTTY_KEY_END},
    {"page-up", GHOSTTY_KEY_PAGE_UP}, {"page-down", GHOSTTY_KEY_PAGE_DOWN},
    {"insert", GHOSTTY_KEY_INSERT},
    {"-", GHOSTTY_KEY_MINUS}, {"=", GHOSTTY_KEY_EQUAL},
    {"[", GHOSTTY_KEY_BRACKET_LEFT}, {"]", GHOSTTY_KEY_BRACKET_RIGHT},
    {"\\", GHOSTTY_KEY_BACKSLASH}, {";", GHOSTTY_KEY_SEMICOLON},
    {"'", GHOSTTY_KEY_QUOTE}, {",", GHOSTTY_KEY_COMMA},
    {".", GHOSTTY_KEY_PERIOD}, {"/", GHOSTTY_KEY_SLASH},
    {"`", GHOSTTY_KEY_BACKQUOTE},
    {"f1", GHOSTTY_KEY_F1}, {"f2", GHOSTTY_KEY_F2}, {"f3", GHOSTTY_KEY_F3},
    {"f4", GHOSTTY_KEY_F4}, {"f5", GHOSTTY_KEY_F5}, {"f6", GHOSTTY_KEY_F6},
    {"f7", GHOSTTY_KEY_F7}, {"f8", GHOSTTY_KEY_F8}, {"f9", GHOSTTY_KEY_F9},
    {"f10", GHOSTTY_KEY_F10}, {"f11", GHOSTTY_KEY_F11}, {"f12", GHOSTTY_KEY_F12},
};

JANET_MODULE_ENTRY(JanetTable *env) {
    janet_cfuns(env, "ghostty", cfuns);

    JanetTable *keys = janet_table((int32_t) (sizeof(key_table) / sizeof(key_table[0])));
    for (size_t i = 0; i < sizeof(key_table) / sizeof(key_table[0]); i++)
        janet_table_put(keys, janet_ckeywordv(key_table[i].name),
                        janet_wrap_integer(key_table[i].code));
    janet_def(env, "keys", janet_wrap_table(keys),
              "Map of keyword -> GhosttyKey code.");

    JanetTable *mods = janet_table(8);
    janet_table_put(mods, janet_ckeywordv("shift"), janet_wrap_integer(GHOSTTY_MODS_SHIFT));
    janet_table_put(mods, janet_ckeywordv("ctrl"), janet_wrap_integer(GHOSTTY_MODS_CTRL));
    janet_table_put(mods, janet_ckeywordv("alt"), janet_wrap_integer(GHOSTTY_MODS_ALT));
    janet_table_put(mods, janet_ckeywordv("super"), janet_wrap_integer(GHOSTTY_MODS_SUPER));
    janet_def(env, "mods", janet_wrap_table(mods),
              "Map of modifier keyword -> bitmask.");

    JanetTable *actions = janet_table(3);
    janet_table_put(actions, janet_ckeywordv("press"), janet_wrap_integer(GHOSTTY_KEY_ACTION_PRESS));
    janet_table_put(actions, janet_ckeywordv("release"), janet_wrap_integer(GHOSTTY_KEY_ACTION_RELEASE));
    janet_table_put(actions, janet_ckeywordv("repeat"), janet_wrap_integer(GHOSTTY_KEY_ACTION_REPEAT));
    janet_def(env, "key-actions", janet_wrap_table(actions),
              "Map of key action keyword -> GhosttyKeyAction.");

    JanetTable *mbuttons = janet_table(8);
    janet_table_put(mbuttons, janet_ckeywordv("left"), janet_wrap_integer(GHOSTTY_MOUSE_BUTTON_LEFT));
    janet_table_put(mbuttons, janet_ckeywordv("right"), janet_wrap_integer(GHOSTTY_MOUSE_BUTTON_RIGHT));
    janet_table_put(mbuttons, janet_ckeywordv("middle"), janet_wrap_integer(GHOSTTY_MOUSE_BUTTON_MIDDLE));
    janet_table_put(mbuttons, janet_ckeywordv("four"), janet_wrap_integer(GHOSTTY_MOUSE_BUTTON_FOUR));
    janet_table_put(mbuttons, janet_ckeywordv("five"), janet_wrap_integer(GHOSTTY_MOUSE_BUTTON_FIVE));
    janet_table_put(mbuttons, janet_ckeywordv("six"), janet_wrap_integer(GHOSTTY_MOUSE_BUTTON_SIX));
    janet_table_put(mbuttons, janet_ckeywordv("seven"), janet_wrap_integer(GHOSTTY_MOUSE_BUTTON_SEVEN));
    janet_def(env, "mouse-buttons", janet_wrap_table(mbuttons),
              "Map of mouse button keyword -> GhosttyMouseButton.");

    JanetTable *mactions = janet_table(3);
    janet_table_put(mactions, janet_ckeywordv("press"), janet_wrap_integer(GHOSTTY_MOUSE_ACTION_PRESS));
    janet_table_put(mactions, janet_ckeywordv("release"), janet_wrap_integer(GHOSTTY_MOUSE_ACTION_RELEASE));
    janet_table_put(mactions, janet_ckeywordv("motion"), janet_wrap_integer(GHOSTTY_MOUSE_ACTION_MOTION));
    janet_def(env, "mouse-actions", janet_wrap_table(mactions),
              "Map of mouse action keyword -> GhosttyMouseAction.");

    janet_def(env, "success", janet_wrap_integer(GHOSTTY_SUCCESS),
              "GHOSTTY_SUCCESS result code.");
}
