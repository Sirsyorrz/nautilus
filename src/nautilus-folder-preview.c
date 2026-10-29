/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Dolphin-style folder previews: a few child thumbnails drawn on top of the
 * folder icon. Layout constants mirror kio-extras' thumbForDirectory().
 */

#define G_LOG_DOMAIN "nautilus-folder-preview"

#include "nautilus-folder-preview.h"

#include "nautilus-global-preferences.h"
#include "nautilus-hash-queue.h"
#include "nautilus-mime-actions.h"
#include "nautilus-thumbnails.h"
#include "nautilus-ui-utilities.h"

#include <math.h>

#define MAX_TILES 4
#define MAX_ENUMERATED_CHILDREN 500
#define CACHE_COUNT_LIMIT 64
#define MIN_VIDEO_FRAME_WIDTH 40

#define CHILD_ATTRIBUTES \
    G_FILE_ATTRIBUTE_STANDARD_NAME "," \
    G_FILE_ATTRIBUTE_STANDARD_TYPE "," \
    G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN "," \
    G_FILE_ATTRIBUTE_STANDARD_IS_BACKUP "," \
    G_FILE_ATTRIBUTE_STANDARD_SIZE "," \
    G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE "," \
    G_FILE_ATTRIBUTE_TIME_MODIFIED "," \
    G_FILE_ATTRIBUTE_THUMBNAIL_PATH "," \
    G_FILE_ATTRIBUTE_THUMBNAIL_IS_VALID "," \
    G_FILE_ATTRIBUTE_THUMBNAILING_FAILED

static void
tile_free (NautilusFolderPreviewTile *tile)
{
    g_clear_object (&tile->texture);
    g_free (tile);
}

typedef struct
{
    GFile *folder;
    guint64 mtime;
    GPtrArray *tiles;
} CacheItem;

static void
cache_item_free (CacheItem *item)
{
    g_clear_object (&item->folder);
    g_clear_pointer (&item->tiles, g_ptr_array_unref);
    g_free (item);
}

static NautilusHashQueue *preview_cache;

static GPtrArray *
cache_lookup (GFile   *folder,
              guint64  mtime)
{
    if (preview_cache == NULL)
    {
        return NULL;
    }

    CacheItem *item = nautilus_hash_queue_find_item (preview_cache, folder);

    if (item == NULL || item->mtime != mtime)
    {
        return NULL;
    }

    nautilus_hash_queue_move_existing_to_tail (preview_cache, item->folder);

    return g_ptr_array_ref (item->tiles);
}

static void
cache_store (GFile     *folder,
             guint64    mtime,
             GPtrArray *tiles)
{
    if (G_UNLIKELY (preview_cache == NULL))
    {
        preview_cache = nautilus_hash_queue_new (g_file_hash, (GEqualFunc) g_file_equal,
                                                 NULL, (GDestroyNotify) cache_item_free);
    }

    nautilus_hash_queue_remove (preview_cache, folder);

    if (nautilus_hash_queue_get_length (preview_cache) >= CACHE_COUNT_LIMIT)
    {
        CacheItem *oldest = nautilus_hash_queue_peek_head (preview_cache);

        nautilus_hash_queue_remove (preview_cache, oldest->folder);
    }

    CacheItem *item = g_new0 (CacheItem, 1);
    item->folder = g_object_ref (folder);
    item->mtime = mtime;
    item->tiles = g_ptr_array_ref (tiles);

    nautilus_hash_queue_enqueue (preview_cache, item->folder, item);
}

typedef struct
{
    char *uri;
    char *content_type;
    guint64 mtime;
    GdkPixbuf *pixbuf;
} Slot;

static void
slot_free (Slot *slot)
{
    g_free (slot->uri);
    g_free (slot->content_type);
    g_clear_object (&slot->pixbuf);
    g_free (slot);
}

typedef struct
{
    char *collate_key;
    GFileInfo *info;
} Candidate;

static void
candidate_free (Candidate *candidate)
{
    g_free (candidate->collate_key);
    g_clear_object (&candidate->info);
    g_free (candidate);
}

