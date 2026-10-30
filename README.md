# grassy-terrain

A bare-minimum terminal emulator written in [Janet](https://janet-lang.org),
built on **[libghostty-vt](https://github.com/ghostty-org/ghostty)** for terminal
emulation and **[Jaylib](https://github.com/janet-lang/jaylib)** (raylib
bindings) for windowing and 2D rendering.

This is a Janet port of the ideas in
[ghostling](https://github.com/ghostty-org/ghostling), the single-file C demo
of the libghostty C API. libghostty does the hard part: VT parsing, screen
state, cursor, styles, text reflow, scrollback, mouse/key encoding, and the
Kitty keyboard protocol. This project is the glue: a PTY, an event loop, and a
per-cell renderer.

```
main.janet        the emulator: PTY plumbing, input forwarding, render loop
src/ghostty.c     Janet native bindings for libghostty-vt + POSIX PTY helpers
project.janet     jpm build definition for the native module
scripts/build.sh  builds libghostty-vt, Jaylib, and the module; writes build/gt
flake.nix         dev shell (zig 0.17 for libghostty, janet, X11/GL for raylib)
```

## Building

Requirements:

- Nix (the flake provides Zig 0.17, Janet, jpm, and the X11/OpenGL headers
  raylib needs)
- A local checkout of [ghostty](https://github.com/ghostty-org/ghostty) at
  `~/source/ghostty` (override with `GHOSTTY_SRC`)
- A local checkout of [jaylib](https://github.com/janet-lang/jaylib) at
  `~/source/jaylib` (override with `JAYLIB_SRC`)

```sh
./build.sh          # enters the Nix dev shell and builds everything
./build/gt          # run the terminal
```

Or, if you are already inside `nix develop`:

```sh
scripts/build.sh
./build/gt
```

`build.sh` is incremental: libghostty-vt and Jaylib are only rebuilt if their
artifacts are missing. The generated `build/gt` launcher embeds the exact
Janet interpreter used for the build and sets `JANET_PATH`,
`GRASSY_JAYLIB`, and `LD_LIBRARY_PATH` so it also runs outside the dev shell.

## Tests

A headless smoke test exercises the bindings (terminal state, effects,
rendering, key encoding, and a PTY round-trip) without opening a window:

```sh
./test/run.sh
```

## What works

- Spawns `$SHELL` (falling back to the passwd entry, then `/bin/sh`) on a PTY
- Full libghostty-vt terminal emulation: 24-bit and 256-color SGR, bold,
  italic, inverse, cursor rendering, Unicode graphemes
- Window resize with text reflow, forwarded to the child via `TIOCSWINSZ`
- Scrollback with mouse-wheel viewport scrolling
- Keyboard input encoded by libghostty (application cursor keys, modifier
  handling, Kitty keyboard protocol), including key repeat
- Mouse reporting forwarded through libghostty's mouse encoder
- Focus reporting (DECSET 1004) and window-title updates (OSC 0/2)
- Query responses (DA1/DA2/DA3, XTWINOPS size, XTVERSION) so programs like
  vim and tmux do not hang

## Deliberately omitted

To keep the example minimal, the following ghostling features are not ported:

- Kitty graphics protocol (images)
- Scrollbar drag-to-scroll (the scrollbar is drawn but not draggable)
- OSC clipboard integration

## How the bindings work

Janet's FFI cannot manufacture C function pointers, but libghostty-vt takes C
callbacks for its "effects" (write-to-pty, size reports, title changes, device
attributes, ...). `src/ghostty.c` is therefore a small native module that:

- stores Janet callbacks inside the terminal abstract and dispatches to them
  from C trampolines (using `janet_pcall` so a callback error cannot unwind
  through libghostty's C frames),
- flattens the render-state iterator into plain Janet data (`render-rows`
  returns nested arrays of cell tables), and
- exposes the PTY helpers and the key/mouse/focus encoders.

Raylib's `GetCharPressed` and `IsKeyPressedRepeat` are not correctly exposed by
Jaylib (the former has an `argv[1]` typo; the latter is unbound), so
`main.janet` reaches raylib's exported symbols through Janet's FFI instead.
