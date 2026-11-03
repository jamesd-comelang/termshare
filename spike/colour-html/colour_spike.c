/* colour_spike.c — termshare colour spike, NOT production code.
 *
 * Question: can VTE's HTML text export give termshare coloured cells
 * for tabs that already exist (no PTY tee, no restart)?
 *
 * Method: create a VteTerminal with no child process, feed it known
 * coloured ANSI with vte_terminal_feed() (documented: "Interprets data
 * as if it were data received from a child process"), then read it
 * back three ways and print everything verbatim:
 *   1. full visible text as HTML: vte_terminal_get_text_format(HTML)
 *   2. per-row HTML: vte_terminal_get_text_range_format(HTML, r, 0, r, end)
 *   3. the same per-row call resolved at runtime with dlsym()
 *
 * The dlsym path is the one production code would have to use: the
 * shipping binary is built on Ubuntu 24.04 (VTE 0.76, function exists)
 * but must run on Ubuntu 22.04 Jammy (VTE 0.68, function does NOT
 * exist). Linking the symbol directly would make the binary fail to
 * load on Jammy. The direct calls in this file exist ONLY to compare
 * against the dlsym call on a machine where the symbol is present;
 * production code must NOT copy them.
 *
 * Run under xvfb:  xvfb-run -a ./colour_spike
 * Exit status: 0 if HTML was produced and contains a colour span for
 * the known red row, else 1 (a failed spike is a finding, not a crash).
 */
#include <dlfcn.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include <vte/vte.h>

typedef char *(*GetTextRangeFormatFn) (VteTerminal *, VteFormat,
                                       glong, glong, glong, glong,
                                       gsize *);

static void
pump (void)
{
  /* VTE processes fed data on the main loop. Pump it a few times. */
  for (int i = 0; i < 50; i++)
    {
      while (g_main_context_iteration (NULL, FALSE))
        ;
      g_usleep (10000);
    }
}

static void
print_labelled (const char *label, const char *s)
{
  printf ("--- %s ---\n", label);
  if (s == NULL)
    printf ("(NULL)\n");
  else
    printf ("%s\n", s);
  fflush (stdout);
}