static int
compare_candidates (gconstpointer a,
                    gconstpointer b)
{
    const Candidate *ca = *(const Candidate **) a;
    const Candidate *cb = *(const Candidate **) b;

    return strcmp (ca->collate_key, cb->collate_key);
}

typedef struct
{
    GFile *folder;
    guint64 folder_mtime;
    guint64 size_limit;
    GPtrArray *slots;
    guint pending;
} LoadData;

static void
load_data_free (LoadData *data)
{
    g_clear_object (&data->folder);
    g_clear_pointer (&data->slots, g_ptr_array_unref);
    g_free (data);
}

static gboolean
is_candidate (GFileInfo *info)
{
    return g_file_info_get_file_type (info) == G_FILE_TYPE_REGULAR &&
           !g_file_info_get_is_hidden (info) &&
           !g_file_info_get_is_backup (info) &&
           g_file_info_get_size (info) > 0;
}

static GPtrArray *
collect_sorted_candidates (GFile         *folder,
                           GCancellable  *cancellable,
                           GError       **error)
{
    g_autoptr (GFileEnumerator) enumerator = g_file_enumerate_children (folder,
                                                                        CHILD_ATTRIBUTES,
                                                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                                        cancellable,
                                                                        error);

    if (enumerator == NULL)
    {
        return NULL;
    }

    g_autoptr (GPtrArray) candidates = g_ptr_array_new_with_free_func ((GDestroyNotify) candidate_free);

    for (guint seen = 0; seen < MAX_ENUMERATED_CHILDREN; seen++)
    {
        g_autoptr (GFileInfo) info = g_file_enumerator_next_file (enumerator, cancellable, error);

        if (info == NULL)
        {
            if (error != NULL && *error != NULL)
            {
                return NULL;
            }

            break;
        }

        if (!is_candidate (info))
        {
            continue;
        }

        Candidate *candidate = g_new0 (Candidate, 1);
        candidate->collate_key = g_utf8_collate_key_for_filename (g_file_info_get_name (info), -1);
        candidate->info = g_steal_pointer (&info);
        g_ptr_array_add (candidates, candidate);
    }

    g_ptr_array_sort (candidates, compare_candidates);

    return g_steal_pointer (&candidates);
}

static GdkPixbuf *
load_cached_thumbnail (const char *path)
{
    g_autoptr (GdkPixbuf) pixbuf = gdk_pixbuf_new_from_file (path, NULL);

    if (pixbuf == NULL)
    {
        return NULL;
    }

    return gdk_pixbuf_apply_embedded_orientation (pixbuf);
}

/* Runs in a worker thread. nautilus_can_thumbnail() and
 * nautilus_thumbnail_is_mimetype_limited_by_size() are only safe here because
 * their lazy globals were initialized on the main thread in load_async(). */
