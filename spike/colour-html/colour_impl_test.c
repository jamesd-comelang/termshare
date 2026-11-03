/* colour_impl_test.c -- test the production colour abstraction layer.
 *
 * NOT production code, test only.  Creates a VteTerminal with no
 * child process, feeds known coloured ANSI with vte_terminal_feed(),
 * then calls terminal_share_colour_snapshot() -- the same function
 * the sharing server uses -- and checks the runs and the SGR output.
 *
 * Run under xvfb:  xvfb-run -a ./colour_impl_test
 * Exit status: 0 if all hard checks pass, else 1.  Soft checks
 * (dim, scrolled-up) print NOTE and do not fail the run, because
 * they are known open questions, not regressions.
 *
 * Backend expectation: on VTE >= 0.72 this must select "html"; on
 * VTE < 0.72 (Jammy 0.68) it must select "attributes" (dlsym fails)
 * and colour checks must still pass for colours (bold/italic are
 * expected to be absent there, and are checked accordingly).
 */
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include <vte/vte.h>

#include "terminal-share-colour.h"

static int failures = 0;
static int notes = 0;

static void
pump (void)
{
  int i;
  for (i = 0; i < 50; i++)
    {
      while (g_main_context_iteration (NULL, FALSE))
        ;
      g_usleep (10000);
    }
}

static void
check (int ok, const char *label)
{
  printf ("%s: %s\n", ok ? "PASS" : "FAIL", label);
  fflush (stdout);
  if (!ok)
    failures++;
}

static void
note (const char *label)
{
  printf ("NOTE: %s\n", label);
  fflush (stdout);
  notes++;
}

static const TerminalShareColourRow *
find_row_text (TerminalShareColourRow *rows, glong n_rows, const char *needle)
{
  glong i;
  for (i = 0; i < n_rows; i++)
    if (rows[i].text != NULL && strstr (rows[i].text, needle) != NULL)
      return &rows[i];
  return NULL;
}

static int
run_has_fg (const TerminalShareColourRow *row, guint8 r, guint8 g, guint8 b)
{
  gsize i;
  for (i = 0; i < row->n_runs; i++)
    if (row->runs[i].fg_set && row->runs[i].fg_r == r
        && row->runs[i].fg_g == g && row->runs[i].fg_b == b)
      return 1;
  return 0;
}

static int
run_fg_dominant (const TerminalShareColourRow *row, char channel)
{
  gsize i;
  for (i = 0; i < row->n_runs; i++)
    {
      if (!row->runs[i].fg_set)
        continue;
      if (channel == 'r' && row->runs[i].fg_r > row->runs[i].fg_g
          && row->runs[i].fg_r > row->runs[i].fg_b
          && row->runs[i].fg_r > 150)
        return 1;
      if (channel == 'g' && row->runs[i].fg_g > row->runs[i].fg_r
          && row->runs[i].fg_g > row->runs[i].fg_b
          && row->runs[i].fg_g > 150)
        return 1;
      if (channel == 'b' && row->runs[i].fg_b > row->runs[i].fg_r
          && row->runs[i].fg_b > row->runs[i].fg_g
          && row->runs[i].fg_b > 150)
        return 1;
    }
  return 0;
}

static int
run_has_bg_dominant (const TerminalShareColourRow *row, char channel)
{
  gsize i;
  for (i = 0; i < row->n_runs; i++)
    {
      if (!row->runs[i].bg_set)
        continue;
      if (channel == 'r' && row->runs[i].bg_r > row->runs[i].bg_g
          && row->runs[i].bg_r > row->runs[i].bg_b
          && row->runs[i].bg_r > 150)
        return 1;
    }
  return 0;
}

static int
run_has_flag (const TerminalShareColourRow *row, const char *flag)
{
  gsize i;
  for (i = 0; i < row->n_runs; i++)
    {
      if (strcmp (flag, "bold") == 0 && row->runs[i].bold)
        return 1;
      if (strcmp (flag, "italic") == 0 && row->runs[i].italic)
        return 1;
      if (strcmp (flag, "underline") == 0 && row->runs[i].underline)
        return 1;
      if (strcmp (flag, "strikethrough") == 0 && row->runs[i].strikethrough)
        return 1;
    }
  return 0;
}

