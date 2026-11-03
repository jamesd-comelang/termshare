/*-
 * Copyright (c) 2026 James Ding
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or (at
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
 * termshare colour abstraction layer.
 *
 * The sharing server must not know which VTE it is running on.  It
 * asks this layer for a snapshot: per row, plain text plus a list of
 * attribute runs.  It then converts runs to ANSI SGR with
 * terminal_share_colour_row_to_sgr() and sends the SGR-encoded rows
 * over the (unchanged) wire protocol, exactly as if they were plain
 * text rows: SGR sequences contain no tab or newline, so no new frame
 * type is needed.
 *
 * Backends, chosen ONCE at runtime and cached -- never by a
 * compile-time VTE_CHECK_VERSION, because the binary built on Ubuntu
 * 24.04 (VTE 0.76) must run on Ubuntu 22.04 Jammy (VTE 0.68), where
 * the newer API does not exist at all:
 *
 *   html       VTE >= 0.72: vte_terminal_get_text_range_format() and
 *              vte_terminal_get_text_format() resolved with dlsym()
 *              (never linked directly, or the binary would fail to
 *              load on Jammy).  HTML spans are parsed into runs.
 *   attributes VTE < 0.72 (dlsym failed): vte_terminal_get_text_range()
 *              with a non-NULL attributes array.  Deprecated in 0.68
 *              but still returns data there; it returns NULL from
 *              0.72, which is why this path runs only when dlsym
 *              failed.  The old VteCharAttributes struct carries
 *              foreground/background colours, underline and
 *              strikethrough, but NOT bold or italic -- those fields
 *              are left FALSE here, never invented.  Colours are
 *              marked explicit (fg_set/bg_set TRUE) in this backend
 *              for cells that have an attribute entry.  Cells with
 *              no entry, and default runs from the other backends,
 *              stay unset here: the sharing server's caller
 *              (TerminalScreen, which recorded the colours it handed
 *              to VTE) fills them in with
 *              terminal_share_colour_apply_defaults(), so no backend
 *              needs a VTE default-colour getter (there is no public
 *              one on 0.68/0.76).
 *   plain      Neither backend produced runs: one default run per
 *              row, i.e. today's plain-text behaviour, so sharing
 *              never breaks because colour failed.
 *
 * Row mapping, which both backends must honour: the caller passes
 * visible rows (0..rows-1) and the first visible (scrollback) row.
 * get_text_format() returns visible rows only, so it is used for a
 * full snapshot; get_text_range_format() / get_text_range() rows are
 * absolute and scrollback-inclusive, so a visible row v is requested
 * as absolute row first_visible + v.  Callers get first_visible from
 * the VTE scrollable adjustment, the same value the plain snapshot
 * already uses for the cursor.
 *
 * Columns are visible cells: a wide (CJK) character occupies 2
 * cells, every other printable character 1.  Runs are half-open
 * [start_col, end_col) in those cells, sorted, non-overlapping.
 */

#ifndef TERMINAL_SHARE_COLOUR_H
#define TERMINAL_SHARE_COLOUR_H

#include <gtk/gtk.h>
#include <vte/vte.h>

G_BEGIN_DECLS

typedef struct
{
  glong start_col; /* inclusive, visible cells */
  glong end_col;   /* exclusive, visible cells */
  gboolean fg_set;
  gboolean bg_set;
  guint8 fg_r;
  guint8 fg_g;
  guint8 fg_b;
  guint8 bg_r;
  guint8 bg_g;
  guint8 bg_b;
  gboolean bold;
  gboolean italic;
  gboolean underline;
  gboolean strikethrough;
  /* Set only if a backend explicitly reports them.  The HTML backend
   * does not set these: VTE's HTML writer already swaps colours for
   * reverse and darkens them for dim, so the colours in the run
   * already capture the effect and emitting SGR 7/2 as well would
   * double-apply it on the remote terminal. */
  gboolean reverse;
  gboolean dim;
} TerminalShareColourRun;

typedef struct
{
  gchar *text; /* plain text, no SGR, UTF-8 */
  TerminalShareColourRun *runs;
  gsize n_runs;
} TerminalShareColourRow;

/* Which backend the runtime probe selected.  "unknown" means the
 * probe has not run yet (it runs on the first snapshot). */
const gchar *
terminal_share_colour_backend_name (void);

/* Snapshot of the visible grid as text + runs.  terminal is the
 * VTE widget; first_visible is the scrollback row of visible row 0;
 * cols/rows are the visible grid size.  On success, *rows_out holds
 * exactly rows rows (possibly with empty text); free with
 * terminal_share_colour_snapshot_free().  This never fails merely
 * because colour failed: on backend failure it returns plain text
 * with one default run per row.  It returns FALSE only when the
 * grid itself cannot be read (NULL terminal, bad size). */
gboolean
terminal_share_colour_snapshot (VteTerminal *terminal,
                                glong first_visible,
                                glong cols,
                                glong rows,
                                TerminalShareColourRow **rows_out);

void
terminal_share_colour_snapshot_free (TerminalShareColourRow *rows,
                                     glong n_rows);

/* Deterministic runs -> SGR conversion.  The result always starts
 * with \e[0m and ends with \e[0m, so SGR state can never leak into
 * the next row on the remote terminal, even if the client truncates
 * the row.  Identical runs produce identical output, which matters
 * because the server diffs rows by string compare: non-deterministic
 * output would resend every row on every update.  SGR order is
 * fixed: reset, fg (38;2;R;G;B if set), bg (48;2;R;G;B if set),
 * bold (1), italic (3), underline (4), strikethrough (9),
 * reverse (7), dim (2).  Default runs emit only the reset. */
/* Fill in any unset colours in every run of every row with the
 * given host default colours, leaving explicitly-set colours
 * untouched.  This is how the sharing server stops a remote grid
 * showing the remote terminal's own background: default runs
 * (HTML backend: unmarked text; attributes backend: cells with no
 * attribute entry, e.g. trailing/blank cells; plain fallback: the
 * whole row) would otherwise be sent with no background at all,
 * and the client erases those cells with the remote default.  The
 * colours passed here must be the host's actual VTE colours (see
 * TerminalScreen's recorded foreground/background), never an
 * invented theme colour: if the host's own background is grey,
 * the remote grid will (correctly) be grey too. */
void
terminal_share_colour_apply_defaults (TerminalShareColourRow *rows,
                                      glong n_rows,
                                      guint8 fg_r, guint8 fg_g, guint8 fg_b,
                                      guint8 bg_r, guint8 bg_g, guint8 bg_b);

gchar *
terminal_share_colour_row_to_sgr (const TerminalShareColourRow *row,
                                  glong cols);

/* Test hook: parse HTML (as returned by VTE's HTML export) for one
 * row into runs.  Returns FALSE on a structural failure (unbalanced
 * markup is tolerated and does not fail).  The caller owns *runs_out
 * (free with g_free) and *text_out (free with g_free, plain decoded
 * text, entities unescaped).  Used by the colour test program; the
 * snapshot path uses the same parser internally. */
gboolean
terminal_share_colour_parse_html_row (const gchar *html,
                                      gchar **text_out,
                                      TerminalShareColourRun **runs_out,
                                      gsize *n_runs_out);

G_END_DECLS

#endif /* !TERMINAL_SHARE_COLOUR_H */
