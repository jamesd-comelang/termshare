[![License](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](https://github.com/jamesd-comelang/termshare/blob/master/COPYING)
[![Build](https://github.com/jamesd-comelang/termshare/actions/workflows/build.yml/badge.svg)](https://github.com/jamesd-comelang/termshare/actions/workflows/build.yml)

# termshare

Termshare is a terminal emulator forked from Xfce Terminal, with one
addition: remote session sharing. It keeps the upstream feature set
— drop down, tabs, unlimited scrolling, full colors, fonts,
transparent backgrounds, and more — and is based on the Vte terminal
widget library, just like gnome-terminal.

### Remote session sharing (prototype)

Share a running termshare window inside another ordinary terminal
emulator, using only character cells and ANSI sequences. No new
process is spawned for the shared session, no shell is restarted,
and no PTY is touched: the server lives inside the termshare
process, reads VTE's visible grid, and feeds input bytes back
through VTE.

- Host: Terminal menu -> **Share Terminal**. Off by default, and
  off again on every launch. When on, a thin line appears above
  the tab bar, yellow with no client, green when a client connects.
- Client: `termshare-client` with no arguments lists the sessions
  numbered 1, 2, 3, ... — one line per window, tab titles joined
  with ` | `, `*` on the active tab — and waits for a pick.
  `termshare-client list` prints the same list;
  `termshare-client attach [window-id]` attaches directly.
  Ctrl+] detaches; everything else is forwarded verbatim.
- One client at a time. The socket is
  `$XDG_RUNTIME_DIR/termshare/share.sock`, directory 0700, socket
  0600, with a peer-UID check, so only the current user can connect.
- Size: a smaller remote shrinks the host grid to fit; a bigger
  remote keeps the host size. On disconnect the host grid is
  restored, unless the host user resized locally while attached.

Full details, the protocol, and the size rule are in
[doc/termshare.md](https://github.com/jamesd-comelang/termshare/blob/master/doc/termshare.md).

Prototype limits: the shared grid is plain text — on VTE >= 0.72
the public API yields no colours or attributes for an existing tab
— and there is no mouse forwarding, scrollback, or remote tab
switching. The host side builds in CI (see below) but a live attach
on a real terminal, including the cursor-row conversion, is still
to be verified end to end.

### Building

Dependencies: meson (>= 0.54), ninja, glib (>= 2.66),
GTK 3 (>= 3.24), VTE 2.91 (>= 0.51.3), libpcre2 (>= 10.00),
libxfce4ui, libxfce4kbd, libxfce4util and libxfconf (>= 4.18),
plus xsltproc and docbook-xsl for the man page.

From source code repository:

    % git clone https://github.com/jamesd-comelang/termshare.git
    % cd termshare
    % meson setup build
    % meson compile -C build
    % sudo meson install -C build

The binaries are `build/terminal/termshare` and
`build/terminal/termshare-client`. If xsltproc is missing, use
`meson setup build -Ddoc=false` instead.

The client alone needs only glib/gio, no GTK or VTE:

    % gcc -std=gnu11 terminal/terminal-share-client.c terminal/terminal-share-protocol.c \
        -o termshare-client $(pkg-config --cflags --libs glib-2.0 gio-2.0 gio-unix-2.0)

From release tarball:

    % tar xf termshare-<version>.tar.xz
    % cd termshare-<version>
    % meson setup build
    % meson compile -C build
    % meson install -C build

### Uninstallation

    % sudo ninja uninstall -C build

### From xfce4-terminal

This fork was rebranded from xfce4-terminal to termshare: the binary
is `termshare`, the icons are `org.termshare.terminal`, the D-Bus
name is `org.termshare.Terminal`, the xfconf channel is `termshare`,
and config lives under `termshare/terminal/`. Old xfce4-terminal
settings do not migrate automatically. The libxfce4ui, libxfce4util
and libxfce4session libraries it builds against are unchanged.

### Continuous integration

The GitHub Actions workflow in `.github/workflows/build.yml`
("Build") runs on push, pull request, and manual dispatch, and
builds both binaries with meson on ubuntu-latest.

### Changelog

See [NEWS](https://github.com/jamesd-comelang/termshare/blob/master/NEWS) for details on changes and fixes made in the current release.

### Source Code Repository

[Termshare source code](https://github.com/jamesd-comelang/termshare)

### Download a Release Tarball

[Termshare archive](https://github.com/jamesd-comelang/termshare)
    or
[Termshare tags](https://github.com/jamesd-comelang/termshare/tags)

### Reporting Bugs

Visit the [reporting bugs](https://github.com/jamesd-comelang/termshare/issues) page to view currently open bug reports and instructions on reporting new bugs or submitting bugfixes.
