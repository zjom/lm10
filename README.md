# lm10

a picker menu overlay for wayland (wlroots) based compositors.

![screenshot of lm10](./lm10-screenshot.png)

## usage

```sh
lm10 --help
```

### dmenu mode

`lm10 --dmenu` reads plain lines and prints the picked one as is, the way
dmenu does. Return with no match prints the query, and Shift+Return
always prints the query. Cancelling exits 1. dmenu's flags work
alongside lm10's (`-p`, `-l`, `-fn`, `-nb`, `-nf`, `-sb`, `-sf`, `-b`, `-m`,
`-v`). lm10 also turns on dmenu mode when it is run as `dmenu`, so it can
stand in for dmenu:

```sh
ln -s "$(command -v lm10)" ~/.local/bin/dmenu
ls | dmenu -p 'open: ' -l 10 -b
```

### configuration

see [examples/lm10.ini](examples/lm10.ini) for default configuration options.
all configuration options are overridable via cli flags.

#### home manager

the flake exports a home manager module as `homeModules.default`:

```nix
{
  imports = [ lm10.homeModules.default ];

  programs.lm10 = {
    enable = true;
    settings = {
      main = {
        prompt = "❯ ";
        width = "40%";
        match = "fuzzy";
        show-counter = false;
      };
      colors.background = "#141415f2";
      keys."Control+j" = "match-next";
    };
  };
}
```

### examples

see [examples](./examples/) for more usage examples

```sh
# Session menu: lock, log out, suspend and so on.
exec "${LM10:-lm10}" --width 40% <<'ENTRIES'
-n Lock -X loginctl lock-session
-n 'Log out' -X sh -c 'loginctl terminate-session "$XDG_SESSION_ID"'
-n Suspend -X systemctl suspend
-n Hibernate -X systemctl hibernate
-n Reboot -X systemctl reboot
-n 'Reboot into firmware setup' -X systemctl reboot --firmware-setup
-n 'Power off' -X systemctl poweroff
ENTRIES
```

## building

```sh
nix develop
meson setup bld/debug
ninja -C bld/debug
meson test -C bld/debug

# OR
nix build
```

## license

MIT; see [LICENSE](LICENSE). The libraries in `third-party/` keep their own
licenses.
