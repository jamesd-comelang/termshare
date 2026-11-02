/*-
 * Copyright (c) 2005-2006 Benedikt Meurer <benny@xfce.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef TERMINAL_UTIL_H
#define TERMINAL_UTIL_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

void
terminal_util_show_about_dialog (GtkWindow *parent);

/* Plain-GTK menu separator, replacing xfce_gtk_menu_append_separator
 * (added in libxfce4ui 4.17.3, absent in Jammy's 4.16.1) so the binary
 * does not link a 4.18-only symbol. Behaviour is identical: a visible
 * separator line appended to the menu. */
void
terminal_menu_append_separator (GtkMenuShell *menu_shell);

void
terminal_util_activate_window (GtkWindow *window);

void
terminal_util_free_data (gpointer data,
                         GClosure *closure);

gchar *
terminal_util_get_process_cwd (GPid pid);

G_END_DECLS

#endif /* !TERMINAL_UTIL_H */
