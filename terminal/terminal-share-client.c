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
 * termshare-client: render a shared termshare window inside any
 * ordinary terminal emulator, using only character cells and ANSI
 * sequences. No GTK, no VTE.
 *
 *   termshare-client
 *   termshare-client list
 *   termshare-client attach [window-id]
 *
 * With no arguments, or with "attach" and no window id, the client
 * prints the available sessions numbered 1, 2, 3, ... and waits for
 * the user to pick a number before attaching. In attach mode the client's own terminal is put in raw
 * mode and switched to the alternate screen, so detaching restores
 * the caller's terminal exactly. Ctrl+] detaches. Everything else
 * typed is forwarded verbatim to the host tab, so arrows, Ctrl-C and
 * so on behave as they do locally; the host menu is rendered dim
 * and is not interactive, and the remote cannot toggle sharing.
 *
 * Layout (three chrome rows above the grid):
 *   row 0: menu, dim
 *   row 1: indicator, green (this client is connected)
 *   row 2: tab bar, like the host's GtkNotebook strip: padded tabs
 *     separated by │, the active tab bold dark-on-light, inactive
 *     tabs light-on-dark, the strip filled to the full grid width.
 *     Display-only, like the menu: no mouse, no remote close.
 *   row 3+: the shared grid
 */

#include "terminal-share-protocol.h"

#include <glib-unix.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

typedef struct
{
  GMainLoop *loop;
  GSocketConnection *conn;
  TerminalShareFrameReader *reader;
  TerminalShareFrameWriter *writer;

  gboolean list_mode;
  gboolean attached;
  gboolean raw_mode;
  gboolean alt_screen;
  struct termios saved_termios;

  guint target_window_id;
  gboolean target_set;

  /* Last rendered state. */
  gchar **grid;
  glong grid_cols;
  glong grid_rows;
  glong cursor_col;
  glong cursor_row;
  gchar **tabs;
  gint n_tabs;
  gint active_tab;
  gboolean tabs_close_buttons;
  glong client_cols;
  glong client_rows;
  gchar *exit_message;
  int exit_code;
} Client;

static void client_quit (Client *c, const gchar *message, int code);
static void client_restore_terminal (Client *c);
static void client_render_full (Client *c);
static void client_send_resize (Client *c);



/* ------------------------------------------------------------------ */
/* Terminal size / raw mode                                            */
/* ------------------------------------------------------------------ */

