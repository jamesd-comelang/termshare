/*-
 * Copyright (c) 2026 James Ding
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * termshare server, living inside the xfce4-terminal process.
 *
 * Sharing is off by default. terminal_share_server_start() creates
 * $XDG_RUNTIME_DIR/xfce4-terminal/share.sock (directory 0700, socket
 * 0600, peer UID checked) and starts listening; stop() disconnects
 * any client with a BYE, removes the socket and stops listening.
 * The host's shells and PTYs are never touched: the server only reads
 * VTE's visible grid and feeds input bytes back through VTE.
 *
 * One client at a time: a second connection gets BUSY and is closed.
 * The "sharing" and "client-connected" properties notify so that host
 * windows can update their indicator line (yellow = on, green =
 * client connected, hidden = off).
 */

#ifndef TERMINAL_SHARE_SERVER_H
#define TERMINAL_SHARE_SERVER_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define TERMINAL_TYPE_SHARE_SERVER (terminal_share_server_get_type ())
G_DECLARE_FINAL_TYPE (TerminalShareServer, terminal_share_server,
                      TERMINAL, SHARE_SERVER, GObject)

/* Process singleton; borrowed, lives for the process lifetime. */
TerminalShareServer *
terminal_share_server_get (void);

gboolean
terminal_share_server_start (TerminalShareServer *server,
                             GError **error);
void
terminal_share_server_stop (TerminalShareServer *server);

gboolean
terminal_share_server_is_sharing (TerminalShareServer *server);
gboolean
terminal_share_server_has_client (TerminalShareServer *server);

/* Socket path, or NULL when sharing is off. */
const gchar *
terminal_share_server_get_socket_path (TerminalShareServer *server);

G_END_DECLS

#endif /* !TERMINAL_SHARE_SERVER_H */