static void
scan_thread (GTask        *task,
             gpointer      source_object,
             gpointer      task_data,
             GCancellable *cancellable)
{
    LoadData *data = task_data;
    GError *error = NULL;
    g_autoptr (GPtrArray) candidates = collect_sorted_candidates (data->folder, cancellable, &error);

    if (candidates == NULL)
    {
        g_task_return_error (task, error);
        return;
    }

    GPtrArray *slots = g_ptr_array_new_with_free_func ((GDestroyNotify) slot_free);

    for (guint i = 0; i < candidates->len && slots->len < MAX_TILES; i++)
    {
        if (g_cancellable_is_cancelled (cancellable))
        {
            break;
        }

        GFileInfo *info = ((Candidate *) candidates->pdata[i])->info;
        const char *content_type = g_file_info_get_attribute_string (info,
                                                                     G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE);
        const char *thumb_path = g_file_info_get_attribute_byte_string (info,
                                                                        G_FILE_ATTRIBUTE_THUMBNAIL_PATH);
        gboolean thumb_valid = g_file_info_get_attribute_boolean (info,
                                                                  G_FILE_ATTRIBUTE_THUMBNAIL_IS_VALID);
        g_autoptr (GFile) child = g_file_get_child (data->folder, g_file_info_get_name (info));
        g_autofree char *uri = g_file_get_uri (child);
        guint64 mtime = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
        g_autoptr (GdkPixbuf) pixbuf = NULL;

        if (content_type == NULL)
        {
            content_type = "application/octet-stream";
        }

        if (thumb_path != NULL && thumb_valid)
        {
            pixbuf = load_cached_thumbnail (thumb_path);
        }

        if (pixbuf == NULL)
        {
            if (g_file_info_get_attribute_boolean (info, G_FILE_ATTRIBUTE_THUMBNAILING_FAILED))
            {
                continue;
            }

            if ((guint64) g_file_info_get_size (info) > data->size_limit &&
                nautilus_thumbnail_is_mimetype_limited_by_size (content_type))
            {
                continue;
            }

            if (!nautilus_can_thumbnail (uri, content_type, mtime))
            {
                continue;
            }
        }

        Slot *slot = g_new0 (Slot, 1);
        slot->uri = g_steal_pointer (&uri);
        slot->content_type = g_strdup (content_type);
        slot->mtime = mtime;
        slot->pixbuf = g_steal_pointer (&pixbuf);
        g_ptr_array_add (slots, slot);
    }

    g_task_return_pointer (task, slots, (GDestroyNotify) g_ptr_array_unref);
}

static gboolean
pixbuf_is_opaque (GdkPixbuf *pixbuf)
{
    if (!gdk_pixbuf_get_has_alpha (pixbuf) ||
        gdk_pixbuf_get_n_channels (pixbuf) != 4 ||
        gdk_pixbuf_get_bits_per_sample (pixbuf) != 8)
    {
        return !gdk_pixbuf_get_has_alpha (pixbuf);
    }

    int width = gdk_pixbuf_get_width (pixbuf);
    int height = gdk_pixbuf_get_height (pixbuf);
    int rowstride = gdk_pixbuf_get_rowstride (pixbuf);
    const guchar *pixels = gdk_pixbuf_read_pixels (pixbuf);

    for (int y = 0; y < height; y++)
    {
        const guchar *row = pixels + (gsize) y * rowstride;

        for (int x = 0; x < width; x++)
        {
            if (row[x * 4 + 3] != 0xff)
            {
                return FALSE;
            }
        }
    }

    return TRUE;
}

static GdkPixbuf *
scale_to_fit (GdkPixbuf *pixbuf,
              int        max_size)
{
    int width = gdk_pixbuf_get_width (pixbuf);
    int height = gdk_pixbuf_get_height (pixbuf);
    int biggest = MAX (width, height);

    if (biggest <= max_size)
    {
        return g_object_ref (pixbuf);
    }

    double scale = (double) max_size / biggest;

    return gdk_pixbuf_scale_simple (pixbuf,
                                    MAX (1, (int) round (width * scale)),
                                    MAX (1, (int) round (height * scale)),
                                    GDK_INTERP_BILINEAR);
}

static void
finish_load (GTask *task)
{
    LoadData *data = g_task_get_task_data (task);
    GPtrArray *tiles = g_ptr_array_new_with_free_func ((GDestroyNotify) tile_free);
    guint n_ready = 0;

    for (guint i = 0; i < data->slots->len; i++)
    {
        n_ready += ((Slot *) data->slots->pdata[i])->pixbuf != NULL;
    }

    /* A lone preview fills the whole folder face; a 2x2 grid needs half that. */
    int max_size = nautilus_thumbnail_get_max_size ();
    if (n_ready > 1)
    {
        max_size /= 2;
    }

    for (guint i = 0; i < data->slots->len; i++)
    {
        Slot *slot = data->slots->pdata[i];

        if (slot->pixbuf == NULL)
        {
            continue;
        }

        g_autoptr (GdkPixbuf) scaled = scale_to_fit (slot->pixbuf, max_size);
        NautilusFolderPreviewTile *tile = g_new0 (NautilusFolderPreviewTile, 1);

        tile->texture = gdk_texture_new_for_pixbuf (scaled);
        tile->is_video = nautilus_mime_is_video (slot->content_type);
        tile->is_opaque = pixbuf_is_opaque (scaled);
        g_ptr_array_add (tiles, tile);
    }

    if (!g_cancellable_is_cancelled (g_task_get_cancellable (task)))
    {
        cache_store (data->folder, data->folder_mtime, tiles);
    }

    g_task_return_pointer (task, tiles, (GDestroyNotify) g_ptr_array_unref);
}

