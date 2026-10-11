# lm10

A picker for Wayland, in the spirit of dmenu and fzf. lm10 reads a list of
entries on stdin and shows them in an overlay. Typing filters them, and picking
one prints it, runs it in place of lm10, or starts it detached. The same tool
works as an app launcher, a session menu or a directory jumper, depending on
what you feed it.

lm10 draws a [wlr-layer-shell] surface, so it needs a compositor that supports
that protocol: sway, Hyprland, KDE Plasma, niri, river, labwc and others.
GNOME doesn't.

[wlr-layer-shell]: https://wayland.app/protocols/wlr-layer-shell-unstable-v1

## Building

You need meson, ninja, pkg-config, wayland-scanner, and the development files
for wayland, wayland-protocols, pango, cairo and libxkbcommon.

```sh
meson setup bld/debug
ninja -C bld/debug
meson test -C bld/debug      # optional
```

With Nix, `nix build` builds the package, and `nix develop` gives a shell with
everything above.

## Entries

Each line of input is an entry, with shell-style quoting:

```
[-n NAME] [-x|-X] COMMAND [ARG...]
```

- `-n` sets the name that's shown and matched. It defaults to the command line.
- `-x` makes lm10 exec the command in its place, keeping its terminal.
- `-X` starts the command detached, in a new session.
- With neither, the command line is printed to stdout, shell-quoted, for the
  caller to use.

lm10 exits with 0 once something was picked, 130 if nothing was (the list was
cancelled), and 1 on an error.

## Configuration

lm10 reads `$XDG_CONFIG_HOME/lm10/lm10.ini`, or the file given with `-c PATH`.
[`examples/lm10.ini`](examples/lm10.ini) lists every setting, color and key
binding along with its default. Flags override the file for one run:

```sh
lm10 --width 40% --color background=#000000 --bind Escape=cancel
```

A key binding runs a list of actions joined by `+`. Besides editing and moving
the selection, actions can change the prompt, header and border label, reload
the entries from a command, or run a command against the selection:

```ini
[keys]
Control+r = change-prompt(📁 )+reload(fd -t d . ~)
Control+x = execute(rm -- {1..})+reload(ls)
```

Matching ignores case. By default the query must appear as is, and entries
keep their order. With `match = fuzzy` (or `--match fuzzy`), its characters
only have to appear in order, and the best matches come first: those on word
boundaries and next to each other.

`lm10 --help` gives a summary of the flags and actions.

Logging defaults to warnings. `-v` adds debug output, and `-vv` adds trace
output. `LM10_LOG=error|warn|info|debug|trace` sets the level as well.

## Examples

- [`examples/apps.sh`](examples/apps.sh) is an app launcher built on the
  installed `.desktop` files.
- [`examples/session.sh`](examples/session.sh) is a session menu (lock, log
  out, suspend, and so on).
- [`examples/jump.sh`](examples/jump.sh) is a `cd` into git repositories, for
  bash and zsh.
- [`examples/tmux.sh`](examples/tmux.sh) is a tmux session picker with key
  bindings that reload the list.

## License

MIT; see [LICENSE](LICENSE). The libraries in `third-party/` keep their own
licenses.
