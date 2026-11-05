# libghostty-terminal

A terminal window in one C file, from [Elbcasts #011](https://youtu.be/4piXu2Kyi2s)
([elbcasts.dev](https://elbcasts.dev)).
[libghostty-vt](https://github.com/ghostty-org/ghostty) does the terminal
emulation: escape sequences, cursor, colors, scrollback, the alternate screen.
SDL2 opens the window and draws the glyphs, `forkpty` runs your shell.

About 180 lines, on purpose minimal: no font fallback, no emoji, no
selection, no resizing, and keys are mapped by hand instead of using
libghostty's key encoder (so Shift+Enter sends Enter). For a complete
minimal terminal, see [Ghostling](https://github.com/ghostty-org/ghostling).

## Build

Needs Zig 0.17, SDL2 and SDL2_ttf. Tested on macOS (Apple Silicon) only.

```sh
brew install zig sdl2 sdl2_ttf
zig build run
```

The first build compiles libghostty and takes about 40 seconds (M4 Pro);
after that, about a second.

If that first build drowns in `error: server connection failed:
TlsInitializationFailed`, it is Zig's TLS stack giving up on parallel
downloads, not a broken network: just run `zig build` again, each run brings
more of Ghostty's dependencies in.

Ghostty is pinned to one commit, because the libghostty API still changes.
To move to another commit:

```sh
COMMIT=c27ef4c1b109fb2b9947730c391f1a7d06eaa62f
zig fetch --save=ghostty \
  https://github.com/ghostty-org/ghostty/archive/$COMMIT.tar.gz
```

## Two macOS gotchas

- Only the static library is an artifact on macOS:
  `dep.artifact("ghostty-vt-static")`. `"ghostty-vt"` panics with
  `unable to find artifact`.
- Link SDL2_ttf with `.use_pkg_config = .no`. Its pkg-config entry adds
  `-lSDL2` a second time, and dyld refuses to start the program
  (`duplicate linked dylib`).

The font is Menlo (`FONT` in `src/main.c`).

## License

MIT, see [LICENSE](LICENSE). Ghostty itself is MIT, too.
