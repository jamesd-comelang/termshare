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

#include "terminal-share-server.h"

#include "terminal-screen.h"
#include "terminal-share-protocol.h"
#include "terminal-window.h"

#include <string.h>
#include <unistd.h>

#include <glib/gstdio.h>



struct _TerminalShareServer
{
  GObject parent_instance;

  gboolean sharing;
  gboolean client_connected;

  GSocketService *service;
  gchar *socket_path;

  /* One client at a time. */
  GSocketConnection *client_conn;
  TerminalShareFrameReader *reader;
  TerminalShareFrameWriter *writer;
  gboolean client_active;

  /* Attached window/session. Weak pointers: they become NULL if the
   * window or screen is finalised, and the destroy handlers below
   * turn that into a BYE while the connection is still usable. */
  TerminalWindow *attached_window;
  TerminalScreen *attached_screen;
  gulong contents_handler;
  gulong switch_handler;
  gulong page_added_handler;
  gulong page_removed_handler;
  gulong title_handler;
  gulong attached_window_destroy_handler;

  /* Last snapshot sent, for row diffing. */
  gchar **last_lines;
  glong last_cols;
  glong last_rows;
  glong last_cursor_col;
  glong last_cursor_row;
  gboolean has_snapshot;
  guint snapshot_timeout_id;

  /* Size bookkeeping for the attached session (see doc/termshare.md):
   * attach_max is the ceiling the remote may never exceed, raised
   * only by a local resize; pre_attach is restored on disconnect
   * unless the host user resized locally in the meantime. */
  gboolean pending_remote_resize;
  gboolean local_resized;
  glong attach_max_cols;
  glong attach_max_rows;
  glong pre_attach_cols;
  glong pre_attach_rows;

  GHashTable *window_ids; /* GtkWindow* -> window id (gint) */
  guint next_window_id;
};

G_DEFINE_TYPE (TerminalShareServer, terminal_share_server, G_TYPE_OBJECT)

enum
{
  PROP_0,
  PROP_SHARING,
  PROP_CLIENT_CONNECTED,
  N_PROPERTIES
};

static GParamSpec *obj_properties[N_PROPERTIES];

static TerminalShareServer *singleton = NULL;

static void share_server_detach_session (TerminalShareServer *server,
                                         gboolean restore_size);
static void share_server_client_close (TerminalShareServer *server);
static void share_server_send_sync (TerminalShareServer *server);
static void share_server_schedule_snapshot (TerminalShareServer *server);
static void share_server_set_client_connected (TerminalShareServer *server,
                                               gboolean connected);



/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static gchar *
sanitize_title (const gchar *title)
{
  gchar *copy;
  if (title == NULL)
    return g_strdup ("");
  copy = g_strdup (title);
  g_strdelimit (copy, "\n\r\t", ' ');
  return copy;
}

static void
share_server_set_sharing (TerminalShareServer *server,
                          gboolean sharing)
{
  if (server->sharing == sharing)
    return;
  server->sharing = sharing;
  g_object_notify_by_pspec (G_OBJECT (server),
                            obj_properties[PROP_SHARING]);
}

static void
share_server_set_client_connected (TerminalShareServer *server,
                                   gboolean connected)
{
  if (server->client_connected == connected)
    return;
  server->client_connected = connected;
  g_object_notify_by_pspec (G_OBJECT (server),
                            obj_properties[PROP_CLIENT_CONNECTED]);
}

static GtkNotebook *
share_window_notebook (TerminalWindow *window)
{
  GtkWidget *notebook = terminal_window_get_notebook (window);
  return GTK_NOTEBOOK (notebook);
}

static void
share_window_destroy_cb (GtkWidget *window,
                         gpointer user_data)
{
  TerminalShareServer *server = user_data;
  if (server->window_ids != NULL)
    g_hash_table_remove (server->window_ids, window);
}

static guint
share_server_register_window (TerminalShareServer *server,
                              TerminalWindow *window)
{
  gpointer id_ptr;
  guint id;

  if (g_hash_table_lookup_extended (server->window_ids, window, NULL,
                                    &id_ptr))
    return GPOINTER_TO_UINT (id_ptr);

  id = ++server->next_window_id;
  g_hash_table_insert (server->window_ids, window,
                       GUINT_TO_POINTER (id));
  g_signal_connect (window, "destroy",
                    G_CALLBACK (share_window_destroy_cb), server);
  return id;
}