typedef struct
{
    GTask *task;
    Slot *slot;
} GenerateData;

static void
thumbnail_generated_cb (GObject      *source_object,
                        GAsyncResult *result,
                        gpointer      user_data)
{
    g_autofree GenerateData *gen = user_data;
    g_autoptr (GTask) task = gen->task;
    LoadData *data = g_task_get_task_data (task);
    g_autoptr (GdkPixbuf) pixbuf = nautilus_create_thumbnail_finish (result, NULL);

    if (pixbuf != NULL)
    {
        gen->slot->pixbuf = gdk_pixbuf_apply_embedded_orientation (pixbuf);
    }

    data->pending--;
    if (data->pending == 0)
    {
        finish_load (task);
    }
}

static void
scan_done_cb (GObject      *source_object,
              GAsyncResult *result,
              gpointer      user_data)
{
    g_autoptr (GTask) task = user_data;
    LoadData *data = g_task_get_task_data (task);
    GError *error = NULL;
    GPtrArray *slots = g_task_propagate_pointer (G_TASK (result), &error);

    if (slots == NULL)
    {
        g_task_return_error (task, error);
        return;
    }

    data->slots = slots;

    /* Hold one count ourselves so callbacks that fire synchronously
     * (e.g. already cancelled) can't finish the task mid-loop. */
    data->pending = 1;

    for (guint i = 0; i < slots->len; i++)
    {
        Slot *slot = slots->pdata[i];

        if (slot->pixbuf != NULL)
        {
            continue;
        }

        GenerateData *gen = g_new0 (GenerateData, 1);
        gen->task = g_object_ref (task);
        gen->slot = slot;
        data->pending++;

        nautilus_create_thumbnail_async (slot->uri,
                                         slot->content_type,
                                         slot->mtime,
                                         g_task_get_cancellable (task),
                                         thumbnail_generated_cb,
                                         gen);
    }

    data->pending--;
    if (data->pending == 0)
    {
        finish_load (task);
    }
}

void
nautilus_folder_preview_load_async (GFile               *folder,
                                    guint64              folder_mtime,
                                    GCancellable        *cancellable,
                                    GAsyncReadyCallback  callback,
                                    gpointer             user_data)
{
    g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, user_data);
    g_task_set_source_tag (task, nautilus_folder_preview_load_async);

    GPtrArray *cached = cache_lookup (folder, folder_mtime);

    if (cached != NULL)
    {
        g_task_return_pointer (task, cached, (GDestroyNotify) g_ptr_array_unref);
        return;
    }

    g_autofree char *folder_uri = g_file_get_uri (folder);

    /* Warm up lazily-created globals before the worker thread touches them. */
    nautilus_can_thumbnail (folder_uri, "inode/directory", folder_mtime);
    nautilus_thumbnail_is_mimetype_limited_by_size ("image/png");

    LoadData *data = g_new0 (LoadData, 1);
    data->folder = g_object_ref (folder);
    data->folder_mtime = folder_mtime;
    data->size_limit = g_settings_get_uint64 (nautilus_preferences,
                                              NAUTILUS_PREFERENCES_FILE_THUMBNAIL_LIMIT) * 1000000;
    g_task_set_task_data (task, data, (GDestroyNotify) load_data_free);

    g_autoptr (GTask) scan_task = g_task_new (NULL, cancellable, scan_done_cb, g_object_ref (task));
    g_task_set_task_data (scan_task, data, NULL);
    g_task_set_priority (scan_task, G_PRIORITY_LOW);
    g_task_run_in_thread (scan_task, scan_thread);
}

