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
 * See terminal-share-colour.h for the backend contract.  This file
 * is the only place that knows about VTE colour APIs, HTML, or
 * VteCharAttributes.  The server and the protocol never see them.
 *
 * Two rules that must not be broken by future edits:
 *
 *  1. NEVER call vte_terminal_get_text_range_format() or
 *     vte_terminal_get_text_format() directly, and NEVER use the
 *     VteFormat enum from the headers.  Both the functions and the
 *     enum exist only in VTE >= 0.72, so the Jammy headers (0.68)
 *     do not declare them and the Jammy library does not provide
 *     them.  Direct use would either fail to compile on Jammy or,
 *     worse, make a Noble-built binary fail to load on Jammy with
 *     an undefined symbol.  All access is through the dlsym
 *     function pointers below, with the format passed as a plain
 *     int: 0 = TEXT, 1 = HTML, matching the VteFormat enum order.
 *
 *  2. NEVER use a compile-time VTE version check to pick a backend.
 *     The probe in this file runs at runtime, once, and caches its
 *     result.  A binary built on Noble runs on Jammy with an older
 *     library, so a compile-time answer would be wrong there.
 */

#define _GNU_SOURCE

#include "terminal-share-colour.h"

#include <dlfcn.h>
#include <string.h>

/* VteFormat values, as plain ints (see rule 1 above). */
#define TERMSHARE_VTE_FORMAT_TEXT 0
#define TERMSHARE_VTE_FORMAT_HTML 1

typedef char *(*GetTextFormatFn) (VteTerminal *, int);
typedef char *(*GetTextRangeFormatFn) (VteTerminal *, int,
                                       glong, glong, glong, glong,
                                       gsize *);

typedef enum
{
  TERMSHARE_COLOUR_BACKEND_UNKNOWN,
  TERMSHARE_COLOUR_BACKEND_HTML,
  TERMSHARE_COLOUR_BACKEND_ATTRIBUTES,
  TERMSHARE_COLOUR_BACKEND_PLAIN,
} TermshareColourBackend;

static gboolean colour_probed = FALSE;
static TermshareColourBackend colour_backend = TERMSHARE_COLOUR_BACKEND_UNKNOWN;
static GetTextFormatFn colour_get_text_format = NULL;
static GetTextRangeFormatFn colour_get_text_range_format = NULL;

const gchar *
terminal_share_colour_backend_name (void)
{
  if (!colour_probed)
    return "unknown";
  switch (colour_backend)
    {
    case TERMSHARE_COLOUR_BACKEND_HTML:
      return "html";
    case TERMSHARE_COLOUR_BACKEND_ATTRIBUTES:
      return "attributes";
    case TERMSHARE_COLOUR_BACKEND_PLAIN:
      return "plain";
    default:
      return "unknown";
    }
}