static TerminalWindow *
share_server_find_window (TerminalShareServer *server,
                          guint id)
{
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init (&iter, server->window_ids);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      if (GPOINTER_TO_UINT (value) == id)
        return TERMINAL_WINDOW (key);
    }
  return NULL;
}

static GString *
share_server_build_tabbar (TerminalShareServer *server,
                           TerminalWindow *window)
{
  GtkNotebook *notebook;
  gint n_pages, i, active_index = -1;
  GString *s = g_string_new ("TABBAR ");

  if (window == NULL)
    {
      g_string_append (s, "-1\n");
      return s;
    }

  notebook = share_window_notebook (window);
  n_pages = gtk_notebook_get_n_pages (notebook);
  for (i = 0; i < n_pages; i++)
    {
      GtkWidget *page = gtk_notebook_get_nth_page (notebook, i);
      TerminalScreen *active_screen =
          (window == server->attached_window
           && server->attached_screen != NULL)
              ? server->attached_screen
              : terminal_window_get_active (window);
      if (TERMINAL_IS_SCREEN (page)
          && TERMINAL_SCREEN (page) == active_screen)
        active_index = i;
    }
  g_string_append_printf (s, "%d", active_index);
  for (i = 0; i < n_pages; i++)
    {
      GtkWidget *page = gtk_notebook_get_nth_page (notebook, i);
      gchar *title;
      gchar *clean;
      if (!TERMINAL_IS_SCREEN (page))
        continue;
      title = terminal_screen_get_title (TERMINAL_SCREEN (page));
      clean = sanitize_title (title);
      g_string_append_printf (s, "\t%s", clean);
      g_free (clean);
      g_free (title);
    }
  g_string_append_c (s, '\n');
  return s;
}

static void
share_server_send_tabbar (TerminalShareServer *server)
{
  GString *s;
  if (!server->client_active || server->attached_window == NULL)
    return;
  s = share_server_build_tabbar (server, server->attached_window);
  terminal_share_frame_writer_queue_text (server->writer, s->str);
  g_string_free (s, TRUE);
}



/* ------------------------------------------------------------------ */
/* Snapshot / diff                                                     */
/* ------------------------------------------------------------------ */

static void
share_server_send_sync_with (TerminalShareServer *server,
                             glong cols,
                             glong rows,
                             glong cursor_col,
                             glong cursor_row,
                             gchar **lines)
{
  GString *s;
  GString *tabbar;
  glong i;

  tabbar = share_server_build_tabbar (server, server->attached_window);
  s = g_string_new ("");
  g_string_append_printf (s, "SYNC %ld %ld %ld %ld\n",
                          cols, rows, cursor_col, cursor_row);
  g_string_append (s, tabbar->str);
  for (i = 0; i < rows; i++)
    g_string_append_printf (s, "%s\n", lines[i]);
  terminal_share_frame_writer_queue_text (server->writer, s->str);
  g_string_free (s, TRUE);
  g_string_free (tabbar, TRUE);

  terminal_screen_share_snapshot_free (server->last_lines,
                                       server->last_rows);
  server->last_lines = lines;
  server->last_cols = cols;
  server->last_rows = rows;
  server->last_cursor_col = cursor_col;
  server->last_cursor_row = cursor_row;
  server->has_snapshot = TRUE;
}

static void
share_server_send_sync (TerminalShareServer *server)
{
  TerminalScreen *screen = server->attached_screen;
  glong cols, rows, cursor_col, cursor_row;
  gchar **lines = NULL;

  if (!server->client_active || screen == NULL)
    return;
  if (!terminal_screen_get_share_snapshot_coloured (screen, &cols, &rows,
                                           &cursor_col, &cursor_row,
                                           &lines))
    return;
  share_server_send_sync_with (server, cols, rows, cursor_col,
                               cursor_row, lines);
}

