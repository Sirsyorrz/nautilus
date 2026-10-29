/*
 * Per-folder view mode and zoom level. A folder without its own entry
 * inherits from the nearest ancestor that has one.
 *
 * Stored in a key file rather than gvfs metadata so ancestor lookups stay
 * synchronous and work for any URI scheme.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nautilus-folder-view-prefs.h"

#include "nautilus-view-info.h"

#define KEY_VIEW "view"
#define KEY_ZOOM "zoom"

static GKeyFile *store = NULL;
static char *store_path = NULL;

static GKeyFile *
get_store (void)
{
    if (store == NULL)
    {
        store = g_key_file_new ();
        store_path = g_build_filename (g_get_user_config_dir (), "nautilus", "folder-views.ini", NULL);
        g_key_file_load_from_file (store, store_path, G_KEY_FILE_NONE, NULL);
    }

    return store;
}

static void
write_store (void)
{
    g_autoptr (GError) error = NULL;
    g_autofree char *dir = g_path_get_dirname (store_path);

    g_mkdir_with_parents (dir, 0700);
    if (!g_key_file_save_to_file (store, store_path, &error))
    {
        g_warning ("Failed to save folder view settings: %s", error->message);
    }
}

static gboolean
read_entry (GKeyFile   *keyfile,
            const char *uri,
            guint      *view_id,
            int        *zoom_level)
{
    if (!g_key_file_has_group (keyfile, uri))
    {
        return FALSE;
    }

    guint saved_view_id = g_key_file_get_integer (keyfile, uri, KEY_VIEW, NULL);
    if (saved_view_id != NAUTILUS_VIEW_LIST_ID && saved_view_id != NAUTILUS_VIEW_GRID_ID)
    {
        return FALSE;
    }

    *view_id = saved_view_id;
    *zoom_level = g_key_file_get_integer (keyfile, uri, KEY_ZOOM, NULL);
    return TRUE;
}

gboolean
nautilus_folder_view_prefs_lookup (GFile *location,
                                   guint *view_id,
                                   int   *zoom_level)
{
    GKeyFile *keyfile = get_store ();
    g_autoptr (GFile) dir = g_object_ref (location);

    while (dir != NULL)
    {
        g_autofree char *uri = g_file_get_uri (dir);

        if (read_entry (keyfile, uri, view_id, zoom_level))
        {
            return TRUE;
        }

        GFile *parent = g_file_get_parent (dir);
        g_object_unref (dir);
        dir = parent;
    }

    return FALSE;
}

gboolean
nautilus_folder_view_prefs_has_own (GFile *location)
{
    g_autofree char *uri = g_file_get_uri (location);

    return g_key_file_has_group (get_store (), uri);
}

void
nautilus_folder_view_prefs_save (GFile *location,
                                 guint  view_id,
                                 int    zoom_level)
{
    GKeyFile *keyfile = get_store ();
    g_autofree char *uri = g_file_get_uri (location);

    g_key_file_set_integer (keyfile, uri, KEY_VIEW, view_id);
    g_key_file_set_integer (keyfile, uri, KEY_ZOOM, zoom_level);
    write_store ();
}

void
nautilus_folder_view_prefs_reset (GFile *location)
{
    GKeyFile *keyfile = get_store ();
    g_autofree char *uri = g_file_get_uri (location);

    if (g_key_file_remove_group (keyfile, uri, NULL))
    {
        write_store ();
    }
}
