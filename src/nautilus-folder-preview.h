/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct
{
    GdkTexture *texture;
    gboolean is_video;
    gboolean is_opaque;
} NautilusFolderPreviewTile;

void        nautilus_folder_preview_load_async  (GFile               *folder,
                                                 guint64              folder_mtime,
                                                 GCancellable        *cancellable,
                                                 GAsyncReadyCallback  callback,
                                                 gpointer             user_data);

/* Returns: (transfer full) (element-type NautilusFolderPreviewTile): possibly empty */
GPtrArray  *nautilus_folder_preview_load_finish (GAsyncResult        *result,
                                                 GError             **error);

void        nautilus_folder_preview_snapshot    (GtkSnapshot         *snapshot,
                                                 GPtrArray           *tiles,
                                                 float                width,
                                                 float                height,
                                                 int                  scale_factor);

G_END_DECLS