GPtrArray *
nautilus_folder_preview_load_finish (GAsyncResult  *result,
                                     GError       **error)
{
    return g_task_propagate_pointer (G_TASK (result), error);
}

static float
snap (float value,
      int   scale_factor)
{
    return roundf (value * scale_factor) / scale_factor;
}

static void
draw_tile (GtkSnapshot                     *snapshot,
           const NautilusFolderPreviewTile *tile,
           const graphene_rect_t           *segment,
           float                            border,
           int                              scale_factor)
{
    float tex_width = gdk_texture_get_width (tile->texture);
    float tex_height = gdk_texture_get_height (tile->texture);
    float scale = MIN ((segment->size.width - 2 * border) / tex_width,
                       (segment->size.height - 2 * border) / tex_height);
    float image_width = snap (tex_width * scale, scale_factor);
    float image_height = snap (tex_height * scale, scale_factor);

    if (image_width < 1 || image_height < 1)
    {
        return;
    }

    graphene_rect_t image_rect = GRAPHENE_RECT_INIT (
        snap (segment->origin.x + (segment->size.width - image_width) / 2, scale_factor),
        snap (segment->origin.y + (segment->size.height - image_height) / 2, scale_factor),
        image_width,
        image_height);
    graphene_rect_t frame_rect;
    GskRoundedRect frame;

    graphene_rect_inset_r (&image_rect, -border, -border, &frame_rect);
    gsk_rounded_rect_init_from_rect (&frame, &frame_rect, border / 2);

    gtk_snapshot_append_outset_shadow (snapshot, &frame,
                                       &(GdkRGBA) { 0, 0, 0, 0.5 },
                                       0, 0, 0, MAX (border, 1.0f));

    if (tile->is_opaque)
    {
        gtk_snapshot_push_rounded_clip (snapshot, &frame);
        gtk_snapshot_append_color (snapshot, &(GdkRGBA) { 1, 1, 1, 1 }, &frame_rect);
        gtk_snapshot_pop (snapshot);
    }

    gtk_snapshot_append_scaled_texture (snapshot, tile->texture,
                                        GSK_SCALING_FILTER_TRILINEAR, &image_rect);

    if (tile->is_video && image_width >= MIN_VIDEO_FRAME_WIDTH)
    {
        gtk_snapshot_push_clip (snapshot, &image_rect);
        gtk_snapshot_save (snapshot);
        gtk_snapshot_translate (snapshot, &image_rect.origin);
        nautilus_ui_frame_video (snapshot, image_width, image_height);
        gtk_snapshot_restore (snapshot);
        gtk_snapshot_pop (snapshot);
    }
}

void
nautilus_folder_preview_snapshot (GtkSnapshot *snapshot,
                                  GPtrArray   *tiles,
                                  float        width,
                                  float        height,
                                  int          scale_factor)
{
    const float top_margin = height * 0.30f;
    const float bottom_margin = height / 6.0f;
    const float side_margin = width / 13.0f;
    const float spacing = 1.0f;
    const float border = MAX (width / 170.0f, 1.0f / scale_factor);
    const float area_width = width - 2 * side_margin;
    const float area_height = height - top_margin - bottom_margin;

    if (tiles == NULL || tiles->len == 0)
    {
        return;
    }

    if (tiles->len == 1)
    {
        draw_tile (snapshot, tiles->pdata[0],
                   &GRAPHENE_RECT_INIT (side_margin, top_margin, area_width, area_height),
                   border, scale_factor);
        return;
    }

    const float segment_width = (area_width + spacing) / 2 - spacing;
    const float segment_height = (area_height + spacing) / 2 - spacing;

    for (guint i = 0; i < tiles->len && i < MAX_TILES; i++)
    {
        guint column = i % 2;
        guint row = i / 2;

        draw_tile (snapshot, tiles->pdata[i],
                   &GRAPHENE_RECT_INIT (side_margin + column * (segment_width + spacing),
                                        top_margin + row * (segment_height + spacing),
                                        segment_width, segment_height),
                   border, scale_factor);
    }
}