static gboolean
share_server_snapshot_timeout_cb (gpointer user_data)
{
  TerminalShareServer *server = user_data;
  TerminalScreen *screen = server->attached_screen;
  glong cols, rows, cursor_col, cursor_row;
  gchar **lines = NULL;
  glong i;

  server->snapshot_timeout_id = 0;
  if (!server->client_active || screen == NULL)
    return FALSE;
  if (!terminal_screen_get_share_snapshot_coloured (screen, &cols, &rows,
                                           &cursor_col, &cursor_row,
                                           &lines))
    return FALSE;

  if (!server->has_snapshot)
    {
      share_server_send_sync_with (server, cols, rows, cursor_col,
                                   cursor_row, lines);
      return FALSE;
    }

  if (cols != server->last_cols || rows != server->last_rows)
    {
      if (server->pending_remote_resize)
        {
          server->pending_remote_resize = FALSE;
        }
      else
        {
          /* A local resize: the host user owns the size, so the
           * remote ceiling moves with it and no restore happens on
           * disconnect. */
          server->local_resized = TRUE;
          server->attach_max_cols = cols;
          server->attach_max_rows = rows;
        }
      {
        gchar *size_text = g_strdup_printf ("SIZE %ld %ld\n", cols, rows);
        terminal_share_frame_writer_queue_text (server->writer, size_text);
        g_free (size_text);
      }
      share_server_send_sync_with (server, cols, rows, cursor_col,
                                   cursor_row, lines);
      return FALSE;
    }

  /* Same size: diff rows, send only what changed, plus cursor. */
  {
    GString *s = g_string_new ("");
    gboolean any_row = FALSE;
    g_string_append_printf (s, "UPDATE %ld %ld\n",
                            cursor_col, cursor_row);
    for (i = 0; i < rows; i++)
      {
        if (g_strcmp0 (lines[i], server->last_lines[i]) != 0)
          {
            g_string_append_printf (s, "ROW %ld\t%s\n", i, lines[i]);
            any_row = TRUE;
          }
      }
    if (any_row || cursor_col != server->last_cursor_col
        || cursor_row != server->last_cursor_row)
      terminal_share_frame_writer_queue_text (server->writer, s->str);
    g_string_free (s, TRUE);
  }

  terminal_screen_share_snapshot_free (server->last_lines,
                                       server->last_rows);
  server->last_lines = lines;
  server->last_cols = cols;
  server->last_rows = rows;
  server->last_cursor_col = cursor_col;
  server->last_cursor_row = cursor_row;
  return FALSE;
}

static void
share_server_schedule_snapshot (TerminalShareServer *server)
{
  if (server->snapshot_timeout_id != 0)
    return;
  server->snapshot_timeout_id =
      g_timeout_add (20, share_server_snapshot_timeout_cb, server);
}



/* ------------------------------------------------------------------ */
/* Attached session signal handlers                                    */
/* ------------------------------------------------------------------ */

static void
share_contents_changed_cb (VteTerminal *terminal,
                           gpointer user_data)
{
  TerminalShareServer *server = user_data;
  (void) terminal;
  share_server_schedule_snapshot (server);
}

static void
share_title_changed_cb (TerminalScreen *screen,
                        GParamSpec *pspec,
                        gpointer user_data)
{
  TerminalShareServer *server = user_data;
  (void) screen;
  (void) pspec;
  share_server_send_tabbar (server);
}

static void
share_server_connect_screen (TerminalShareServer *server,
                             TerminalScreen *screen)
{
  VteTerminal *terminal = terminal_screen_get_terminal (screen);
  if (terminal != NULL)
    server->contents_handler =
        g_signal_connect (terminal, "contents-changed",
                          G_CALLBACK (share_contents_changed_cb), server);
  server->title_handler =
      g_signal_connect (screen, "notify::title",
                        G_CALLBACK (share_title_changed_cb), server);
}

static void
share_server_disconnect_screen (TerminalShareServer *server)
{
  TerminalScreen *screen = server->attached_screen;
  VteTerminal *terminal;
  if (screen == NULL)
    return;
  terminal = terminal_screen_get_terminal (screen);
  if (terminal != NULL && server->contents_handler != 0)
    g_signal_handler_disconnect (terminal, server->contents_handler);
  if (server->title_handler != 0)
    g_signal_handler_disconnect (screen, server->title_handler);
  server->contents_handler = 0;
  server->title_handler = 0;
}

