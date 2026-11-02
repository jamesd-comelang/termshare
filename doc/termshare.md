# termshare: remote terminal session sharing (prototype)

Share a running termshare window inside another ordinary terminal
emulator, using only character cells and ANSI sequences. No new
process is spawned for the shared session, no shell is restarted, and
no PTY is touched: the server lives inside the termshare process,
reads VTE's visible grid, and feeds input bytes back through VTE.

## Host: turning sharing on and off

Sharing is off by default, and off again on every launch (it is not
persisted). In the host window, Terminal menu -> **Share Terminal**
toggles it for the whole termshare process.

- Off: no socket exists, and a client gets a clear "sharing is off"
  message instead of a connection error.
- On, no client: a 2px line appears directly above the tab bar,
  yellow (`#ffc107`).
- Client connected: the same line turns green (`#00c853`).
- Off again: the line hides, any attached client is sent `BYE
  Sharing stopped`, the socket file is removed, and the host's shells
  are untouched.

The socket is `$XDG_RUNTIME_DIR/termshare/share.sock`. The
directory is created (or tightened) to 0700 and the socket to 0600,
and the server also checks the peer's UID against its own, so only
the current user can connect even if the permissions were widened.
A stale socket from a crashed host is removed only after a connect
probe fails; a live server's socket is never unlinked.

## Client

```
termshare-client
termshare-client list
termshare-client attach [window-id]
```

`list` prints one line per window, as its tab titles joined with
` | `, with `*` on the active tab (for example, `1  vim: foo.c* |
shell`). The window title itself is not shown, except as a fallback
for a window with no tabs. With no arguments, or with `attach` and
no window id, the client prints the same numbered list, waits for
the user to pick a number, and attaches to that window. A bad number
or EOF exits without attaching. `attach <window-id>` still takes an
internal window id directly. The client renders the whole window:

```
 File  Edit  View  Terminal  Tabs  Help      (dim, display-only)
 ─────────────────────────────────────          (green while attached)
 tab1 | tab2* | tab3
 $ shared grid...
```

The menu is display-only: the remote has no mouse or menu
interaction and cannot reach the share toggle. Sharing stays
host-controlled. The toolbar is omitted in this prototype (its icons
have no character-cell form). Ctrl+] detaches; everything else is
forwarded verbatim to the host tab, so arrows and Ctrl-C work as
locally. Detaching restores the caller's terminal via the alternate
screen.

## Size rule

The client needs 3 chrome rows (menu, indicator, tab bar) above the
grid. "Host maximum" is the host grid size when sharing is turned
on, raised only by the host user resizing locally, never by the
remote. On attach and on remote resize, per dimension:

- remote smaller: the host grid shrinks to fit
  (`min(host_max_cols, remote_cols)`,
  `min(host_max_rows, remote_rows - 3)`);
- remote bigger: that dimension keeps the host size, and the client
  leaves the extra space blank.

On disconnect, the host grid is restored to its pre-attach size,
unless the host user resized locally while the remote was attached,
in which case their resize wins and no restore happens. If the host
user enlarges the grid beyond the remote terminal while attached,
the client crops to its own size; that is a host-owner override, not
a remote resize.

## Protocol

See `terminal/terminal-share-protocol.h` for the full frame list.
In short: 4-byte big-endian length frames over the Unix socket.
Client sends `LIST`, `ATTACH <window-id>`, `INPUT <raw bytes>`,
`RESIZE <cols> <rows>` (total client size, chrome included) and
`DETACH`. The server sends `WELCOME`, `LIST`, `SYNC` (full grid +
tab bar), `UPDATE` (only changed rows + cursor), `SIZE`, `TABBAR`,
`BUSY`, `ERROR` and `BYE`. One client at a time: a second connection
gets `BUSY` and is closed. Reconnecting is a fresh `ATTACH` plus a
full `SYNC`; disconnecting (EOF, DETACH, or sharing off) never
signals the host PTY.

## What this prototype does not do yet

- **No colour, bold, or other attributes in the shared grid.** On
  VTE >= 0.72 the public text-attributes array is deprecated and
  returns NULL, so the public VTE API only yields plain text. The
  client's own chrome (dim menu, green indicator, reverse-video
  active tab) does use ANSI colours, but the grid itself is plain.
  Colour for existing tabs needs a different capture path (see the
  Phase 4 research item in the plan); it is not a matter of adding
  an SGR pass to this code.
- **Cursor row conversion is unverified on a real VTE build.** VTE
  reports an absolute cursor row, and the server converts it with
  the scrollable adjustment's value as the first visible row. That
  conversion, and the exact `get_text` behaviour for soft-wrapped
  lines and the alternate screen, must be checked against a real
  termshare build before this is called working.
- **Build status:** `termshare-client` plus the shared protocol code
  compile cleanly with `-Wall -Wextra` and were tested against a
  mock server (list, attach, SYNC, row UPDATE, RESIZE, Ctrl+]
  detach). The host side (`termshare` itself, GTK/VTE) could
  not be built in the development VM used for this prototype, which
  has no GTK/VTE development packages, so the server, screen
  snapshot, window indicator and toggle have been code-reviewed
  but not compiled or run end to end. That is the first thing to
  verify on a machine with the termshare build dependencies.
- Tab bars longer than the grid width, and grid rows containing
  very wide characters, may wrap in the remote terminal; both are
  accepted prototype limitations.
- Mouse forwarding, scrollback, and remote tab switching are not
  implemented. Host tab switches are followed (the remote shows
  the new active tab), but the remote cannot switch tabs itself.