static void
colour_probe (void)
{
  void *sym;
  void *handle;

  if (colour_probed)
    return;
  colour_probed = TRUE;

  /* RTLD_DEFAULT first: the VTE library is already loaded, because
   * the host window owns a VteTerminal widget. */
  sym = dlsym (RTLD_DEFAULT, "vte_terminal_get_text_range_format");
  if (sym != NULL)
    {
      colour_get_text_range_format = (GetTextRangeFormatFn) sym;
      sym = dlsym (RTLD_DEFAULT, "vte_terminal_get_text_format");
      if (sym != NULL)
        colour_get_text_format = (GetTextFormatFn) sym;
      colour_backend = TERMSHARE_COLOUR_BACKEND_HTML;
      return;
    }

  /* Fallback: open the library by soname and look the symbol up
   * there.  On Jammy this dlopen succeeds (same soname) but the
   * dlsym fails, which is exactly the detection we want. */
  handle = dlopen ("libvte-2.91.so.0", RTLD_NOW | RTLD_GLOBAL);
  if (handle != NULL)
    {
      sym = dlsym (handle, "vte_terminal_get_text_range_format");
      if (sym != NULL)
        {
          colour_get_text_range_format = (GetTextRangeFormatFn) sym;
          sym = dlsym (handle, "vte_terminal_get_text_format");
          if (sym != NULL)
            colour_get_text_format = (GetTextFormatFn) sym;
          colour_backend = TERMSHARE_COLOUR_BACKEND_HTML;
          /* Keep the handle open for the process lifetime: the
           * function pointers above point into it. */
          return;
        }
      dlclose (handle);
    }

  colour_backend = TERMSHARE_COLOUR_BACKEND_ATTRIBUTES;
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static gint
colour_char_width (gunichar ch)
{
  if (ch == 0)
    return 0;
  return g_unichar_iswide (ch) ? 2 : 1;
}

static gboolean
colour_attr_equal (const TerminalShareColourRun *a,
                   const TerminalShareColourRun *b)
{
  return a->fg_set == b->fg_set && a->bg_set == b->bg_set
      && a->fg_r == b->fg_r && a->fg_g == b->fg_g && a->fg_b == b->fg_b
      && a->bg_r == b->bg_r && a->bg_g == b->bg_g && a->bg_b == b->bg_b
      && a->bold == b->bold && a->italic == b->italic
      && a->underline == b->underline
      && a->strikethrough == b->strikethrough
      && a->reverse == b->reverse && a->dim == b->dim;
}

static void
colour_run_from_state (TerminalShareColourRun *run,
                       glong start_col,
                       glong end_col,
                       const TerminalShareColourRun *state)
{
  *run = *state;
  run->start_col = start_col;
  run->end_col = end_col;
}

static gboolean
colour_parse_hex (const gchar *s, guint8 *r, guint8 *g, guint8 *b)
{
  unsigned int ri, gi, bi;
  if (s == NULL || s[0] != '#')
    return FALSE;
  if (strlen (s) == 7
      && sscanf (s + 1, "%02x%02x%02x", &ri, &gi, &bi) == 3)
    {
      *r = (guint8) ri;
      *g = (guint8) gi;
      *b = (guint8) bi;
      return TRUE;
    }
  if (strlen (s) == 4
      && sscanf (s + 1, "%01x%01x%01x", &ri, &gi, &bi) == 3)
    {
      /* #RGB expands to #RRGGBB by duplicating each nibble. */
      *r = (guint8) (ri * 17);
      *g = (guint8) (gi * 17);
      *b = (guint8) (bi * 17);
      return TRUE;
    }
  return FALSE;
}

/* Parse one "key: value" pair from a style="..." attribute.  Returns
 * TRUE if it recognised (and applied) the pair. */
static gboolean
colour_apply_style_pair (TerminalShareColourRun *state,
                         const gchar *key, const gchar *value)
{
  gchar *k, *v;
  gboolean handled = FALSE;

  k = g_strstrip (g_ascii_strdown (key, -1));
  v = g_strstrip (g_strdup (value));
  if (g_strcmp0 (k, "color") == 0)
    {
      guint8 r, g, b;
      if (colour_parse_hex (v, &r, &g, &b))
        {
          state->fg_set = TRUE;
          state->fg_r = r;
          state->fg_g = g;
          state->fg_b = b;
          handled = TRUE;
        }
    }
  else if (g_strcmp0 (k, "background-color") == 0)
    {
      guint8 r, g, b;
      if (colour_parse_hex (v, &r, &g, &b))
        {
          state->bg_set = TRUE;
          state->bg_r = r;
          state->bg_g = g;
          state->bg_b = b;
          handled = TRUE;
        }
    }
  else if (g_strcmp0 (k, "text-decoration") == 0)
    {
      if (strstr (v, "line-through") != NULL)
        {
          state->strikethrough = TRUE;
          handled = TRUE;
        }
      else if (strstr (v, "underline") != NULL)
        {
          state->underline = TRUE;
          handled = TRUE;
        }
    }
  g_free (k);
  g_free (v);
  return handled;
}

static void
colour_apply_style (TerminalShareColourRun *state, const gchar *style)
{
  gchar **pairs;
  gint i;

  if (style == NULL)
    return;
  pairs = g_strsplit (style, ";", -1);
  for (i = 0; pairs[i] != NULL; i++)
    {
      gchar *colon = strchr (pairs[i], ':');
      if (colon != NULL)
        {
          *colon = '\0';
          colour_apply_style_pair (state, pairs[i], colon + 1);
        }
    }
  g_strfreev (pairs);
}

/* Extract attr="value" from a tag body (tag body without < >).
 * Returns a newly allocated value, or NULL. */
static gchar *
colour_tag_attr (const gchar *tag, const gchar *name)
{
  const gchar *p = tag;
  gsize name_len = strlen (name);

  while ((p = strstr (p, name)) != NULL)
    {
      const gchar *after = p + name_len;
      /* The name must be a whole attribute name, followed by
       * optional spaces and '='. */
      while (*after == ' ' || *after == '\t')
        after++;
      if (*after != '=')
        {
          p += name_len;
          continue;
        }
      after++;
      while (*after == ' ' || *after == '\t')
        after++;
      if (*after == '"' || *after == '\'')
        {
          gchar quote = *after;
          const gchar *end = strchr (after + 1, quote);
          if (end != NULL)
            return g_strndup (after + 1, end - (after + 1));
          return NULL;
        }
      else
        {
          const gchar *end = after;
          while (*end != '\0' && *end != ' ' && *end != '\t')
            end++;
          return g_strndup (after, end - after);
        }
    }
  return NULL;
}

/* ------------------------------------------------------------------ */
/* HTML parser: one row                                               */
/* ------------------------------------------------------------------ */

typedef struct
{
  gchar *tag; /* lower-case tag name, without '/' */
  TerminalShareColourRun state;
} HtmlFrame;

typedef struct
{
  gunichar ch;
  TerminalShareColourRun state;
} HtmlCell;

/* Decode one HTML entity starting at *p (which points at '&').
 * On success, returns the decoded codepoint and sets *consumed to
 * the number of bytes consumed (including '&' and ';').  On
 * failure, returns 0 and *consumed = 0 (caller copies '&' literally). */
static gunichar
colour_decode_entity (const gchar *p, gsize *consumed)
{
  const gchar *semi;
  gchar *body;
  gunichar ch = 0;

  *consumed = 0;
  semi = strchr (p, ';');
  if (semi == NULL || semi - p > 12)
    return 0;
  body = g_strndup (p + 1, semi - (p + 1));
  if (g_strcmp0 (body, "amp") == 0)
    ch = '&';
  else if (g_strcmp0 (body, "lt") == 0)
    ch = '<';
  else if (g_strcmp0 (body, "gt") == 0)
    ch = '>';
  else if (g_strcmp0 (body, "quot") == 0)
    ch = '"';
  else if (g_strcmp0 (body, "apos") == 0)
    ch = '\'';
  else if (g_strcmp0 (body, "nbsp") == 0)
    ch = ' ';
  else if (body[0] == '#')
    {
      unsigned long v;
      if (body[1] == 'x' || body[1] == 'X')
        v = strtoul (body + 2, NULL, 16);
      else
        v = strtoul (body + 1, NULL, 10);
      if (v > 0 && v < 0x110000 && !(v >= 0xD800 && v <= 0xDFFF))
        ch = (gunichar) v;
    }
  g_free (body);
  if (ch != 0)
    *consumed = (semi - p) + 1;
  return ch;
}

static void
colour_html_frame_apply_open (TerminalShareColourRun *state,
                              const gchar *tag,
                              const gchar *tag_body)
{
  gchar *colour;
  gchar *style;

  if (g_strcmp0 (tag, "font") == 0)
    {
      colour = colour_tag_attr (tag_body, "color");
      if (colour != NULL)
        {
          guint8 r, g, b;
          if (colour_parse_hex (colour, &r, &g, &b))
            {
              state->fg_set = TRUE;
              state->fg_r = r;
              state->fg_g = g;
              state->fg_b = b;
            }
          g_free (colour);
        }
    }
  else if (g_strcmp0 (tag, "span") == 0)
    {
      style = colour_tag_attr (tag_body, "style");
      if (style != NULL)
        {
          colour_apply_style (state, style);
          g_free (style);
        }
    }
  else if (g_strcmp0 (tag, "b") == 0 || g_strcmp0 (tag, "strong") == 0)
    {
      state->bold = TRUE;
    }
  else if (g_strcmp0 (tag, "i") == 0 || g_strcmp0 (tag, "em") == 0)
    {
      state->italic = TRUE;
    }
  else if (g_strcmp0 (tag, "u") == 0)
    {
      state->underline = TRUE;
      style = colour_tag_attr (tag_body, "style");
      if (style != NULL)
        colour_apply_style (state, style);
      g_free (style);
    }
  else if (g_strcmp0 (tag, "s") == 0 || g_strcmp0 (tag, "strike") == 0
           || g_strcmp0 (tag, "del") == 0)
    {
      state->strikethrough = TRUE;
    }
  /* <a>, <pre> and anything else: no attribute change, text kept. */
}

gboolean
terminal_share_colour_parse_html_row (const gchar *html,
                                      gchar **text_out,
                                      TerminalShareColourRun **runs_out,
                                      gsize *n_runs_out)
{
  GArray *cells; /* HtmlCell */
  GArray *frames; /* HtmlFrame */
  GString *text;
  GArray *runs; /* TerminalShareColourRun */
  const gchar *p;
  glong col = 0;
  gsize i;

  g_return_val_if_fail (html != NULL, FALSE);
  g_return_val_if_fail (text_out != NULL, FALSE);
  g_return_val_if_fail (runs_out != NULL, FALSE);
  g_return_val_if_fail (n_runs_out != NULL, FALSE);

  cells = g_array_new (FALSE, FALSE, sizeof (HtmlCell));
  frames = g_array_new (FALSE, FALSE, sizeof (HtmlFrame));
  /* Base frame: default attributes, tag "". */
  {
    HtmlFrame base;
    memset (&base, 0, sizeof (base));
    base.tag = g_strdup ("");
    g_array_append_val (frames, base);
  }

  p = html;
  while (*p != '\0')
    {
      if (*p == '<')
        {
          const gchar *gt = strchr (p, '>');
          gchar *body, *tag_name, *space;
          gboolean closing = FALSE;
          if (gt == NULL)
            break; /* unterminated tag: stop, keep what we have */
          body = g_strndup (p + 1, gt - (p + 1));
          g_strstrip (body);
          if (body[0] == '/')
            closing = TRUE;
          tag_name = g_strdup (closing ? body + 1 : body);
          space = strchr (tag_name, ' ');
          if (space != NULL)
            *space = '\0';
          if (tag_name[0] != '\0')
            {
              gchar *lower = g_ascii_strdown (tag_name, -1);
              if (closing)
                {
                  /* Pop up to and including the matching frame.
                   * Never pop the base frame. */
                  gint j;
                  for (j = (gint) frames->len - 1; j > 0; j--)
                    {
                      HtmlFrame *f = &g_array_index (frames, HtmlFrame, j);
                      if (g_strcmp0 (f->tag, lower) == 0)
                        {
                          gint k;
                          for (k = (gint) frames->len - 1; k >= j; k--)
                            {
                              HtmlFrame *d = &g_array_index (frames, HtmlFrame, k);
                              g_free (d->tag);
                            }
                          g_array_set_size (frames, j);
                          break;
                        }
                    }
                }
              else
                {
                  HtmlFrame top = g_array_index (frames, HtmlFrame, frames->len - 1);
                  HtmlFrame nf;
                  nf.tag = g_strdup (lower);
                  nf.state = top.state;
                  colour_html_frame_apply_open (&nf.state, lower, body);
                  g_array_append_val (frames, nf);
                }
              g_free (lower);
            }
          g_free (tag_name);
          g_free (body);
          p = gt + 1;
        }
      else if (*p == '&')
        {
          gsize consumed = 0;
          gunichar ch = colour_decode_entity (p, &consumed);
          if (consumed > 0 && ch != 0)
            {
              HtmlFrame *top = &g_array_index (frames, HtmlFrame, frames->len - 1);
              HtmlCell cell;
              cell.ch = ch;
              cell.state = top->state;
              g_array_append_val (cells, cell);
              p += consumed;
            }
          else
            {
              HtmlFrame *top = &g_array_index (frames, HtmlFrame, frames->len - 1);
              HtmlCell cell;
              cell.ch = '&';
              cell.state = top->state;
              g_array_append_val (cells, cell);
              p++;
            }
        }
      else if (*p == '\n' || *p == '\r')
        {
          /* Row separator in full HTML: a single-row parse stops
           * here; trailing newlines are not content. */
          break;
        }
      else
        {
          gunichar ch = g_utf8_get_char (p);
          const gchar *next = g_utf8_next_char (p);
          HtmlFrame *top = &g_array_index (frames, HtmlFrame, frames->len - 1);
          HtmlCell cell;
          cell.ch = ch;
          cell.state = top->state;
          g_array_append_val (cells, cell);
          p = next;
        }
    }

  /* Build text + runs from cells. */
  text = g_string_new ("");
  runs = g_array_new (FALSE, FALSE, sizeof (TerminalShareColourRun));
  for (i = 0; i < cells->len; i++)
    {
      HtmlCell *cell = &g_array_index (cells, HtmlCell, i);
      gint w = colour_char_width (cell->ch);
      gchar buf[8];
      gint blen;
      if (runs->len > 0)
        {
          TerminalShareColourRun *last = &g_array_index (runs, TerminalShareColourRun, runs->len - 1);
          if (last->end_col == col
              && colour_attr_equal ((const TerminalShareColourRun *) last,
                                    &cell->state))
            {
              last->end_col = col + w;
            }
          else
            {
              TerminalShareColourRun run;
              colour_run_from_state (&run, col, col + w, &cell->state);
              g_array_append_val (runs, run);
            }
        }
      else
        {
          TerminalShareColourRun run;
          colour_run_from_state (&run, col, col + w, &cell->state);
          g_array_append_val (runs, run);
        }
      blen = g_unichar_to_utf8 (cell->ch, buf);
      g_string_append_len (text, buf, blen);
      col += w;
    }

  /* Free frames (tags only). */
  {
    gsize j;
    for (j = 0; j < frames->len; j++)
      g_free (g_array_index (frames, HtmlFrame, j).tag);
  }
  g_array_free (frames, TRUE);
  g_array_free (cells, TRUE);

  *text_out = g_string_free (text, FALSE);
  *n_runs_out = runs->len;
  *runs_out = (TerminalShareColourRun *) g_array_free (runs, FALSE);
  return TRUE;
}

/* ------------------------------------------------------------------ */
/* Plain text (authoritative text for every backend)                   */
/* ------------------------------------------------------------------ */

static gchar **
colour_plain_lines (VteTerminal *terminal, glong rows)
{
  gchar *text;
  gchar **split;
  gchar **lines;
  glong i;

  text = vte_terminal_get_text_include_trailing_spaces (terminal,
                                                          NULL, NULL,
                                                          NULL);
  split = (text != NULL) ? g_strsplit (text, "\n", -1) : NULL;
  lines = g_new0 (gchar *, rows);
  for (i = 0; i < rows; i++)
    {
      const gchar *line = (split != NULL && split[i] != NULL) ? split[i] : "";
      gsize len = strlen (line);
      if (len > 0 && line[len - 1] == '\r')
        lines[i] = g_strndup (line, len - 1);
      else
        lines[i] = g_strdup (line);
    }
  g_strfreev (split);
  g_free (text);
  return lines;
}

/* Cell width of a plain-text line, in visible cells. */
static glong
colour_line_cells (const gchar *line)
{
  const gchar *p = line;
  glong cells = 0;
  while (*p != '\0')
    {
      gunichar ch = g_utf8_get_char (p);
      cells += colour_char_width (ch);
      p = g_utf8_next_char (p);
    }
  return cells;
}

/* Does html_text agree with plain_text as a prefix (in codepoints),
 * allowing html_text to be shorter only because of trimmed trailing
 * spaces?  Returns TRUE if every codepoint of html_text matches the
 * corresponding codepoint of plain_text, and any remaining plain
 * text is all spaces. */
static gboolean
colour_text_prefix_matches (const gchar *html_text, const gchar *plain_text)
{
  const gchar *h = html_text;
  const gchar *p = plain_text;
  while (*h != '\0')
    {
      gunichar hc = g_utf8_get_char (h);
      gunichar pc;
      if (*p == '\0')
        return FALSE;
      pc = g_utf8_get_char (p);
      if (hc != pc)
        return FALSE;
      h = g_utf8_next_char (h);
      p = g_utf8_next_char (p);
    }
  while (*p != '\0')
    {
      gunichar pc = g_utf8_get_char (p);
      if (pc != ' ')
        return FALSE;
      p = g_utf8_next_char (p);
    }
  return TRUE;
}

static TerminalShareColourRun *
colour_default_runs (glong cols, gsize *n_runs_out)
{
  TerminalShareColourRun *runs = g_new0 (TerminalShareColourRun, 1);
  runs[0].start_col = 0;
  runs[0].end_col = cols;
  *n_runs_out = 1;
  return runs;
}

/* ------------------------------------------------------------------ */
/* HTML backend                                                        */
/* ------------------------------------------------------------------ */

/* Parse one row's HTML fragment and, if its text agrees with the
 * plain line, return its runs.  Otherwise return NULL (caller uses
 * a default run for that row). */
static TerminalShareColourRun *
colour_html_row_runs (const gchar *html_fragment,
                      const gchar *plain_line,
                      gsize *n_runs_out)
{
  gchar *html_text = NULL;
  TerminalShareColourRun *runs = NULL;
  gsize n_runs = 0;

  if (html_fragment == NULL)
    return NULL;
  if (!terminal_share_colour_parse_html_row (html_fragment, &html_text,
                                             &runs, &n_runs))
    {
      g_free (html_text);
      return NULL;
    }
  if (!colour_text_prefix_matches (html_text, plain_line))
    {
      g_free (html_text);
      g_free (runs);
      return NULL;
    }
  g_free (html_text);
  *n_runs_out = n_runs;
  return runs;
}

/* Split a full <pre> HTML document into row fragments.  The caller
 * frees the returned array and each fragment with g_free.  The
 * number of fragments is *n_rows_out (may be less than requested
 * rows if VTE returned fewer lines). */
static gchar **
colour_split_full_html (const gchar *full_html, glong *n_rows_out)
{
  const gchar *start, *end;
  gchar *body;
  gchar **rows;
  glong n = 0;

  *n_rows_out = 0;
  if (full_html == NULL)
    return NULL;
  start = strstr (full_html, "<pre>");
  if (start != NULL)
    start += 5;
  else
    start = full_html;
  end = strstr (start, "</pre>");
  if (end != NULL)
    body = g_strndup (start, end - start);
  else
    body = g_strdup (start);
  /* A trailing newline after the last row is a separator, not a
   * row: strip exactly the final newline(s) so we do not invent an
   * extra empty row.  Interior empty rows are preserved by split. */
  {
    gsize len = strlen (body);
    while (len > 0 && (body[len - 1] == '\n' || body[len - 1] == '\r'))
      body[--len] = '\0';
  }
  if (body[0] == '\0')
    {
      g_free (body);
      return NULL;
    }
  rows = g_strsplit (body, "\n", -1);
  g_free (body);
  while (rows[n] != NULL)
    n++;
  *n_rows_out = n;
  return rows;
}

static gboolean
colour_html_snapshot (VteTerminal *terminal,
                      glong first_visible,
                      glong cols,
                      glong rows,
                      TerminalShareColourRow *out_rows)
{
  gchar **full_fragments = NULL;
  glong n_fragments = 0;
  glong i;
  gboolean used_full = FALSE;

  if (colour_get_text_format != NULL)
    {
      gchar *full = colour_get_text_format (terminal,
                                            TERMSHARE_VTE_FORMAT_HTML);
      if (full != NULL)
        {
          full_fragments = colour_split_full_html (full, &n_fragments);
          if (full_fragments != NULL)
            used_full = TRUE;
          g_free (full);
        }
    }

  for (i = 0; i < rows; i++)
    {
      TerminalShareColourRun *runs = NULL;
      gsize n_runs = 0;
      gboolean ok = FALSE;

      if (used_full && i < n_fragments)
        {
          runs = colour_html_row_runs (full_fragments[i], out_rows[i].text,
                                       &n_runs);
          ok = (runs != NULL);
        }
      if (!ok && colour_get_text_range_format != NULL)
        {
          /* Per-row HTML, absolute row, with an end-column fallback:
           * try cols-1 first (inclusive last column); if the
           * decoded text does not match the plain line, retry with
           * cols (exclusive end).  Only one of the two can be right
           * for a full-width row, and the text check tells us which. */
          glong abs_row = first_visible + i;
          gchar *h = colour_get_text_range_format (terminal,
                                                   TERMSHARE_VTE_FORMAT_HTML,
                                                   abs_row, 0, abs_row,
                                                   cols - 1, NULL);
          if (h != NULL)
            {
              runs = colour_html_row_runs (h, out_rows[i].text, &n_runs);
              g_free (h);
              ok = (runs != NULL);
            }
          if (!ok)
            {
              h = colour_get_text_range_format (terminal,
                                                TERMSHARE_VTE_FORMAT_HTML,
                                                abs_row, 0, abs_row,
                                                cols, NULL);
              if (h != NULL)
                {
                  runs = colour_html_row_runs (h, out_rows[i].text, &n_runs);
                  g_free (h);
                  ok = (runs != NULL);
                }
            }
        }
      if (!ok)
        runs = colour_default_runs (cols, &n_runs);
      out_rows[i].runs = runs;
      out_rows[i].n_runs = n_runs;
    }

  if (full_fragments != NULL)
    g_strfreev (full_fragments);
  return TRUE;
}

/* ------------------------------------------------------------------ */
/* Attributes backend (VTE < 0.72)                                     */
/* ------------------------------------------------------------------ */

static gboolean
colour_attributes_snapshot (VteTerminal *terminal,
                            glong first_visible,
                            glong cols,
                            glong rows,
                            TerminalShareColourRow *out_rows)
{
  glong i;

  for (i = 0; i < rows; i++)
    {
      GArray *attrs;
      gchar *text;
      TerminalShareColourRun *runs = NULL;
      gsize n_runs = 0;
      glong abs_row = first_visible + i;

      attrs = g_array_new (FALSE, FALSE, sizeof (VteCharAttributes));
      /* end_col is exclusive: cols (not cols-1) covers the last
       * cell of a full-width row.  With cols-1 the last cell had
       * no attribute entry and fell back to default, so a full-width
       * coloured row rendered its last cell in default colours. */
      text = vte_terminal_get_text_range (terminal, abs_row, 0,
                                          abs_row, cols,
                                          NULL, NULL, attrs);
      if (text == NULL || attrs->len == 0)
        {
          g_free (text);
          g_array_free (attrs, TRUE);
          runs = colour_default_runs (cols, &n_runs);
          out_rows[i].runs = runs;
          out_rows[i].n_runs = n_runs;
          continue;
        }

      /* Per-cell attributes, indexed by visible column.  A cell
       * with no attribute entry stays default. */
      {
        typedef struct
        {
          gboolean has_attr;
          TerminalShareColourRun attr;
        } CellAttr;
        CellAttr *cells = g_new0 (CellAttr, cols > 0 ? cols : 1);
        gsize j;
        glong col;
        const gchar *p;

        for (j = 0; j < attrs->len; j++)
          {
            VteCharAttributes *a = &g_array_index (attrs, VteCharAttributes, j);
            glong ccol = a->column;
            if (ccol < 0 || ccol >= cols)
              continue;
            cells[ccol].has_attr = TRUE;
            cells[ccol].attr.fg_set = TRUE;
            cells[ccol].attr.bg_set = TRUE;
            cells[ccol].attr.fg_r = (guint8) (a->fore.red >> 8);
            cells[ccol].attr.fg_g = (guint8) (a->fore.green >> 8);
            cells[ccol].attr.fg_b = (guint8) (a->fore.blue >> 8);
            cells[ccol].attr.bg_r = (guint8) (a->back.red >> 8);
            cells[ccol].attr.bg_g = (guint8) (a->back.green >> 8);
            cells[ccol].attr.bg_b = (guint8) (a->back.blue >> 8);
            cells[ccol].attr.underline = a->underline ? TRUE : FALSE;
            cells[ccol].attr.strikethrough = a->strikethrough ? TRUE : FALSE;
            /* Bold/italic: NOT in VteCharAttributes.  Left FALSE,
             * never invented (see header). */
            cells[ccol].attr.bold = FALSE;
            cells[ccol].attr.italic = FALSE;
            cells[ccol].attr.reverse = FALSE;
            cells[ccol].attr.dim = FALSE;
          }

        /* Wide characters: the second cell of a wide character
         * must carry the first cell's attributes (notably the
         * background), or the remote terminal would show a
         * default-background gap in the middle of the glyph. */
        {
          glong c = 0;
          p = out_rows[i].text;
          while (*p != '\0')
            {
              gunichar ch = g_utf8_get_char (p);
              gint w = colour_char_width (ch);
              if (w == 2 && c + 1 < cols && cells[c].has_attr)
                cells[c + 1] = cells[c];
              c += w;
              p = g_utf8_next_char (p);
            }
        }

        /* Coalesce cells into runs. */
        {
          GArray *run_arr = g_array_new (FALSE, FALSE,
                                         sizeof (TerminalShareColourRun));
          for (col = 0; col < cols; col++)
            {
              TerminalShareColourRun cur;
              memset (&cur, 0, sizeof (cur));
              if (cells[col].has_attr)
                cur = cells[col].attr;
              if (run_arr->len > 0)
                {
                  TerminalShareColourRun *last = &g_array_index (run_arr, TerminalShareColourRun, run_arr->len - 1);
                  if (last->end_col == col
                      && colour_attr_equal (last, &cur))
                    {
                      last->end_col = col + 1;
                      continue;
                    }
                }
              cur.start_col = col;
              cur.end_col = col + 1;
              g_array_append_val (run_arr, cur);
            }
          n_runs = run_arr->len;
          runs = (TerminalShareColourRun *) g_array_free (run_arr, FALSE);
        }
        g_free (cells);
      }
      g_free (text);
      g_array_free (attrs, TRUE);
      out_rows[i].runs = runs;
      out_rows[i].n_runs = n_runs;
    }
  return TRUE;
}

/* ------------------------------------------------------------------ */
/* Public snapshot + SGR conversion                                    */
/* ------------------------------------------------------------------ */

gboolean
terminal_share_colour_snapshot (VteTerminal *terminal,
                                glong first_visible,
                                glong cols,
                                glong rows,
                                TerminalShareColourRow **rows_out)
{
  gchar **plain_lines;
  TerminalShareColourRow *out_rows;
  glong i;

  g_return_val_if_fail (VTE_IS_TERMINAL (terminal), FALSE);
  g_return_val_if_fail (cols >= 1 && rows >= 1, FALSE);
  g_return_val_if_fail (rows_out != NULL, FALSE);

  colour_probe ();

  plain_lines = colour_plain_lines (terminal, rows);
  out_rows = g_new0 (TerminalShareColourRow, rows);
  for (i = 0; i < rows; i++)
    out_rows[i].text = plain_lines[i];
  g_free (plain_lines);

  if (colour_backend == TERMSHARE_COLOUR_BACKEND_HTML)
    {
      colour_html_snapshot (terminal, first_visible, cols, rows, out_rows);
    }
  else if (colour_backend == TERMSHARE_COLOUR_BACKEND_ATTRIBUTES)
    {
      colour_attributes_snapshot (terminal, first_visible, cols, rows,
                                  out_rows);
    }
  else
    {
      for (i = 0; i < rows; i++)
        {
          gsize n_runs = 0;
          out_rows[i].runs = colour_default_runs (cols, &n_runs);
          out_rows[i].n_runs = n_runs;
        }
    }

  *rows_out = out_rows;
  return TRUE;
}

void
terminal_share_colour_snapshot_free (TerminalShareColourRow *rows,
                                     glong n_rows)
{
  glong i;
  if (rows == NULL)
    return;
  for (i = 0; i < n_rows; i++)
    {
      g_free (rows[i].text);
      g_free (rows[i].runs);
    }
  g_free (rows);
}

/* Find the run covering column col, or NULL for default.  Runs are
 * sorted and non-overlapping; the search is a linear walk because
 * rows are short and run changes are rare, and the caller walks
 * columns in order, so we keep a hint via *idx. */
static const TerminalShareColourRun *
colour_run_at (const TerminalShareColourRow *row, glong col, gsize *idx)
{
  while (*idx < row->n_runs && row->runs[*idx].end_col <= col)
    (*idx)++;
  if (*idx < row->n_runs && row->runs[*idx].start_col <= col
      && col < row->runs[*idx].end_col)
    return &row->runs[*idx];
  return NULL;
}

static void
colour_append_run_sgr (GString *out, const TerminalShareColourRun *run)
{
  GString *params = g_string_new ("");
  if (run->fg_set)
    g_string_append_printf (params, "%s38;2;%u;%u;%u",
                            params->len ? ";" : "",
                            run->fg_r, run->fg_g, run->fg_b);
  if (run->bg_set)
    g_string_append_printf (params, "%s48;2;%u;%u;%u",
                            params->len ? ";" : "",
                            run->bg_r, run->bg_g, run->bg_b);
  if (run->bold)
    g_string_append_printf (params, "%s1", params->len ? ";" : "");
  if (run->italic)
    g_string_append_printf (params, "%s3", params->len ? ";" : "");
  if (run->underline)
    g_string_append_printf (params, "%s4", params->len ? ";" : "");
  if (run->strikethrough)
    g_string_append_printf (params, "%s9", params->len ? ";" : "");
  if (run->reverse)
    g_string_append_printf (params, "%s7", params->len ? ";" : "");
  if (run->dim)
    g_string_append_printf (params, "%s2", params->len ? ";" : "");
  /* The caller already emitted \e[0m before calling this for a run
   * change, so a default run needs nothing more. */
  if (params->len > 0)
    g_string_append_printf (out, "\e[%sm", params->str);
  g_string_free (params, TRUE);
}

gchar *
terminal_share_colour_row_to_sgr (const TerminalShareColourRow *row,
                                  glong cols)
{
  GString *out;
  const gchar *p;
  glong col = 0;
  gsize idx = 0;
  const TerminalShareColourRun *cur = NULL;
  gboolean have_cur = FALSE;

  (void) cols;
  g_return_val_if_fail (row != NULL, NULL);
  g_return_val_if_fail (row->text != NULL, NULL);

  out = g_string_new (""); /* first run change adds the leading reset */
  p = row->text;
  while (*p != '\0')
    {
      gunichar ch = g_utf8_get_char (p);
      const gchar *next = g_utf8_next_char (p);
      const TerminalShareColourRun *run = colour_run_at (row, col, &idx);
      gint w = colour_char_width (ch);

      if (!have_cur || run != cur
          || (run == NULL && cur != NULL)
          || (run != NULL && cur == NULL)
          || (run != NULL && cur != NULL
              && !colour_attr_equal (run, cur)))
        {
          /* Run change (including to/from default): reset, then
           * the new run's SGR if it is not default. */
          g_string_append (out, "\e[0m");
          if (run != NULL)
            colour_append_run_sgr (out, run);
          cur = run;
          have_cur = TRUE;
        }

      /* Control characters other than printable text would corrupt
       * the wire protocol (tab/newline delimit fields/lines) or
       * inject escapes, so render them as spaces.  Plain VTE text
       * should not contain them, but never trust that here. */
      if (ch < 0x20 || ch == 0x7f)
        {
          g_string_append_c (out, ' ');
        }
      else
        {
          g_string_append_len (out, p, next - p);
        }
      col += w;
      p = next;
    }
  g_string_append (out, "\e[0m");
  return g_string_free (out, FALSE);
}