static gboolean
client_get_size (Client *c)
{
  struct winsize ws;
  int fd = isatty (STDIN_FILENO) ? STDIN_FILENO : STDOUT_FILENO;
  if (ioctl (fd, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0 || ws.ws_row == 0)
    return FALSE;
  c->client_cols = ws.ws_col;
  c->client_rows = ws.ws_row;
  return TRUE;
}

static void
client_enter_raw (Client *c)
{
  struct termios raw;
  if (!isatty (STDIN_FILENO))
    return;
  if (tcgetattr (STDIN_FILENO, &c->saved_termios) != 0)
    return;
  raw = c->saved_termios;
  cfmakeraw (&raw);
  tcsetattr (STDIN_FILENO, TCSANOW, &raw);
  c->raw_mode = TRUE;
  /* Alternate screen + clear; exiting it restores the caller's
   * terminal content. */
  write (STDOUT_FILENO, "\e[?1049h\e[2J", 12);
  c->alt_screen = TRUE;
}

static void
client_restore_terminal (Client *c)
{
  if (c->alt_screen)
    {
      write (STDOUT_FILENO, "\e[?1049l\e[?25h", 14);
      c->alt_screen = FALSE;
    }
  if (c->raw_mode)
    {
      tcsetattr (STDIN_FILENO, TCSANOW, &c->saved_termios);
      c->raw_mode = FALSE;
    }
}

static void
client_quit (Client *c, const gchar *message, int code)
{
  client_restore_terminal (c);
  if (message != NULL)
    {
      c->exit_message = g_strdup (message);
      c->exit_code = code;
    }
  if (c->loop != NULL && g_main_loop_is_running (c->loop))
    g_main_loop_quit (c->loop);
}



/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

static void
client_write (const gchar *data, gsize len)
{
  gsize off = 0;
  while (off < len)
    {
      ssize_t n = write (STDOUT_FILENO, data + off, len - off);
      if (n <= 0)
        return;
      off += (gsize) n;
    }
}

static void
client_render_row_text (GString *out, glong row, const gchar *text,
                        glong width)
{
  /* Move to the row, erase it, print text truncated to width
   * visible cells.  Grid rows may now contain SGR sequences
   * (\e[...m) from the colour layer: those bytes are copied
   * verbatim, never counted as cells, and never cut in the middle
   * -- cutting inside an escape sequence, or counting its bytes
   * as text, would corrupt the row and shift every following cell.
   * Wide characters count as 2 cells; a wide character that would
   * straddle the width limit is skipped, not split.  A reset is
   * always appended, even if the row was truncated, so SGR state
   * from this row can never leak into the next row. */
  const gchar *p;
  glong cells = 0;

  g_string_append_printf (out, "\e[%ld;1H\e[K", row + 1);
  if (text == NULL)
    text = "";
  p = text;
  while (*p != '\0' && cells < width)
    {
      if (p[0] == '\e' && p[1] == '[')
        {
          /* CSI sequence: copy through its final byte (in the
           * range @ to ~), regardless of the width limit, so a
           * trailing reset is never lost.  SGR is the only CSI the
           * server emits, but copying any CSI this way is safe. */
          const gchar *q = p + 2;
          while (*q != '\0' && !(*q >= 0x40 && *q <= 0x7e))
            q++;
          if (*q != '\0')
            q++;
          g_string_append_len (out, p, q - p);
          p = q;
        }
      else
        {
          gunichar ch = g_utf8_get_char (p);
          const gchar *next = g_utf8_next_char (p);
          glong w = g_unichar_iswide (ch) ? 2 : 1;
          if (cells + w > width)
            break;
          g_string_append_len (out, p, next - p);
          cells += w;
          p = next;
        }
    }
  g_string_append (out, "\e[0m");
}

static void
client_render_menu (GString *out, Client *c)
{
  gchar *menu;
  if (c->grid_cols > 0
      && (glong) g_utf8_strlen (TERMSHARE_MENU_TEXT, -1) > c->grid_cols)
    menu = g_utf8_substring (TERMSHARE_MENU_TEXT, 0, c->grid_cols);
  else
    menu = g_strdup (TERMSHARE_MENU_TEXT);
  g_string_append_printf (out, "\e[1;1H\e[K\e[2m%s\e[0m", menu);
  g_free (menu);
}

static void
client_render_indicator (GString *out, Client *c)
{
  glong i, width = c->grid_cols;
  g_string_append (out, "\e[2;1H\e[K\e[32m");
  for (i = 0; i < width; i++)
    g_string_append (out, "─");
  g_string_append (out, "\e[0m");
}

/* ------------------------------------------------------------------ */
/* Tab bar: host-like GtkNotebook strip in character cells             */
/* ------------------------------------------------------------------ */

/* Visible-cell width of a string: SGR sequences count as 0, wide
 * (CJK etc.) characters count as 2, everything else as 1. The tab
 * bar bypasses client_render_row_text, so it must do its own
 * width accounting or a long bar wraps and corrupts the grid. */
static gboolean
client_char_is_wide (gunichar ch)
{
  return (ch >= 0x1100 && ch <= 0x115F)
      || (ch >= 0x2E80 && ch <= 0x303E)
      || (ch >= 0x3041 && ch <= 0xA4CF)
      || (ch >= 0xAC00 && ch <= 0xD7A3)
      || (ch >= 0xF900 && ch <= 0xFAFF)
      || (ch >= 0xFE10 && ch <= 0xFE19)
      || (ch >= 0xFE30 && ch <= 0xFE6F)
      || (ch >= 0xFF00 && ch <= 0xFF60)
      || (ch >= 0xFFE0 && ch <= 0xFFE6)
      || (ch >= 0x20000 && ch <= 0x2FFFD)
      || (ch >= 0x30000 && ch <= 0x3FFFD);
}

static glong
client_visible_width (const gchar *s)
{
  glong width = 0;
  const gchar *p;
  if (s == NULL)
    return 0;
  for (p = s; *p != '\0';)
    {
      gunichar ch;
      if (p[0] == '\e' && p[1] == '[')
        {
          const gchar *q = p + 2;
          while (*q != '\0' && !(*q >= '@' && *q <= '~'))
            q++;
          if (*q != '\0')
            q++;
          p = q;
          continue;
        }
      ch = g_utf8_get_char (p);
      if (ch == (gunichar) -1)
        {
          width += 1;
          p++;
          continue;
        }
      width += client_char_is_wide (ch) ? 2 : 1;
      p = g_utf8_next_char (p);
    }
  return width;
}

/* Truncate a title to at most max_cells visible cells, appending
 * "…" when truncated. A wide character that would straddle the
 * limit is skipped, never split. */
static gchar *
client_ellipsize_title (const gchar *title, glong max_cells)
{
  GString *s;
  glong cells = 0;
  const gchar *p;
  if (max_cells <= 0)
    return g_strdup ("");
  if (client_visible_width (title) <= max_cells)
    return g_strdup (title);
  if (max_cells == 1)
    return g_strdup ("…");
  s = g_string_new ("");
  for (p = title; *p != '\0' && cells < max_cells - 1;)
    {
      gunichar ch = g_utf8_get_char (p);
      glong cw;
      const gchar *next;
      if (ch == (gunichar) -1)
        {
          cw = 1;
          next = p + 1;
        }
      else
        {
          cw = client_char_is_wide (ch) ? 2 : 1;
          next = g_utf8_next_char (p);
        }
      if (cells + cw > max_cells - 1)
        break;
      g_string_append_len (s, p, next - p);
      cells += cw;
      p = next;
    }
  g_string_append (s, "…");
  return g_string_free (s, FALSE);
}

/* Width of one rendered tab cell: " " + title + (" ×" if the host
 * shows close buttons) + " ". */
static glong
client_tab_cell_width (glong title_cells, gboolean close_buttons)
{
  return 1 + title_cells + (close_buttons ? 2 : 0) + 1;
}

static void
client_render_tabbar (GString *out, Client *c)
{
  /* Fixed 256-colour pair, fg and bg both set explicitly so the
   * strip is readable on dark and light remote terminals alike
   * (the protocol does not carry the host theme, so this is a
   * stylised tab strip, not the host's exact colours). Active:
   * bold dark-on-light, like a raised tab; inactive and filler:
   * light-on-dark, like the host's dimmer tab strip. */
  const gchar *strip_style = "\e[38;5;250;48;5;240m";
  const gchar *active_style = "\e[1;38;5;240;48;5;250m";
  glong width = c->grid_cols;
  gchar **shown = NULL;
  glong *shown_width = NULL;
  gboolean *visible = NULL;
  glong total, rendered;
  gint i, n_visible;

  g_string_append (out, "\e[3;1H\e[K");
  if (c->n_tabs <= 0)
    {
      g_string_append (out, "\e[0m");
      return;
    }

  shown = g_new0 (gchar *, c->n_tabs);
  shown_width = g_new0 (glong, c->n_tabs);
  visible = g_new0 (gboolean, c->n_tabs);
  for (i = 0; i < c->n_tabs; i++)
    {
      shown[i] = g_strdup (c->tabs[i] != NULL ? c->tabs[i] : "");
      shown_width[i] = client_visible_width (shown[i]);
      visible[i] = TRUE;
    }

  if (width <= 0)
    {
      /* No grid size yet: render untruncated, no filler. */
      for (i = 0; i < c->n_tabs; i++)
        {
          if (i > 0)
            g_string_append (out, strip_style);
          if (i > 0)
            g_string_append (out, "│");
          g_string_append (out, i == c->active_tab ? active_style : strip_style);
          g_string_append_printf (out, " %s%s ", shown[i],
                                  c->tabs_close_buttons ? " ×" : "");
        }
      g_string_append (out, "\e[0m");
      goto done;
    }

  /* Overflow: shrink inactive tabs first, widest first, down to
   * a minimum cell (" … " plus the close mark if shown), keeping
   * the active tab fully visible. Only if every inactive tab is
   * at its minimum and the bar still does not fit do we ellipsize
   * the active tab, and only after that drop inactive tabs
   * farthest from the active one. The bar never wraps. */
  for (;;)
    {
      total = 0;
      n_visible = 0;
      for (i = 0; i < c->n_tabs; i++)
        {
          if (!visible[i])
            continue;
          total += client_tab_cell_width (shown_width[i],
                                          c->tabs_close_buttons);
          n_visible++;
        }
      if (n_visible > 1)
        total += n_visible - 1; /* │ separators */
      if (total <= width)
        break;
      {
        gint widest = -1;
        glong widest_width = 0;
        glong min_width = client_tab_cell_width (1, c->tabs_close_buttons);
        for (i = 0; i < c->n_tabs; i++)
          {
            glong w;
            if (!visible[i] || i == c->active_tab)
              continue;
            w = client_tab_cell_width (shown_width[i],
                                       c->tabs_close_buttons);
            if (w > min_width && w > widest_width)
              {
                widest = i;
                widest_width = w;
              }
          }
        if (widest < 0)
          break;
        {
          glong new_max = shown_width[widest] - 1;
          gchar *next;
          if (new_max < 1)
            new_max = 1;
          next = client_ellipsize_title (c->tabs[widest], new_max);
          g_free (shown[widest]);
          shown[widest] = next;
          shown_width[widest] = client_visible_width (next);
        }
      }
    }

  if (total > width)
    {
      /* Active tab alone still too wide: ellipsize it to fit. */
      if (c->active_tab >= 0 && c->active_tab < c->n_tabs
          && visible[c->active_tab])
        {
          glong budget = width
              - client_tab_cell_width (0, c->tabs_close_buttons);
          /* title cells available: width minus padding/close */
          gchar *next;
          if (budget < 1)
            budget = 1;
          next = client_ellipsize_title (c->tabs[c->active_tab], budget);
          g_free (shown[c->active_tab]);
          shown[c->active_tab] = next;
          shown_width[c->active_tab] =
              client_visible_width (next);
        }
      /* Still too wide (many tabs at minimum): drop inactive
       * tabs farthest from the active one, one at a time. */
      while (total > width)
        {
          gint farthest = -1;
          gint farthest_dist = -1;
          for (i = 0; i < c->n_tabs; i++)
            {
              gint dist;
              if (!visible[i] || i == c->active_tab)
                continue;
              dist = i > c->active_tab ? i - c->active_tab
                                       : c->active_tab - i;
              /* On a tie, drop the rightmost tab first, so the
               * strip keeps its leftmost tabs and overflows to
               * the right, like a notebook strip. */
              if (dist >= farthest_dist)
                {
                  farthest = i;
                  farthest_dist = dist;
                }
            }
          if (farthest < 0)
            break;
          visible[farthest] = FALSE;
          total = 0;
          n_visible = 0;
          for (i = 0; i < c->n_tabs; i++)
            {
              if (!visible[i])
                continue;
              total += client_tab_cell_width (shown_width[i],
                                              c->tabs_close_buttons);
              n_visible++;
            }
          if (n_visible > 1)
            total += n_visible - 1;
        }
    }

  /* If dropping a tab freed space, give it back to the remaining
   * inactive tabs (left to right, up to their full titles), so a
   * dropped tab does not leave the survivors stuck at "…". */
  if (total < width)
    {
      glong slack = width - total;
      for (i = 0; i < c->n_tabs && slack > 0; i++)
        {
          glong orig_width, new_max;
          gchar *next;
          if (!visible[i] || i == c->active_tab)
            continue;
          orig_width = client_visible_width (c->tabs[i]);
          if (shown_width[i] >= orig_width)
            continue;
          new_max = shown_width[i] + slack;
          if (new_max > orig_width)
            new_max = orig_width;
          next = client_ellipsize_title (c->tabs[i], new_max);
          slack -= client_visible_width (next) - shown_width[i];
          g_free (shown[i]);
          shown[i] = next;
          shown_width[i] = client_visible_width (next);
        }
    }

  /* Render visible tabs, then fill the rest of the row with the
   * strip background so the strip spans the full width. */
  rendered = 0;
  n_visible = 0;
  for (i = 0; i < c->n_tabs; i++)
    {
      if (!visible[i])
        continue;
      if (n_visible > 0)
        {
          g_string_append (out, strip_style);
          g_string_append (out, "│");
          rendered += 1;
        }
      g_string_append (out, i == c->active_tab ? active_style : strip_style);
      g_string_append_printf (out, " %s%s ", shown[i],
                              c->tabs_close_buttons ? " ×" : "");
      rendered += client_tab_cell_width (shown_width[i],
                                         c->tabs_close_buttons);
      n_visible++;
    }
  if (rendered < width)
    {
      glong j;
      g_string_append (out, strip_style);
      for (j = rendered; j < width; j++)
        g_string_append_c (out, ' ');
    }
  g_string_append (out, "\e[0m");

done:
  for (i = 0; i < c->n_tabs; i++)
    g_free (shown[i]);
  g_free (shown);
  g_free (shown_width);
  g_free (visible);
}

static void
client_move_cursor (GString *out, Client *c)
{
  if (c->grid_cols > 0 && c->grid_rows > 0
      && c->cursor_col < c->grid_cols
      && c->cursor_row < c->grid_rows
      && (TERMSHARE_CHROME_ROWS + c->cursor_row) < c->client_rows)
    {
      g_string_append_printf (out, "\e[%ld;%ldH\e[?25h",
                              TERMSHARE_CHROME_ROWS + c->cursor_row + 1,
                              c->cursor_col + 1);
    }
  else
    {
      g_string_append (out, "\e[?25l");
    }
}

static void
client_render_full (Client *c)
{
  GString *out = g_string_new ("");
  glong i;

  if (!c->attached)
    {
      g_string_free (out, TRUE);
      return;
    }
  client_render_menu (out, c);
  client_render_indicator (out, c);
  client_render_tabbar (out, c);
  for (i = 0; i < c->grid_rows; i++)
    {
      glong row = TERMSHARE_CHROME_ROWS + i;
      if (row >= c->client_rows)
        break;
      client_render_row_text (out, row,
                              (c->grid != NULL) ? c->grid[i] : "",
                              c->grid_cols);
    }
  client_move_cursor (out, c);
  client_write (out->str, out->len);
  g_string_free (out, TRUE);
}

static void
client_set_grid (Client *c, glong cols, glong rows, gchar **lines)
{
  glong i;
  if (c->grid != NULL)
    {
      for (i = 0; i < c->grid_rows; i++)
        g_free (c->grid[i]);
      g_free (c->grid);
    }
  c->grid_cols = cols;
  c->grid_rows = rows;
  c->grid = g_new0 (gchar *, rows > 0 ? rows : 1);
  for (i = 0; i < rows; i++)
    c->grid[i] = g_strdup (lines[i]);
}

static void
client_set_tabs (Client *c, gint active, gchar **titles, gint n,
                 gboolean close_buttons)
{
  gint i;
  if (c->tabs != NULL)
    g_strfreev (c->tabs);
  c->tabs = g_new0 (gchar *, n + 1);
  for (i = 0; i < n; i++)
    c->tabs[i] = g_strdup (titles[i]);
  c->n_tabs = n;
  c->active_tab = active;
  c->tabs_close_buttons = close_buttons;
}



/* ------------------------------------------------------------------ */
/* Line parsing helpers (payload is not NUL-terminated)                */
/* ------------------------------------------------------------------ */

/* Split payload into lines, keeping empty lines. Returns a NULL-
 * terminated array of newly allocated strings, and the count. */
static gchar **
client_split_lines (const guint8 *data, gsize len, gsize *n_out)
{
  GPtrArray *arr = g_ptr_array_new ();
  gsize start = 0, i;
  for (i = 0; i <= len; i++)
    {
      if (i == len || data[i] == '\n')
        {
          g_ptr_array_add (arr, g_strndup ((const gchar *) data + start,
                                           i - start));
          start = i + 1;
        }
    }
  /* A trailing newline produces a final empty element from the loop
   * above only when i == len after that newline; drop it so a frame
   * ending in '\n' does not gain a phantom line. */
  if (len > 0 && data[len - 1] == '\n' && arr->len > 0)
    {
      g_free (g_ptr_array_index (arr, arr->len - 1));
      g_ptr_array_remove_index (arr, arr->len - 1);
    }
  g_ptr_array_add (arr, NULL);
  if (n_out != NULL)
    *n_out = arr->len - 1;
  return (gchar **) g_ptr_array_free (arr, FALSE);
}

static gboolean
client_parse_tabbar_line (Client *c, const gchar *line)
{
  /* TABBAR <active> <close-buttons 0/1>\t<title>\t...
   * The close flag rides in the header field, after a space, so
   * an old client still parses it: atoi() stops at the space and
   * titles (fields+1) are unchanged. An old server sends only
   * "<active>", in which case the flag is absent and defaults
   * to no close mark. */
  gchar **fields;
  gint i, n, active = -1;
  gboolean close_buttons = FALSE;
  const gchar *space;
  if (!g_str_has_prefix (line, "TABBAR "))
    return FALSE;
  fields = g_strsplit (line + 7, "\t", -1);
  n = g_strv_length (fields);
  if (n < 1)
    {
      g_strfreev (fields);
      return FALSE;
    }
  active = atoi (fields[0]);
  space = strchr (fields[0], ' ');
  if (space != NULL)
    close_buttons = atoi (space + 1) != 0;
  client_set_tabs (c, active, fields + 1, n - 1, close_buttons);
  for (i = 0; i < n; i++)
    (void) i;
  g_strfreev (fields);
  return TRUE;
}



/* ------------------------------------------------------------------ */
/* Server frames                                                       */
/* ------------------------------------------------------------------ */

static void
client_send_resize (Client *c)
{
  gchar *text;
  if (c->writer == NULL || !client_get_size (c))
    return;
  text = g_strdup_printf ("RESIZE %ld %ld\n", c->client_cols,
                          c->client_rows);
  terminal_share_frame_writer_queue_text (c->writer, text);
  g_free (text);
}


/* Print one line per window: "<label>  <tab1>* | <tab2> | ...".
 * The label is the sequential number when numbered is TRUE, else
 * the window id from the WINDOW line. Tab titles are what the user
 * recognises a session by, so the window title is not shown, except
 * as a fallback for a window with no tabs at all. The active tab
 * gets a trailing "*". */
static void
client_print_session_list (gchar **lines, gsize n_lines,
                           gboolean numbered)
{
  guint number = 0;
  guint window_id = 0;
  gchar *window_title = NULL;
  GString *tabs = NULL;
  gsize i;

  for (i = 1; i < n_lines; i++)
    {
      if (g_str_has_prefix (lines[i], "WINDOW "))
        {
          /* Flush the previous window. */
          if (tabs != NULL)
            {
              {
                gchar *label = g_strdup_printf ("%u", numbered ? number : window_id);
                g_print ("%s  %s\n", label,
                         tabs->len > 0 ? tabs->str
                                       : (window_title != NULL ? window_title : ""));
                g_free (label);
              }
              g_string_free (tabs, TRUE);
              tabs = NULL;
              g_free (window_title);
              window_title = NULL;
            }
          number++;
          window_id = (guint) atoi (lines[i] + 7);
          {
            gchar **f = g_strsplit (lines[i], "\t", -1);
            window_title = g_strdup (f[1] != NULL ? f[1] : "");
            g_strfreev (f);
          }
          tabs = g_string_new ("");
        }
      else if (g_str_has_prefix (lines[i], "TAB ") && tabs != NULL)
        {
          gchar **f = g_strsplit (lines[i], "\t", -1);
          if (f[3] != NULL)
            {
              if (tabs->len > 0)
                g_string_append (tabs, " | ");
              g_string_append (tabs, f[3]);
              if (g_strcmp0 (f[2], "1") == 0)
                g_string_append_c (tabs, '*');
            }
          g_strfreev (f);
        }
    }
  if (tabs != NULL)
    {
      {
        gchar *label = g_strdup_printf ("%u", numbered ? number : window_id);
        g_print ("%s  %s\n", label,
                 tabs->len > 0 ? tabs->str
                               : (window_title != NULL ? window_title : ""));
        g_free (label);
      }
      g_string_free (tabs, TRUE);
      g_free (window_title);
    }
}

/* Attach mode, after a LIST frame arrives: if a window id was given
 * on the command line, attach to it; otherwise print the sessions
 * numbered 1, 2, 3, ... (in list order, not by internal window id),
 * wait for the user to pick a number in normal mode, and attach to
 * the window behind that number. Returns after attaching or after
 * quitting on a bad / missing pick. */
static void
client_pick_and_attach (Client *c, gchar **lines, gsize n_lines)
{
  GArray *ids = g_array_new (FALSE, FALSE, sizeof (guint));
  gsize i;
  guint window_number = 0;

  for (i = 1; i < n_lines; i++)
    {
      if (g_str_has_prefix (lines[i], "WINDOW "))
        {
          guint id = (guint) atoi (lines[i] + 7);
          g_array_append_val (ids, id);
        }
    }

  if (ids->len == 0)
    {
      g_array_free (ids, TRUE);
      client_quit (c, "No shared windows are available.", 1);
      return;
    }

  if (c->target_set)
    {
      gboolean found = FALSE;
      guint j;
      for (j = 0; j < ids->len; j++)
        if (g_array_index (ids, guint, j) == c->target_window_id)
          found = TRUE;
      g_array_free (ids, TRUE);
      if (!found)
        {
          client_quit (c, "No such shared window.", 1);
          return;
        }
      {
        gchar *attach = g_strdup_printf ("ATTACH %u\n",
                                         c->target_window_id);
        client_enter_raw (c);
        c->attached = TRUE;
        terminal_share_frame_writer_queue_text (c->writer, attach);
        g_free (attach);
        client_send_resize (c);
      }
      return;
    }

  /* Numbered list, one line per window, tab titles joined
   * with " | ". window_number counts windows in list order, which
   * is the same order the ids array above was built in, so picking
   * number N maps to ids[N-1]. */
  client_print_session_list (lines, n_lines, TRUE);
  window_number = ids->len;

  {
    gchar buf[64];
    guint choice = 0;
    guint attach_id;
    gchar *attach;

    g_print ("Pick a session (1-%u): ", window_number);
    fflush (stdout);
    if (fgets (buf, sizeof (buf), stdin) == NULL)
      {
        g_array_free (ids, TRUE);
        client_quit (c, "No session picked.", 1);
        return;
      }
    choice = (guint) atoi (buf);
    if (choice < 1 || choice > ids->len)
      {
        g_array_free (ids, TRUE);
        client_quit (c, "No such session.", 1);
        return;
      }
    attach_id = g_array_index (ids, guint, choice - 1);
    g_array_free (ids, TRUE);
    attach = g_strdup_printf ("ATTACH %u\n", attach_id);
    client_enter_raw (c);
    c->attached = TRUE;
    terminal_share_frame_writer_queue_text (c->writer, attach);
    g_free (attach);
    client_send_resize (c);
  }
}

static void
client_frame_cb (const guint8 *data, gsize len, gpointer user_data)
{
  Client *c = user_data;
  gchar *text;
  gsize n_lines = 0;
  gchar **lines;

  if (len >= 7 && memcmp (data, "WELCOME", 7) == 0)
    return;
  if (len >= 4 && memcmp (data, "BUSY", 4) == 0)
    {
      client_quit (c, "Another client is already attached.", 1);
      return;
    }
  if (len >= 3 && memcmp (data, "BYE", 3) == 0)
    {
      text = g_strndup ((const gchar *) data, len);
      g_strchomp (text);
      client_quit (c, (g_str_has_prefix (text, "BYE ") && text[4] != '\0')
                          ? text + 4 : "Sharing stopped.", 0);
      g_free (text);
      return;
    }
  if (len >= 5 && memcmp (data, "ERROR", 5) == 0)
    {
      text = g_strndup ((const gchar *) data, len);
      g_strchomp (text);
      client_quit (c, (g_str_has_prefix (text, "ERROR ") && text[6] != '\0')
                          ? text + 6 : "Server error.", 1);
      g_free (text);
      return;
    }

  lines = client_split_lines (data, len, &n_lines);
  if (n_lines == 0)
    {
      g_strfreev (lines);
      return;
    }

  if (g_str_has_prefix (lines[0], "LIST"))
    {
      if (c->list_mode)
        {
          client_print_session_list (lines, n_lines, FALSE);
          client_quit (c, NULL, 0);
          g_strfreev (lines);
          return;
        }

      client_pick_and_attach (c, lines, n_lines);
      g_strfreev (lines);
      return;
    }

  if (g_str_has_prefix (lines[0], "SYNC "))
    {
      glong cols = 0, rows = 0, cur_col = 0, cur_row = 0;
      if (sscanf (lines[0], "SYNC %ld %ld %ld %ld", &cols, &rows,
                  &cur_col, &cur_row) == 4
          && n_lines >= 2
          && client_parse_tabbar_line (c, lines[1]))
        {
          /* Grid lines are lines[2 .. 2+rows-1]; missing lines are
           * treated as empty, extra lines are ignored. */
          gchar **grid = g_new0 (gchar *, rows > 0 ? rows : 1);
          glong i;
          for (i = 0; i < rows; i++)
            grid[i] = g_strdup ((2 + i < (glong) n_lines)
                                    ? lines[2 + i] : "");
          client_set_grid (c, cols, rows, grid);
          for (i = 0; i < rows; i++)
            g_free (grid[i]);
          g_free (grid);
          c->cursor_col = cur_col;
          c->cursor_row = cur_row;
          c->attached = TRUE;
          client_render_full (c);
        }
      g_strfreev (lines);
      return;
    }

  if (g_str_has_prefix (lines[0], "UPDATE "))
    {
      glong cur_col = 0, cur_row = 0;
      GString *out;
      gsize i;
      gboolean changed = FALSE;
      if (sscanf (lines[0], "UPDATE %ld %ld", &cur_col, &cur_row) != 2)
        {
          g_strfreev (lines);
          return;
        }
      out = g_string_new ("");
      for (i = 1; i < n_lines; i++)
        {
          if (g_str_has_prefix (lines[i], "ROW "))
            {
              gchar *tab = strchr (lines[i] + 4, '\t');
              glong row;
              if (tab == NULL)
                continue;
              row = atol (lines[i] + 4);
              if (row >= 0 && row < c->grid_rows)
                {
                  g_free (c->grid[row]);
                  c->grid[row] = g_strdup (tab + 1);
                  client_render_row_text (out, TERMSHARE_CHROME_ROWS + row,
                                          tab + 1, c->grid_cols);
                  changed = TRUE;
                }
            }
        }
      if (cur_col != c->cursor_col || cur_row != c->cursor_row)
        changed = TRUE;
      c->cursor_col = cur_col;
      c->cursor_row = cur_row;
      if (changed)
        {
          client_move_cursor (out, c);
          client_write (out->str, out->len);
        }
      g_string_free (out, TRUE);
      g_strfreev (lines);
      return;
    }

  if (g_str_has_prefix (lines[0], "SIZE "))
    {
      /* A SIZE frame is always followed by a SYNC, which does the
       * actual re-render; nothing to do here. */
      g_strfreev (lines);
      return;
    }

  if (g_str_has_prefix (lines[0], "TABBAR "))
    {
      if (client_parse_tabbar_line (c, lines[0]) && c->attached)
        {
          GString *out = g_string_new ("");
          client_render_tabbar (out, c);
          client_move_cursor (out, c);
          client_write (out->str, out->len);
          g_string_free (out, TRUE);
        }
      g_strfreev (lines);
      return;
    }

  g_strfreev (lines);
}

static void
client_end_cb (gpointer user_data)
{
  Client *c = user_data;
  if (c->list_mode && !c->attached)
    client_quit (c, "Sharing is off on the host.", 1);
  else if (c->attached)
    client_quit (c, "Host disconnected.", 0);
  else
    client_quit (c, "Connection closed.", 1);
}

static gboolean
client_stdin_cb (GIOChannel *source, GIOCondition condition,
                 gpointer user_data)
{
  Client *c = user_data;
  gchar buf[4096];
  ssize_t n;
  (void) source;
  if ((condition & (G_IO_HUP | G_IO_ERR)) != 0)
    {
      client_quit (c, NULL, 0);
      return FALSE;
    }
  n = read (STDIN_FILENO, buf, sizeof (buf));
  if (n <= 0)
    {
      client_quit (c, NULL, 0);
      return FALSE;
    }
  /* Ctrl+] detaches. It is checked before forwarding so that the
   * byte never reaches the host shell. */
  if (memchr (buf, 0x1d, (gsize) n) != NULL)
    {
      GBytes *frame = terminal_share_encode_frame ("DETACH\n", 7);
      gsize size;
      gconstpointer data = g_bytes_get_data (frame, &size);
      GOutputStream *out =
          g_io_stream_get_output_stream (G_IO_STREAM (c->conn));
      g_output_stream_write_all (out, data, size, NULL, NULL, NULL);
      g_bytes_unref (frame);
      client_quit (c, NULL, 0);
      return FALSE;
    }
  {
    GString *frame = g_string_new ("INPUT ");
    g_string_append_len (frame, buf, n);
    terminal_share_frame_writer_queue (c->writer, frame->str, frame->len);
    g_string_free (frame, TRUE);
  }
  return TRUE;
}

static gboolean
client_sigwinch_cb (gpointer user_data)
{
  Client *c = user_data;
  client_send_resize (c);
  /* Re-render at the new client size even if the host grid did not
   * change (e.g. the remote grew: the window stays top-left and the
   * extra space must be cleared of stale rows). */
  if (c->attached)
    {
      client_write ("\e[2J", 4);
      client_render_full (c);
    }
  return TRUE;
}

static gboolean
client_signal_cb (gpointer user_data)
{
  Client *c = user_data;
  client_quit (c, NULL, 0);
  return FALSE;
}



int
main (int argc, char **argv)
{
  Client c;
  gchar *socket_path;
  GSocketClient *socket_client;
  GSocketAddress *address;
  GError *error = NULL;
  GIOChannel *stdin_channel;

  memset (&c, 0, sizeof (c));
  c.loop = g_main_loop_new (NULL, FALSE);
  c.exit_code = 0;

  /* No arguments means: show the numbered session list and wait
   * for the user to pick one (the same as "attach" with no id). */
  if (argc >= 2 && g_strcmp0 (argv[1], "list") != 0
      && g_strcmp0 (argv[1], "attach") != 0)
    {
      g_printerr ("Usage: %s [list | attach [window-id]]\n", argv[0]);
      return 2;
    }
  c.list_mode = (argc >= 2 && g_strcmp0 (argv[1], "list") == 0);
  if (!c.list_mode && argc >= 3)
    {
      c.target_window_id = (guint) atoi (argv[2]);
      c.target_set = TRUE;
    }

  if (!c.list_mode && !client_get_size (&c))
    {
      g_printerr ("termshare-client: cannot determine terminal size\n");
      return 1;
    }
  if (!c.list_mode && c.client_rows <= TERMSHARE_CHROME_ROWS)
    {
      g_printerr ("termshare-client: terminal too small for the shared "
                  "window (needs more than %d rows)\n",
                  TERMSHARE_CHROME_ROWS);
      return 1;
    }

  socket_path = g_build_filename (g_get_user_runtime_dir (),
                                  "termshare", "share.sock", NULL);
  if (!g_file_test (socket_path, G_FILE_TEST_EXISTS))
    {
      g_printerr ("termshare-client: sharing is off on the host "
                  "(no socket at %s)\n", socket_path);
      g_free (socket_path);
      return 1;
    }

  socket_client = g_socket_client_new ();
  address = g_unix_socket_address_new (socket_path);
  c.conn = g_socket_client_connect (socket_client,
                                    G_SOCKET_CONNECTABLE (address),
                                    NULL, &error);
  g_object_unref (address);
  g_object_unref (socket_client);
  g_free (socket_path);
  if (c.conn == NULL)
    {
      g_printerr ("termshare-client: cannot connect to the host: %s\n",
                  error != NULL ? error->message : "unknown error");
      g_clear_error (&error);
      return 1;
    }

  c.writer = terminal_share_frame_writer_new (
      g_io_stream_get_output_stream (G_IO_STREAM (c.conn)));
  c.reader = terminal_share_frame_reader_new (
      g_io_stream_get_input_stream (G_IO_STREAM (c.conn)),
      client_frame_cb, client_end_cb, &c);
  terminal_share_frame_reader_start (c.reader);

  /* The server sends WELCOME first; LIST is the first real exchange
   * in both modes. */
  terminal_share_frame_writer_queue_text (c.writer, "LIST\n");

  if (!c.list_mode)
    {
      stdin_channel = g_io_channel_unix_new (STDIN_FILENO);
      g_io_channel_set_encoding (stdin_channel, NULL, NULL);
      g_io_add_watch (stdin_channel, G_IO_IN | G_IO_HUP | G_IO_ERR,
                      client_stdin_cb, &c);
      g_io_channel_unref (stdin_channel);
      g_unix_signal_add (SIGWINCH, client_sigwinch_cb, &c);
      g_unix_signal_add (SIGINT, client_signal_cb, &c);
      g_unix_signal_add (SIGTERM, client_signal_cb, &c);
    }

  g_main_loop_run (c.loop);

  /* Flush any queued DETACH before closing, best effort. */
  if (c.writer != NULL)
    {
      /* The writer queue is asynchronous; a short main-loop iteration
       * is not guaranteed to flush it, so a DETACH already queued is
       * best effort. The server also learns of the disconnect from
       * EOF, which restores the host size the same way. */
    }
  client_restore_terminal (&c);
  if (c.reader != NULL)
    terminal_share_frame_reader_free (c.reader);
  if (c.writer != NULL)
    terminal_share_frame_writer_free (c.writer);
  if (c.conn != NULL)
    {
      g_io_stream_close (G_IO_STREAM (c.conn), NULL, NULL);
      g_object_unref (c.conn);
    }
  if (c.exit_message != NULL)
    {
      g_printerr ("termshare-client: %s\n", c.exit_message);
      g_free (c.exit_message);
    }
  g_main_loop_unref (c.loop);
  return c.exit_code;
}