static void
share_server_switch_to_screen (TerminalShareServer *server,
                               TerminalScreen *screen)
{
  if (server->attached_screen == screen)
    return;
  share_server_disconnect_screen (server);
  if (server->attached_screen != NULL)
    g_object_remove_weak_pointer (G_OBJECT (server->attached_screen),
                                  (gpointer *) &server->attached_screen);
  server->attached_screen = screen;
  if (screen != NULL)
    {
      g_object_add_weak_pointer (G_OBJECT (screen),
                                 (gpointer *) &server->attached_screen);
      share_server_connect_screen (server, screen);
    }
  server->has_snapshot = FALSE;
  if (server->snapshot_timeout_id != 0)
    {
      g_source_remove (server->snapshot_timeout_id);
      server->snapshot_timeout_id = 0;
    }
  terminal_screen_share_snapshot_free (server->last_lines,
                                       server->last_rows);
  server->last_lines = NULL;
}

static void
share_switch_page_cb (GtkNotebook *notebook,
                      GtkWidget *page,
                      guint page_num,
                      gpointer user_data)
{
  TerminalShareServer *server = user_data;
  (void) notebook;
  (void) page_num;
  if (!TERMINAL_IS_SCREEN (page))
    return;
  share_server_switch_to_screen (server, TERMINAL_SCREEN (page));
  share_server_send_tabbar (server);
  share_server_schedule_snapshot (server);
}

static void
share_page_changed_cb (GtkNotebook *notebook,
                       GtkWidget *page,
                       guint page_num,
                       gpointer user_data)
{
  TerminalShareServer *server = user_data;
  (void) notebook;
  (void) page;
  (void) page_num;
  /* A tab was added or removed: the tab bar changed, and if the
   * attached tab itself was removed, follow the new active tab. */
  if (server->attached_window != NULL)
    {
      TerminalScreen *active =
          terminal_window_get_active (server->attached_window);
      if (active != NULL && active != server->attached_screen)
        share_server_switch_to_screen (server, active);
      else if (active == NULL)
        {
          share_server_client_close (server);
          return;
        }
    }
  share_server_send_tabbar (server);
  share_server_schedule_snapshot (server);
}

static void
share_attached_window_destroy_cb (GtkWidget *window,
                                  gpointer user_data)
{
  TerminalShareServer *server = user_data;
  (void) window;
  /* The window is going away: tell the client, then clear state
   * without touching the dying window's notebook or handlers (they
   * die with it) and without trying to restore its size. */
  if (server->client_active)
    {
      GBytes *frame = terminal_share_encode_frame ("BYE Window closed\n",
                                                   strlen ("BYE Window closed\n"));
      gsize size;
      gconstpointer data = g_bytes_get_data (frame, &size);
      GOutputStream *out =
          g_io_stream_get_output_stream (G_IO_STREAM (server->client_conn));
      g_output_stream_write_all (out, data, size, NULL, NULL, NULL);
      g_bytes_unref (frame);
    }
  if (server->snapshot_timeout_id != 0)
    {
      g_source_remove (server->snapshot_timeout_id);
      server->snapshot_timeout_id = 0;
    }
  terminal_screen_share_snapshot_free (server->last_lines,
                                       server->last_rows);
  server->last_lines = NULL;
  server->has_snapshot = FALSE;
  /* Remove the weak pointers first: otherwise the dying window's
   * later finalise would overwrite a newer attachment in this field. */
  if (server->attached_window != NULL)
    g_object_remove_weak_pointer (G_OBJECT (server->attached_window),
                                  (gpointer *) &server->attached_window);
  if (server->attached_screen != NULL)
    g_object_remove_weak_pointer (G_OBJECT (server->attached_screen),
                                  (gpointer *) &server->attached_screen);
  server->attached_window = NULL;
  server->attached_screen = NULL;
  server->switch_handler = 0;
  server->page_added_handler = 0;
  server->page_removed_handler = 0;
  server->contents_handler = 0;
  server->title_handler = 0;
  server->attached_window_destroy_handler = 0;
  share_server_client_close (server);
}

/* ------------------------------------------------------------------ */
/* Client lifecycle                                                    */
/* ------------------------------------------------------------------ */

