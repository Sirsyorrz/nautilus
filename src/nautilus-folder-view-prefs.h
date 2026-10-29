/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

gboolean nautilus_folder_view_prefs_lookup  (GFile *location,
                                             guint *view_id,
                                             int   *zoom_level);
gboolean nautilus_folder_view_prefs_has_own (GFile *location);
void     nautilus_folder_view_prefs_save    (GFile *location,
                                             guint  view_id,
                                             int    zoom_level);
void     nautilus_folder_view_prefs_reset   (GFile *location);

G_END_DECLS
