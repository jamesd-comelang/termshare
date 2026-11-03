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
 * termshare wire protocol, version 1.
 *
 * Transport: one Unix domain socket, one client at a time.
 * Framing: every frame is a 4-byte big-endian length followed by that
 * many payload bytes. The payload is not NUL-terminated. Text messages
 * are lines separated by '\n'; fields inside a line are separated by
 * '\t' so that titles containing spaces survive intact.
 *
 * Client -> server:
 *   LIST\n
 *   ATTACH <window-id>\n
 *   INPUT <raw bytes>          (no trailing newline; the frame length
 *                               delimits the bytes, so NULs survive)
 *   RESIZE <cols> <rows>\n     (client terminal size, chrome included)
 *   DETACH\n
 *
 * Server -> client:
 *   WELCOME 1\n
 *   LIST\nWINDOW <id>\t<title>\nTAB <window-id>\t<tab-id>\t<active 0/1>\t<title>\nEND\n
 *   SYNC <cols> <rows> <cursor-col> <cursor-row>\n
 *   TABBAR <active-index> <close-buttons 0/1>\t<title>\t<title>...\n
 *     (close-buttons is the host's misc-tab-close-buttons
 *     preference, so the client shows " ×" only when the host
 *     shows a close button; old clients ignore it, since atoi()
 *     stops at the space and titles follow the tab separator)
 *   <grid row 0>\n ... <grid row rows-1>\n
 *     (cols/rows are the grid size; cursor-row is a visible row;
 *     the TABBAR line and exactly <rows> grid lines follow the SYNC
 *     line in the same frame)
 *   UPDATE <cursor-col> <cursor-row>\nROW <row>\t<text>\n...
 *     (zero or more ROW lines; no ROW lines means cursor-only move)
 *   SIZE <cols> <rows>\n       (grid size changed; a SYNC follows)
 *   TABBAR <active-index> <close-buttons 0/1>\t<title>...\n
 *   BUSY\n
 *   ERROR <message>\n
 *   BYE <reason>\n
 *
 * Rendering note: the client draws three chrome rows above the grid:
 * a menu row, an indicator row and a tab-bar row. That is why the
 * server converts a client's total size to a grid size by subtracting
 * TERMSHARE_CHROME_ROWS from the row count (see the server).
 */

#ifndef TERMINAL_SHARE_PROTOCOL_H
#define TERMINAL_SHARE_PROTOCOL_H

#include <gio/gio.h>
/* Explicit, not via gio.h: glib 2.72 on Ubuntu 22.04 does not include
 * gunixsocketaddress.h from gio.h (glib 2.80 on 24.04 does), so without
 * this the Jammy build fails with an implicit declaration of
 * g_unix_socket_address_new, used by both the client and the server. */
#include <gio/gunixsocketaddress.h>

G_BEGIN_DECLS

#define TERMSHARE_PROTOCOL_VERSION 1
#define TERMSHARE_CHROME_ROWS 3
#define TERMSHARE_MAX_FRAME (1024 * 1024)
#define TERMSHARE_MENU_TEXT " File  Edit  View  Terminal  Tabs  Help"

/* --- asynchronous frame reader -------------------------------------- */

/* Called for each complete frame. data is valid only for the call. */
typedef void (*TerminalShareFrameFunc) (const guint8 *data,
                                        gsize len,
                                        gpointer user_data);
/* Called once when the stream ends or errors, after which the reader
 * stops itself. */
typedef void (*TerminalShareFrameEndFunc) (gpointer user_data);

typedef struct _TerminalShareFrameReader TerminalShareFrameReader;

TerminalShareFrameReader *
terminal_share_frame_reader_new (GInputStream *stream,
                                 TerminalShareFrameFunc on_frame,
                                 TerminalShareFrameEndFunc on_end,
                                 gpointer user_data);
void
terminal_share_frame_reader_start (TerminalShareFrameReader *reader);
void
terminal_share_frame_reader_free (TerminalShareFrameReader *reader);

/* --- asynchronous frame writer (ordered queue) ---------------------- */

typedef struct _TerminalShareFrameWriter TerminalShareFrameWriter;

TerminalShareFrameWriter *
terminal_share_frame_writer_new (GOutputStream *stream);
void
terminal_share_frame_writer_queue (TerminalShareFrameWriter *writer,
                                   const gchar *data,
                                   gsize len);
void
terminal_share_frame_writer_queue_text (TerminalShareFrameWriter *writer,
                                        const gchar *text);
void
terminal_share_frame_writer_free (TerminalShareFrameWriter *writer);

/* Encode one frame (length header + payload) into a newly allocated
 * buffer, for callers that build a frame synchronously. */
GBytes *
terminal_share_encode_frame (const gchar *data,
                             gsize len);

G_END_DECLS

#endif /* !TERMINAL_SHARE_PROTOCOL_H */