static void
print_row (const char *label, const TerminalShareColourRow *row)
{
  gsize i;
  printf ("--- %s text=[%s] n_runs=%zu ---\n", label,
          row->text ? row->text : "(null)", row->n_runs);
  for (i = 0; i < row->n_runs; i++)
    {
      TerminalShareColourRun *r = &row->runs[i];
      printf ("  run %ld-%ld fg=%s#%02X%02X%02X bg=%s#%02X%02X%02X "
              "b=%d i=%d u=%d s=%d\n",
              r->start_col, r->end_col,
              r->fg_set ? "" : "(def)", r->fg_r, r->fg_g, r->fg_b,
              r->bg_set ? "" : "(def)", r->bg_r, r->bg_g, r->bg_b,
              r->bold, r->italic, r->underline, r->strikethrough);
    }
  {
    gchar *sgr = terminal_share_colour_row_to_sgr (row, 40);
    printf ("  sgr=[%s]\n", sgr ? sgr : "(null)");
    g_free (sgr);
  }
  fflush (stdout);
}

int
main (int argc, char **argv)
{
  GtkWidget *window, *term_widget;
  VteTerminal *term;
  GtkAdjustment *adj;
  glong cols, rows, first_visible;
  TerminalShareColourRow *snap = NULL;
  const char *backend;
  int is_html;

  gtk_init (&argc, &argv);
  window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  term_widget = vte_terminal_new ();
  term = VTE_TERMINAL (term_widget);
  vte_terminal_set_size (term, 40, 12);
  vte_terminal_set_scrollback_lines (term, 100);
  gtk_container_add (GTK_CONTAINER (window), term_widget);
  gtk_widget_show_all (window);
  pump ();

  /* Feed 12 rows, one per visible row, each SGR-reset at the end. */
  {
    const char *feed[] = {
      "plain",
      "\x1b[31mRED\x1b[0m plain",
      "\x1b[1;32mBOLDGREEN\x1b[0m",
      "\x1b[38;5;196mPAL196\x1b[0m \x1b[38;2;10;20;30mTRUE\x1b[0m",
      "\x1b[41mBGRED\x1b[0m \x1b[4mUNDER\x1b[0m \x1b[3mITAL\x1b[0m",
      "\x1b[31mRED   \x1b[0m",
      "\x1b[33m\xe4\xb8\xad\xe6\x96\x87\x1b[0m",
      "\x1b[9mSTRIKE\x1b[0m \x1b[7mREV\x1b[0m \x1b[2mDIM\x1b[0m",
      "ABC   ",
      "\x1b]8;;https://example.com\x1b\\LINK\x1b]8;;\x1b\\",
      "1234567890123456789012345678901234567890",
      "\x1b[31mFULLRED123456789012345678901234567890123\x1b[0m",
    };
    int i;
    for (i = 0; i < 12; i++)
      {
        vte_terminal_feed (term, feed[i], -1);
        if (i < 11)
          vte_terminal_feed (term, "\r\n", -1);
      }
    pump ();
  }

  cols = vte_terminal_get_column_count (term);
  rows = vte_terminal_get_row_count (term);
  printf ("cols=%ld rows=%ld\n", cols, rows);
  adj = gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (term));
  first_visible = adj ? (glong) gtk_adjustment_get_value (adj) : 0;
  printf ("first_visible=%ld\n", first_visible);

  if (!terminal_share_colour_snapshot (term, first_visible, cols, rows, &snap))
    {
      printf ("FAIL: snapshot returned FALSE\n");
      return 1;
    }
  backend = terminal_share_colour_backend_name ();
  printf ("backend=%s\n", backend);
  is_html = strcmp (backend, "html") == 0;
  check (is_html || strcmp (backend, "attributes") == 0,
         "backend is html or attributes (not plain/unknown)");

  {
    const TerminalShareColourRow *r;
    gchar *sgr;
    glong i;
    for (i = 0; i < rows; i++)
      {
        char label[32];
        snprintf (label, sizeof (label), "row %ld", i);
        print_row (label, &snap[i]);
      }

    r = find_row_text (snap, rows, "RED");
    check (r != NULL, "red row found");
    if (r != NULL)
      {
        if (is_html)
          check (run_has_fg (r, 0xC0, 0x00, 0x00),
                 "red fg is #C00000 on HTML backend");
        else
          check (run_fg_dominant (r, 'r'), "red fg is red-dominant");
      }
    r = find_row_text (snap, rows, "BOLDGREEN");
    check (r != NULL, "bold-green row found");
    if (r != NULL)
      {
        check (run_fg_dominant (r, 'g'), "bold-green fg green-dominant");
        if (is_html)
          check (run_has_flag (r, "bold"), "bold flag set (HTML)");
        else
          note ("bold flag not expected on attributes backend (not in VteCharAttributes)");
      }
    r = find_row_text (snap, rows, "PAL196");
    check (r != NULL && run_fg_dominant (r, 'r'), "palette 196 red-dominant");
    r = find_row_text (snap, rows, "TRUE");
    check (r != NULL && run_has_fg (r, 10, 20, 30),
           "truecolor fg is exactly 10,20,30");
    r = find_row_text (snap, rows, "BGRED");
    check (r != NULL && run_has_bg_dominant (r, 'r'), "bg red dominant");
    r = find_row_text (snap, rows, "UNDER");
    check (r != NULL && run_has_flag (r, "underline"), "underline flag set");
    r = find_row_text (snap, rows, "ITAL");
    check (r != NULL, "italic row found");
    if (r != NULL && is_html)
      check (run_has_flag (r, "italic"), "italic flag set (HTML)");
    r = find_row_text (snap, rows, "STRIKE");
    check (r != NULL && run_has_flag (r, "strikethrough"),
           "strikethrough flag set");
    r = find_row_text (snap, rows, "REV");
    if (r != NULL)
      {
        /* Reverse must not vanish: either swapped colours or a
         * reverse flag.  Plain default runs would mean it was lost. */
        int any = 0;
        gsize j;
        for (j = 0; j < r->n_runs; j++)
          if (r->runs[j].fg_set || r->runs[j].bg_set || r->runs[j].reverse)
            any = 1;
        check (any, "reverse row is not plain default (captured somehow)");
      }
    else
      check (0, "reverse row found");
    r = find_row_text (snap, rows, "DIM");
    if (r != NULL)
      {
        int any = 0;
        gsize j;
        for (j = 0; j < r->n_runs; j++)
          if (r->runs[j].fg_set || r->runs[j].bg_set || r->runs[j].dim)
            any = 1;
        if (any)
          printf ("PASS: dim row captured somehow\n");
        else
          note ("dim row is plain default in this VTE output (dim not captured; known open item)");
      }
    r = find_row_text (snap, rows, "LINK");
    check (r != NULL, "hyperlink text LINK present");
    if (r != NULL)
      {
        sgr = terminal_share_colour_row_to_sgr (r, 40);
        check (sgr != NULL && strstr (sgr, "<a") == NULL
               && strstr (sgr, "LINK") != NULL,
               "hyperlink tag stripped, text kept in SGR output");
        g_free (sgr);
      }
    r = find_row_text (snap, rows, "1234567890");
    check (r != NULL, "full-width digit row found");
    if (r != NULL)
      {
        glong cells = 0;
        const gchar *p = r->text;
        while (*p)
          {
            cells += g_unichar_iswide (g_utf8_get_char (p)) ? 2 : 1;
            p = g_utf8_next_char (p);
          }
        check (cells == 40, "full-width digit row is 40 cells");
        check (r->n_runs >= 1 && r->runs[r->n_runs - 1].end_col >= 40
               , "full-width row runs reach column 40");
      }
    r = find_row_text (snap, rows, "FULLRED");
    check (r != NULL, "full-width coloured row found");
    if (r != NULL)
      {
        /* FULLRED (7) + 33 digits = 40 cells, full width. */
        glong cells = 0;
        const gchar *p = r->text;
        while (*p)
          {
            cells += g_unichar_iswide (g_utf8_get_char (p)) ? 2 : 1;
            p = g_utf8_next_char (p);
          }
        printf ("FULLRED row cells=%ld text=[%s]\n", cells, r->text);
        check (cells == 40, "full-width coloured row is 40 cells");
        check (run_fg_dominant (r, 'r'), "full-width coloured row fg red-dominant");
      }
    /* SGR determinism + framing for every row. */
    for (i = 0; i < rows; i++)
      {
        gchar *a = terminal_share_colour_row_to_sgr (&snap[i], 40);
        gchar *b = terminal_share_colour_row_to_sgr (&snap[i], 40);
        int ok = a != NULL && b != NULL && strcmp (a, b) == 0
            && strncmp (a, "\e[0m", 4) == 0
            && strlen (a) >= 4 && strcmp (a + strlen (a) - 4, "\e[0m") == 0
            && strchr (a, '\t') == NULL && strchr (a, '\n') == NULL;
        if (!ok)
          printf ("FAIL: SGR framing/determinism row %ld sgr=[%s]\n", i, a ? a : "(null)");
        else if (i == 0)
          printf ("PASS: SGR framing/determinism (all rows checked)\n");
        if (!ok)
          failures++;
        g_free (a);
        g_free (b);
      }
  }
  terminal_share_colour_snapshot_free (snap, rows);
  snap = NULL;

  /* Scroll test: feed 5 more lines, visible rows should be the
   * newest, not the oldest, when first_visible is honoured. */
  {
    int i;
    for (i = 0; i < 5; i++)
      {
        char buf[32];
        snprintf (buf, sizeof (buf), "\r\nSCROLL%d", i);
        vte_terminal_feed (term, buf, -1);
      }
    pump ();
    adj = gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (term));
    first_visible = adj ? (glong) gtk_adjustment_get_value (adj) : 0;
    printf ("after scroll first_visible=%ld\n", first_visible);
    if (!terminal_share_colour_snapshot (term, first_visible, cols, rows, &snap))
      {
        check (0, "scroll snapshot returned TRUE");
      }
    else
      {
        const TerminalShareColourRow *r = find_row_text (snap, rows, "SCROLL4");
        check (r != NULL, "scrolled snapshot shows newest SCROLL4 (offset honoured)");
        r = find_row_text (snap, rows, "plain");
        /* "plain" as a whole first row should no longer be visible
         * if we scrolled past it; it may appear as a substring of
         * other rows (e.g. "RED plain" is gone too).  Check the
         * first visible row is not the original "plain" alone. */
        if (snap[0].text != NULL && strcmp (snap[0].text, "plain") == 0)
          check (0, "first visible row after scroll is not original plain");
        else
          printf ("PASS: first visible row after scroll is not original plain\n");
        terminal_share_colour_snapshot_free (snap, rows);
        snap = NULL;
      }
  }

  /* Alt screen: enter, feed coloured text, snapshot should show
   * it; exit, snapshot should show the main screen again. */
  {
    vte_terminal_feed (term, "\x1b[?1049h", -1);
    pump ();
    vte_terminal_feed (term, "\x1b[31mALTRED\x1b[0m", -1);
    pump ();
    adj = gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (term));
    first_visible = adj ? (glong) gtk_adjustment_get_value (adj) : 0;
    if (terminal_share_colour_snapshot (term, first_visible, cols, rows, &snap))
      {
        const TerminalShareColourRow *r = find_row_text (snap, rows, "ALTRED");
        check (r != NULL, "alt-screen coloured row visible in snapshot");
        if (r != NULL)
          check (run_fg_dominant (r, 'r'), "alt-screen row fg red-dominant");
        terminal_share_colour_snapshot_free (snap, rows);
        snap = NULL;
      }
    else
      check (0, "alt-screen snapshot returned TRUE");
    vte_terminal_feed (term, "\x1b[?1049l", -1);
    pump ();
    adj = gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (term));
    first_visible = adj ? (glong) gtk_adjustment_get_value (adj) : 0;
    if (terminal_share_colour_snapshot (term, first_visible, cols, rows, &snap))
      {
        const TerminalShareColourRow *r = find_row_text (snap, rows, "SCROLL4");
        check (r != NULL, "main screen restored after alt-screen exit");
        terminal_share_colour_snapshot_free (snap, rows);
        snap = NULL;
      }
    else
      check (0, "post-alt snapshot returned TRUE");
  }

  printf ("COLOUR IMPL TEST RESULT: %s (%d failures, %d notes)\n",
          failures == 0 ? "PASS" : "FAIL", failures, notes);
  return failures == 0 ? 0 : 1;
}