static void
share_server_detach_session (TerminalShareServer *server,
                             gboolean restore_size)
{
  TerminalScreen *screen = server->attached_screen;

  if (server->snapshot_timeout_id != 0)
    {
      g_source_remove (server->snapshot_timeout_id);
      server->snapshot_timeout_id = 0;
    }
  if (server->attached_window != NULL)
    {
      GtkNotebook *notebook = share_window_notebook (server->attached_window);
      if (server->attached_window_destroy_handler != 0)
        g_signal_handler_disconnect (server->attached_window,
                                     server->attached_window_destroy_handler);
      server->attached_window_destroy_handler = 0;
      if (server->switch_handler != 0)
        g_signal_handler_disconnect (notebook, server->switch_handler);
      if (server->page_added_handler != 0)
        g_signal_handler_disconnect (notebook, server->page_added_handler);
      if (server->page_removed_handler != 0)
        g_signal_handler_disconnect (notebook, server->page_removed_handler);
      server->switch_handler = 0;
      server->page_added_handler = 0;
      server->page_removed_handler = 0;
      g_object_remove_weak_pointer (G_OBJECT (server->attached_window),
                                    (gpointer *) &server->attached_window);
      server->attached_window = NULL;
    }
  share_server_disconnect_screen (server);
  if (server->attached_screen != NULL)
    {
      g_object_remove_weak_pointer (G_OBJECT (server->attached_screen),
                                    (gpointer *) &server->attached_screen);
      server->attached_screen = NULL;
    }

  if (restore_size && screen != NULL && !server->local_resized)
    {
      glong cols, rows;
      terminal_screen_get_size (screen, &cols, &rows);
      if (cols != server->pre_attach_cols
          || rows != server->pre_attach_rows)
        terminal_screen_set_size (screen, server->pre_attach_cols,
                                  server->pre_attach_rows);
    }

  terminal_screen_share_snapshot_free (server->last_lines,
                                       server->last_rows);
  server->last_lines = NULL;
  server->has_snapshot = FALSE;
  server->pending_remote_resize = FALSE;
  server->local_resized = FALSE;
}

static void
share_server_client_close (TerminalShareServer *server)
{
  gboolean was_active = server->client_active;

  if (server->reader != NULL)
    {
      terminal_share_frame_reader_free (server->reader);
      server->reader = NULL;
    }
  if (server->writer != NULL)
    {
      terminal_share_frame_writer_free (server->writer);
      server->writer = NULL;
    }
  if (server->client_conn != NULL)
    {
      g_io_stream_close (G_IO_STREAM (server->client_conn), NULL, NULL);
      g_clear_object (&server->client_conn);
    }
  server->client_active = FALSE;
  if (was_active)
    share_server_set_client_connected (server, FALSE);
}