int
main (int argc, char **argv)
{
  GtkWidget *window, *term_widget;
  VteTerminal *term;
  char *html_full, *html_text;
  int failures = 0;

  gtk_init (&argc, &argv);

  window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  term_widget = vte_terminal_new ();
  term = VTE_TERMINAL (term_widget);
  /* Small grid so row numbers are easy to read in the output. */
  vte_terminal_set_size (term, 40, 8);
  vte_terminal_set_scrollback_lines (term, 100);
  gtk_container_add (GTK_CONTAINER (window), term_widget);
  gtk_widget_show_all (window);
  pump ();

  /* Rows 0-7, each fed separately so SGR state cannot leak silently
   * across the test: every coloured run is explicitly reset. */
  const char *rows[] = {
    "plain",
    "\x1b[31mRED\x1b[0m plain",
    "\x1b[1;32mBOLDGREEN\x1b[0m",
    "\x1b[38;5;196mPAL196\x1b[0m \x1b[38;2;10;20;30mTRUE\x1b[0m",
    "\x1b[41mBGRED\x1b[0m \x1b[4mUNDER\x1b[0m \x1b[3mITAL\x1b[0m",
    "\x1b[31mRED   \x1b[0m",
    "\x1b[33m\xe4\xb8\xad\xe6\x96\x87\x1b[0m",
    "A\x1b[39mB\x1b[49mC",
  };
  for (int i = 0; i < 8; i++)
    {
      vte_terminal_feed (term, rows[i], -1);
      if (i < 7)
        vte_terminal_feed (term, "\r\n", -1);
    }
  pump ();

  printf ("cols=%ld rows=%ld\n",
          vte_terminal_get_column_count (term),
          vte_terminal_get_row_count (term));
  {
    glong ccol, crow;
    vte_terminal_get_cursor_position (term, &ccol, &crow);
    printf ("cursor col=%ld abs_row=%ld\n", ccol, crow);
  }

  /* 1. Full visible text, TEXT and HTML. */
  html_text = vte_terminal_get_text_format (term, VTE_FORMAT_TEXT);
  print_labelled ("FULL TEXT", html_text);
  html_full = vte_terminal_get_text_format (term, VTE_FORMAT_HTML);
  print_labelled ("FULL HTML", html_full);
  if (html_full == NULL)
    {
      printf ("FAIL: full HTML is NULL\n");
      failures++;
    }
  if (html_full == NULL || strstr (html_full, "RED") == NULL)
    {
      printf ("FAIL: full HTML does not contain RED\n");
      failures++;
    }
  /* Colour evidence: any hex colour or span at all. */
  if (html_full != NULL
      && strstr (html_full, "<span") == NULL
      && strstr (html_full, "<font") == NULL)
    {
      printf ("FAIL: full HTML contains no span/font markup\n");
      failures++;
    }

  /* 2. Per-row HTML, two end-column conventions, to settle whether
   * end_col is inclusive or exclusive. Row 1 is "RED plain": with an
   * inclusive end (cols-1 = 39) the row must still contain "plain";
   * with an exclusive end (cols = 40) it must too, but one of the two
   * may drop the last cell. Print both, compare by eye in CI logs. */
  for (int r = 0; r < 8; r++)
    {
      char label[64], *h;
      gsize len = 0;
      snprintf (label, sizeof (label), "ROW %d HTML end_col=39", r);
      h = vte_terminal_get_text_range_format (term, VTE_FORMAT_HTML,
                                              r, 0, r, 39, &len);
      print_labelled (label, h);
      g_free (h);
      snprintf (label, sizeof (label), "ROW %d HTML end_col=40", r);
      h = vte_terminal_get_text_range_format (term, VTE_FORMAT_HTML,
                                              r, 0, r, 40, &len);
      print_labelled (label, h);
      g_free (h);
    }

  /* 3. Same call via dlsym — the production-safe path. */
  {
    void *handle = dlopen ("libvte-2.91.so.0", RTLD_NOW | RTLD_GLOBAL);
    GetTextRangeFormatFn via_dlsym = NULL;
    if (handle != NULL)
      via_dlsym = (GetTextRangeFormatFn) dlsym (handle,
                                                "vte_terminal_get_text_range_format");
    printf ("dlsym: handle=%p fn=%p\n", handle, (void *) via_dlsym);
    if (via_dlsym == NULL)
      {
        printf ("NOTE: dlsym did not resolve the symbol on this machine "
                "(expected on VTE < 0.72, e.g. Jammy 0.68); "
                "production code must then fall back to the old "
                "attributes API.\n");
      }
    else
      {
        char *h = via_dlsym (term, VTE_FORMAT_HTML, 1, 0, 1, 39, NULL);
        print_labelled ("ROW 1 HTML via dlsym", h);
        if (h == NULL || strstr (h, "RED") == NULL)
          {
            printf ("FAIL: dlsym row HTML missing RED\n");
            failures++;
          }
        g_free (h);
      }
  }

  /* 4. Scrollback / row-coordinate probe: feed 5 more lines into the
   * 8-row terminal, then ask for rows 0..7 again. If row 0 now reads
   * the first fed line ("plain"), rows are absolute (scrollback
   * included) and termshare must offset by the first visible row, as
   * it already does for the cursor. If row 0 reads a later line, rows
   * are visible-relative and no offset is needed. */
  for (int i = 0; i < 5; i++)
    {
      char buf[32];
      snprintf (buf, sizeof (buf), "\r\nSCROLL%d", i);
      vte_terminal_feed (term, buf, -1);
    }
  pump ();
  {
    char *h = vte_terminal_get_text_range_format (term, VTE_FORMAT_HTML,
                                                  0, 0, 0, 39, NULL);
    print_labelled ("AFTER SCROLL ROW 0 HTML", h);
    g_free (h);
    h = vte_terminal_get_text_format (term, VTE_FORMAT_HTML);
    print_labelled ("AFTER SCROLL FULL HTML", h);
    g_free (h);
  }

  g_free (html_text);
  g_free (html_full);
  printf ("SPIKE RESULT: %s (%d failures)\n",
          failures == 0 ? "PASS" : "FAIL", failures);
  return failures == 0 ? 0 : 1;
}