static void
share_server_client_frame_cb (const guint8 *data,
                              gsize len,
                              gpointer user_data)
{
  TerminalShareServer *server = user_data;
  gchar *text;

  if (len >= 6 && memcmp (data, "INPUT ", 6) == 0)
    {
      if (server->attached_screen != NULL && len > 6)
        terminal_screen_feed_data (server->attached_screen,
                                   (const gchar *) data + 6, len - 6);
      return;
    }

  /* Text commands: copy to a NUL-terminated buffer for parsing. */
  text = g_strndup ((const gchar *) data, len);

  if (g_str_has_prefix (text, "LIST"))
    {
      GList *windows = gtk_window_list_toplevels ();
      GList *lp;
      GString *s = g_string_new ("LIST\n");
      for (lp = windows; lp != NULL; lp = lp->next)
        {
          GtkWindow *win = GTK_WINDOW (lp->data);
          TerminalWindow *window;
          GtkNotebook *notebook;
          gint n_pages, i;
          guint id;
          gchar *win_title;
          gchar *clean;
          if (!TERMINAL_IS_WINDOW (win))
            continue;
          window = TERMINAL_WINDOW (win);
          id = share_server_register_window (server, window);
          win_title = sanitize_title (gtk_window_get_title (win));
          clean = win_title;
          g_string_append_printf (s, "WINDOW %u\t%s\n", id, clean);
          g_free (clean);
          notebook = share_window_notebook (window);
          n_pages = gtk_notebook_get_n_pages (notebook);
          for (i = 0; i < n_pages; i++)
            {
              GtkWidget *page = gtk_notebook_get_nth_page (notebook, i);
              gchar *title, *tclean;
              if (!TERMINAL_IS_SCREEN (page))
                continue;
              title = terminal_screen_get_title (TERMINAL_SCREEN (page));
              tclean = sanitize_title (title);
              g_string_append_printf (
                  s, "TAB %u\t%u\t%d\t%s\n", id,
                  terminal_screen_get_session_id (TERMINAL_SCREEN (page)),
                  (TERMINAL_SCREEN (page)
                       == terminal_window_get_active (window)) ? 1 : 0,
                  tclean);
              g_free (tclean);
              g_free (title);
            }
        }
      g_list_free (windows);
      g_string_append (s, "END\n");
      terminal_share_frame_writer_queue_text (server->writer, s->str);
      g_string_free (s, TRUE);
    }
  else if (g_str_has_prefix (text, "ATTACH "))
    {
      guint id = 0;
      TerminalWindow *window;
      TerminalScreen *screen;
      GtkNotebook *notebook;
      if (sscanf (text, "ATTACH %u", &id) != 1
          || (window = share_server_find_window (server, id)) == NULL)
        {
          terminal_share_frame_writer_queue_text (server->writer,
                                                  "ERROR No such window\n");
          g_free (text);
          return;
        }
      screen = terminal_window_get_active (window);
      if (screen == NULL)
        {
          terminal_share_frame_writer_queue_text (server->writer,
                                                  "ERROR Window has no tabs\n");
          g_free (text);
          return;
        }
      if (server->attached_window != NULL)
        share_server_detach_session (server, TRUE);
      server->attached_window = window;
      g_object_add_weak_pointer (G_OBJECT (window),
                                 (gpointer *) &server->attached_window);
      server->attached_window_destroy_handler =
          g_signal_connect (window, "destroy",
                            G_CALLBACK (share_attached_window_destroy_cb),
                            server);
      notebook = share_window_notebook (window);
      server->switch_handler =
          g_signal_connect (notebook, "switch-page",
                            G_CALLBACK (share_switch_page_cb), server);
      server->page_added_handler =
          g_signal_connect (notebook, "page-added",
                            G_CALLBACK (share_page_changed_cb), server);
      server->page_removed_handler =
          g_signal_connect (notebook, "page-removed",
                            G_CALLBACK (share_page_changed_cb), server);
      server->attached_screen = screen;
      g_object_add_weak_pointer (G_OBJECT (screen),
                                 (gpointer *) &server->attached_screen);
      share_server_connect_screen (server, screen);
      terminal_screen_get_size (screen, &server->pre_attach_cols,
                                &server->pre_attach_rows);
      server->attach_max_cols = server->pre_attach_cols;
      server->attach_max_rows = server->pre_attach_rows;
      server->local_resized = FALSE;
      server->pending_remote_resize = FALSE;
      share_server_set_client_connected (server, TRUE);
      share_server_send_sync (server);
    }
  else if (g_str_has_prefix (text, "RESIZE "))
    {
      glong client_cols = 0, client_rows = 0;
      glong grid_cols, grid_rows, cur_cols, cur_rows;
      if (sscanf (text, "RESIZE %ld %ld", &client_cols,
                  &client_rows) == 2
          && server->attached_screen != NULL)
        {
          if (client_rows <= TERMSHARE_CHROME_ROWS || client_cols < 1)
            {
              terminal_share_frame_writer_queue_text (
                  server->writer, "ERROR Remote terminal too small\n");
            }
          else
            {
              grid_cols = MIN (server->attach_max_cols, client_cols);
              grid_rows = MIN (server->attach_max_rows,
                               client_rows - TERMSHARE_CHROME_ROWS);
              terminal_screen_get_size (server->attached_screen,
                                        &cur_cols, &cur_rows);
              if (grid_cols != cur_cols || grid_rows != cur_rows)
                {
                  server->pending_remote_resize = TRUE;
                  terminal_screen_set_size (server->attached_screen,
                                            grid_cols, grid_rows);
                  share_server_schedule_snapshot (server);
                }
            }
        }
    }
  else if (g_str_has_prefix (text, "DETACH"))
    {
      share_server_detach_session (server, TRUE);
      share_server_client_close (server);
    }
  else
    {
      terminal_share_frame_writer_queue_text (server->writer,
                                              "ERROR Unknown command\n");
    }

  g_free (text);
}

static void
share_server_client_end_cb (gpointer user_data)
{
  TerminalShareServer *server = user_data;
  /* EOF or read error: the client is gone. This is the one
   * disconnect path that must never touch the host sessions. */
  share_server_detach_session (server, TRUE);
  share_server_client_close (server);
}

static gboolean
share_server_incoming_cb (GSocketService *service,
                          GSocketConnection *connection,
                          GObject *source_object,
                          gpointer user_data)
{
  TerminalShareServer *server = user_data;
  GSocket *sock;
  GCredentials *creds = NULL;
  GError *error = NULL;
  uid_t peer_uid = (uid_t) -1;
  (void) service;
  (void) source_object;

  if (server->client_active)
    {
      /* Single client only: a small synchronous BUSY write, then let
       * the service close this connection (return FALSE). */
      GBytes *frame = terminal_share_encode_frame ("BUSY\n", 5);
      gsize size;
      gconstpointer data = g_bytes_get_data (frame, &size);
      GOutputStream *out =
          g_io_stream_get_output_stream (G_IO_STREAM (connection));
      g_output_stream_write_all (out, data, size, NULL, NULL, NULL);
      g_bytes_unref (frame);
      return FALSE;
    }

  /* Defence in depth on top of the 0700 directory and 0600 socket:
   * only the current user may connect, whatever the permissions. */
  sock = g_socket_connection_get_socket (connection);
  creds = g_socket_get_credentials (sock, &error);
  if (creds == NULL)
    {
      g_clear_error (&error);
      return FALSE;
    }
  peer_uid = g_credentials_get_unix_user (creds, &error);
  g_object_unref (creds);
  if (error != NULL || peer_uid != getuid ())
    {
      g_clear_error (&error);
      return FALSE;
    }

  server->client_conn = g_object_ref (connection);
  server->client_active = TRUE;
  server->writer = terminal_share_frame_writer_new (
      g_io_stream_get_output_stream (G_IO_STREAM (connection)));
  terminal_share_frame_writer_queue_text (server->writer, "WELCOME 1\n");
  server->reader = terminal_share_frame_reader_new (
      g_io_stream_get_input_stream (G_IO_STREAM (connection)),
      share_server_client_frame_cb, share_server_client_end_cb, server);
  terminal_share_frame_reader_start (server->reader);
  return TRUE;
}



/* ------------------------------------------------------------------ */
/* Start / stop                                                        */
/* ------------------------------------------------------------------ */

gboolean
terminal_share_server_start (TerminalShareServer *server,
                             GError **error)
{
  gchar *dir;
  GSocketAddress *address;
  gboolean ok;

  g_return_val_if_fail (TERMINAL_IS_SHARE_SERVER (server), FALSE);
  if (server->sharing)
    return TRUE;

  dir = g_build_filename (g_get_user_runtime_dir (), "termshare",
                          NULL);
  if (g_mkdir_with_parents (dir, 0700) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Cannot create %s", dir);
      g_free (dir);
      return FALSE;
    }
  /* Tighten an existing directory too; only this user may traverse
   * it, which is what makes the socket private. */
  g_chmod (dir, 0700);
  g_free (server->socket_path);
  server->socket_path =
      g_build_filename (g_get_user_runtime_dir (), "termshare",
                        "share.sock", NULL);

  if (g_file_test (server->socket_path, G_FILE_TEST_EXISTS))
    {
      /* Never unlink a live server's socket: probe it first. */
      GSocketClient *probe = g_socket_client_new ();
      GSocketAddress *probe_addr =
          g_unix_socket_address_new (server->socket_path);
      GSocketConnection *probe_conn =
          g_socket_client_connect (probe,
                                   G_SOCKET_CONNECTABLE (probe_addr),
                                   NULL, NULL);
      g_object_unref (probe_addr);
      g_object_unref (probe);
      if (probe_conn != NULL)
        {
          g_io_stream_close (G_IO_STREAM (probe_conn), NULL, NULL);
          g_object_unref (probe_conn);
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_ADDRESS_IN_USE,
                       "A sharing server is already listening on %s",
                       server->socket_path);
          return FALSE;
        }
      g_unlink (server->socket_path);
    }

  server->service = g_socket_service_new ();
  address = g_unix_socket_address_new (server->socket_path);
  ok = g_socket_listener_add_address (G_SOCKET_LISTENER (server->service),
                                      address, G_SOCKET_TYPE_STREAM,
                                      G_SOCKET_PROTOCOL_DEFAULT,
                                      NULL, NULL, error);
  g_object_unref (address);
  if (!ok)
    {
      g_clear_object (&server->service);
      return FALSE;
    }
  g_chmod (server->socket_path, 0600);
  g_signal_connect (server->service, "incoming",
                    G_CALLBACK (share_server_incoming_cb), server);
  g_socket_service_start (server->service);
  share_server_set_sharing (server, TRUE);
  return TRUE;
}

void
terminal_share_server_stop (TerminalShareServer *server)
{
  g_return_if_fail (TERMINAL_IS_SHARE_SERVER (server));
  if (!server->sharing)
    return;

  if (server->client_active)
    {
      GBytes *frame = terminal_share_encode_frame ("BYE Sharing stopped\n",
                                                   strlen ("BYE Sharing stopped\n"));
      gsize size;
      gconstpointer data = g_bytes_get_data (frame, &size);
      GOutputStream *out =
          g_io_stream_get_output_stream (G_IO_STREAM (server->client_conn));
      g_output_stream_write_all (out, data, size, NULL, NULL, NULL);
      g_bytes_unref (frame);
      share_server_detach_session (server, TRUE);
      share_server_client_close (server);
    }
  else
    {
      share_server_detach_session (server, TRUE);
    }

  if (server->service != NULL)
    {
      g_socket_service_stop (server->service);
      g_clear_object (&server->service);
    }
  if (server->socket_path != NULL)
    {
      g_unlink (server->socket_path);
      g_free (server->socket_path);
      server->socket_path = NULL;
    }
  share_server_set_sharing (server, FALSE);
}



/* ------------------------------------------------------------------ */
/* GObject                                                             */
/* ------------------------------------------------------------------ */

static void
terminal_share_server_get_property (GObject *object,
                                    guint prop_id,
                                    GValue *value,
                                    GParamSpec *pspec)
{
  TerminalShareServer *server = TERMINAL_SHARE_SERVER (object);
  switch (prop_id)
    {
    case PROP_SHARING:
      g_value_set_boolean (value, server->sharing);
      break;
    case PROP_CLIENT_CONNECTED:
      g_value_set_boolean (value, server->client_connected);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
terminal_share_server_finalize (GObject *object)
{
  TerminalShareServer *server = TERMINAL_SHARE_SERVER (object);
  if (server->sharing)
    terminal_share_server_stop (server);
  if (server->window_ids != NULL)
    g_hash_table_destroy (server->window_ids);
  g_free (server->socket_path);
  terminal_screen_share_snapshot_free (server->last_lines,
                                       server->last_rows);
  G_OBJECT_CLASS (terminal_share_server_parent_class)->finalize (object);
}

static void
terminal_share_server_class_init (TerminalShareServerClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->get_property = terminal_share_server_get_property;
  gobject_class->finalize = terminal_share_server_finalize;

  obj_properties[PROP_SHARING] =
      g_param_spec_boolean ("sharing", "Sharing", "Sharing is on",
                            FALSE, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  obj_properties[PROP_CLIENT_CONNECTED] =
      g_param_spec_boolean ("client-connected", "Client connected",
                            "A remote client is attached",
                            FALSE, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties (gobject_class, N_PROPERTIES,
                                     obj_properties);
}

static void
terminal_share_server_init (TerminalShareServer *server)
{
  server->window_ids = g_hash_table_new (g_direct_hash, g_direct_equal);
  server->next_window_id = 0;
}

TerminalShareServer *
terminal_share_server_get (void)
{
  if (singleton == NULL)
    singleton = g_object_new (TERMINAL_TYPE_SHARE_SERVER, NULL);
  return singleton;
}

gboolean
terminal_share_server_is_sharing (TerminalShareServer *server)
{
  g_return_val_if_fail (TERMINAL_IS_SHARE_SERVER (server), FALSE);
  return server->sharing;
}

gboolean
terminal_share_server_has_client (TerminalShareServer *server)
{
  g_return_val_if_fail (TERMINAL_IS_SHARE_SERVER (server), FALSE);
  return server->client_connected;
}

const gchar *
terminal_share_server_get_socket_path (TerminalShareServer *server)
{
  g_return_val_if_fail (TERMINAL_IS_SHARE_SERVER (server), NULL);
  return server->socket_path;
}
