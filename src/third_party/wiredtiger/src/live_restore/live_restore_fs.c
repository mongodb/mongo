/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *  All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"
#include "live_restore_private.h"

/* This is where basename comes from. */
#include <libgen.h>

static int __live_restore_fs_directory_list_free(
  WT_FILE_SYSTEM *fs, WT_SESSION *wt_session, char **dirlist, uint32_t count);

/*
 * __live_restore_create_file_path --
 *     Generate the path of a file or directory in a layer. The file or directory must exist at the
 *     root of the layer.
 */
static int
__live_restore_create_file_path(
  WT_SESSION_IMPL *session, WTI_LIVE_RESTORE_FS_LAYER *layer, char *name, char **filepathp)
{
    char *base_name = basename(name);
    /* +1 for the path separator, +1 for the null terminator. */
    size_t len = strlen(layer->home) + 1 + strlen(base_name) + 1;

    WT_RET(__wt_calloc(session, 1, len, filepathp));
    WT_RET(__wt_snprintf(*filepathp, len, "%s%s%s", layer->home, __wt_path_separator(), base_name));

    return (0);
}

/*
 * __live_restore_fs_backing_filename --
 *     Convert a live restore file/directory path (e..g WT_TEST/WiredTiger.wt) to the actual path of
 *     the backing file/directory. This can be the file in the destination directory (which is
 *     identical the wiredtiger home path), or the file in the source directory. The function
 *     allocates memory for the path string and expects the caller to free it. If name is an
 *     absolute path, it will always be in format "/absolute_prefix/dest_home/relative_path",
 *     otherwise name is a relative path which always begins with dest_home (e..g
 *     dest_home/relative_path). The function returns path in format "layer->home/relative_path".
 */
static int
__live_restore_fs_backing_filename(WTI_LIVE_RESTORE_FS_LAYER *layer, WT_SESSION_IMPL *session,
  const char *dest_home, const char *name, char **pathp)
{
    WT_DECL_RET;
    size_t len;
    char *buf, *filename;

    buf = filename = NULL;

    /*
     * Name must start with dest_home. If name is an absolute path like "/home/dest_home/file.txt"
     * then dest_home which derived from conn->home will be "/home/dest_home".
     */
    filename = strstr(name, dest_home);
    WT_ASSERT_ALWAYS(session, filename == name,
      "Provided name '%s' does not start with the destination home folder path '%s'", name,
      dest_home);

    if (layer->which == WTI_LIVE_RESTORE_FS_LAYER_DESTINATION) {
        WT_RET(__wt_strdup(session, filename, pathp));
    } else {
        /*
         * By default the live restore file path is identical to the file in the destination
         * directory, which will include the destination folder. We need to replace this destination
         * folder's path with the source directory's path.
         */
        filename += strlen(dest_home);

        /* +1 for the null terminator. */
        len = strlen(layer->home) + strlen(filename) + 1;
        WT_ERR(__wt_calloc(session, 1, len, &buf));
        WT_ERR(__wt_snprintf(buf, len, "%s%s", layer->home, filename));

        *pathp = buf;
        __wt_verbose_debug3(session, WT_VERB_LIVE_RESTORE,
          "Generated SOURCE path: %s. layer->home = %s, name = %s", buf, layer->home, name);
    }

    if (0) {
err:
        __wt_free(session, buf);
    }
    return (ret);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
/*
 * __live_restore_debug_dump_extent_list --
 *     Dump the contents of a file handle's extent list. Callers must hold the extent list readlock
 *     at a minimum.
 */
static void
__live_restore_debug_dump_extent_list(WT_SESSION_IMPL *session, WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh)
{
    WTI_LIVE_RESTORE_HOLE_NODE *hole;
    WTI_LIVE_RESTORE_HOLE_NODE *prev;
    bool list_valid;

    __wt_verbose_debug1(
      session, WT_VERB_LIVE_RESTORE, "Dumping extent list for %s", lr_fh->iface.name);
    WT_ASSERT_ALWAYS(session, __wt_rwlock_islocked(session, &lr_fh->ext_lock),
      "Live restore lock not taken when needed");

    prev = NULL;
    hole = lr_fh->destination.hole_list_head;
    list_valid = true;

    while (hole != NULL) {

        /* Sanity check. This hole doesn't overlap with the previous hole */
        if (prev != NULL) {
            if (WTI_EXTENT_END(prev) >= hole->off) {
                __wt_verbose_debug1(session, WT_VERB_LIVE_RESTORE,
                  "Error: Holes overlap prev: %" PRId64 "-%" PRId64 ", hole: %" PRId64 "-%" PRId64,
                  prev->off, WTI_EXTENT_END(prev), hole->off, WTI_EXTENT_END(hole));
                list_valid = false;
            }
        }
        __wt_verbose_debug1(session, WT_VERB_LIVE_RESTORE, "Hole: %" PRId64 "-%" PRId64, hole->off,
          WTI_EXTENT_END(hole));

        prev = hole;
        hole = hole->next;
    }

    WT_ASSERT_ALWAYS(session, list_valid, "Extent list contains overlaps!");
}
#pragma GCC diagnostic pop

/*
 * __live_restore_create_tombstone_path --
 *     Generate the file path of a tombstone for a file. This tombstone does not need to exist.
 */
static int
__live_restore_create_tombstone_path(
  WT_SESSION_IMPL *session, const char *name, const char *marker, char **out)
{
    size_t p, suffix_len;

    p = strlen(name);
    suffix_len = strlen(marker);

    WT_RET(__wt_malloc(session, p + suffix_len + 1, out));
    memcpy(*out, name, p);
    memcpy(*out + p, marker, suffix_len + 1);
    return (0);
}

/*
 * __live_restore_fs_create_tombstone --
 *     Create a tombstone for the given file.
 */
static int
__live_restore_fs_create_tombstone(
  WT_FILE_SYSTEM *fs, WT_SESSION_IMPL *session, const char *name, uint32_t flags)
{
    WT_DECL_RET;
    WT_FILE_HANDLE *fh;
    WTI_LIVE_RESTORE_FS *lr_fs;
    uint32_t open_flags;
    char *path, *path_marker;

    lr_fs = (WTI_LIVE_RESTORE_FS *)fs;
    path = path_marker = NULL;

    WT_ERR(__live_restore_fs_backing_filename(
      &lr_fs->destination, session, lr_fs->destination.home, name, &path));
    WT_ERR(__live_restore_create_tombstone_path(
      session, path, WTI_LIVE_RESTORE_FS_TOMBSTONE_SUFFIX, &path_marker));

    open_flags = WT_FS_OPEN_CREATE;
    if (LF_ISSET(WT_FS_DURABLE | WT_FS_OPEN_DURABLE))
        FLD_SET(open_flags, WT_FS_OPEN_DURABLE);

    WT_ERR(lr_fs->os_file_system->fs_open_file(lr_fs->os_file_system, &session->iface, path_marker,
      WT_FS_OPEN_FILE_TYPE_DATA, open_flags, &fh));
    WT_ERR(fh->close(fh, &session->iface));

    __wt_verbose_debug2(session, WT_VERB_LIVE_RESTORE, "Creating tombstone: %s", path_marker);

err:
    __wt_free(session, path);
    __wt_free(session, path_marker);

    return (ret);
}

/*
 * __dest_has_tombstone --
 *     Check whether the destination directory contains a tombstone for a given file.
 */
static int
__dest_has_tombstone(WTI_LIVE_RESTORE_FS *lr_fs, char *name, WT_SESSION_IMPL *session, bool *existp)
{
    WT_DECL_RET;
    char *path_marker;

    path_marker = NULL;

    WT_ERR(__live_restore_create_tombstone_path(
      session, name, WTI_LIVE_RESTORE_FS_TOMBSTONE_SUFFIX, &path_marker));

    lr_fs->os_file_system->fs_exist(
      lr_fs->os_file_system, (WT_SESSION *)session, path_marker, existp);
    __wt_verbose_debug2(
      session, WT_VERB_LIVE_RESTORE, "Tombstone check for %s (Y/N)? %s", name, *existp ? "Y" : "N");

err:
    __wt_free(session, path_marker);
    return (ret);
}

/*
 * __live_restore_fs_has_file --
 *     Set a boolean to indicate if the given file name exists in the provided layer.
 */
static int
__live_restore_fs_has_file(WTI_LIVE_RESTORE_FS *lr_fs, WTI_LIVE_RESTORE_FS_LAYER *layer,
  WT_SESSION_IMPL *session, const char *name, bool *existsp)
{
    WT_DECL_RET;
    char *path;

    path = NULL;

    WT_ERR(
      __live_restore_fs_backing_filename(layer, session, lr_fs->destination.home, name, &path));
    WT_ERR(lr_fs->os_file_system->fs_exist(lr_fs->os_file_system, &session->iface, path, existsp));
err:
    __wt_free(session, path);

    return (ret);
}

/*
 * __live_restore_fs_find_layer --
 *     Find a layer for the given file. Return the type of the layer and whether the layer contains
 *     the file.
 */
static int
__live_restore_fs_find_layer(WT_FILE_SYSTEM *fs, WT_SESSION_IMPL *session, const char *name,
  WTI_LIVE_RESTORE_FS_LAYER_TYPE *whichp, bool *existp)
{
    WTI_LIVE_RESTORE_FS *lr_fs;

    WT_ASSERT(session, existp != NULL);

    *existp = false;
    lr_fs = (WTI_LIVE_RESTORE_FS *)fs;

    WT_RET(__live_restore_fs_has_file(lr_fs, &lr_fs->destination, session, name, existp));
    if (*existp) {
        /* The file exists in the destination we don't need to look any further. */
        if (whichp != NULL)
            *whichp = WTI_LIVE_RESTORE_FS_LAYER_DESTINATION;
        return (0);
    }

    WT_RET(__live_restore_fs_has_file(lr_fs, &lr_fs->source, session, name, existp));
    if (*existp) {
        /* The file exists in the source we don't need to look any further. */
        if (whichp != NULL)
            *whichp = WTI_LIVE_RESTORE_FS_LAYER_SOURCE;
    }

    return (0);
}

/*
 * __live_restore_fs_directory_list_worker --
 *     The list is a combination of files from the destination and source directories. For
 *     destination files, exclude any files matching the marker paths. For source files, exclude
 *     files that are either marked as tombstones or already present in the destination directory.
 */
static int
__live_restore_fs_directory_list_worker(WT_FILE_SYSTEM *fs, WT_SESSION *wt_session,
  const char *directory, const char *prefix, char ***dirlistp, uint32_t *countp, bool single)
{
    WT_DECL_RET;
    WTI_LIVE_RESTORE_FS *lr_fs = (WTI_LIVE_RESTORE_FS *)fs;
    WT_SESSION_IMPL *session = (WT_SESSION_IMPL *)wt_session;
    size_t dirallocsz = 0;
    uint32_t count_dest = 0, count_src = 0;
    char **dirlist_dest, **dirlist_src, **entries, *path_dest, *path_src, *temp_path;
    bool dest_exist = false, have_tombstone = false;
    bool dest_folder_exists = false, source_folder_exists = false;
    uint32_t num_src_files = 0, num_dest_files = 0;

    *dirlistp = dirlist_dest = dirlist_src = entries = NULL;
    path_dest = path_src = temp_path = NULL;

    __wt_verbose_debug1(session, WT_VERB_LIVE_RESTORE,
      "DIRECTORY LIST %s (single ? %s) : ", directory, single ? "YES" : "NO");

    /* Get files from destination. */
    WT_ERR(__live_restore_fs_backing_filename(
      &lr_fs->destination, session, lr_fs->destination.home, directory, &path_dest));

    WT_ERR(lr_fs->os_file_system->fs_exist(
      lr_fs->os_file_system, wt_session, path_dest, &dest_folder_exists));

    if (dest_folder_exists) {
        WT_ERR(lr_fs->os_file_system->fs_directory_list(
          lr_fs->os_file_system, wt_session, path_dest, prefix, &dirlist_dest, &num_dest_files));

        for (uint32_t i = 0; i < num_dest_files; ++i)
            if (!WT_SUFFIX_MATCH(dirlist_dest[i], WTI_LIVE_RESTORE_FS_TOMBSTONE_SUFFIX)) {
                WT_ERR(__wt_realloc_def(session, &dirallocsz, count_dest + 1, &entries));
                WT_ERR(__wt_strdup(session, dirlist_dest[i], &entries[count_dest]));
                ++count_dest;

                if (single)
                    goto done;
            }
    }

    /* Get files from source. */
    WT_ERR(__live_restore_fs_backing_filename(
      &lr_fs->source, session, lr_fs->destination.home, directory, &path_src));

    WT_ERR(lr_fs->os_file_system->fs_exist(
      lr_fs->os_file_system, wt_session, path_src, &source_folder_exists));

    if (source_folder_exists) {
        WT_ERR(lr_fs->os_file_system->fs_directory_list(
          lr_fs->os_file_system, wt_session, path_src, prefix, &dirlist_src, &num_src_files));

        for (uint32_t i = 0; i < num_src_files; ++i) {
            /*
             * If a file in source hasn't been background migrated yet we need to add it to the
             * list.
             */
            bool add_source_file = false;
            if (WT_SUFFIX_MATCH(dirlist_src[i], WTI_LIVE_RESTORE_FS_TOMBSTONE_SUFFIX)) {
                /*
                 * It is possible for tombstones to exist in the source directory. Currently those
                 * files are not cleaned up on completion. If a backup is taken after a live
                 * restore, and the user takes a snapshot of the directory instead of walking the
                 * backup cursor, then the tombstone files will be included in the backup.
                 */
                continue;
            }
            if (!dest_folder_exists)
                add_source_file = true;
            else {
                /*
                 * We're iterating files in the source, but we want to check if they exist in the
                 * destination, so create the file path to the backing destination file.
                 */
                WT_ERR(__live_restore_create_file_path(
                  session, &lr_fs->destination, dirlist_src[i], &temp_path));
                WT_ERR_NOTFOUND_OK(__live_restore_fs_has_file(
                                     lr_fs, &lr_fs->destination, session, temp_path, &dest_exist),
                  false);
                WT_ERR(__dest_has_tombstone(lr_fs, temp_path, session, &have_tombstone));
                __wt_free(session, temp_path);

                add_source_file = !dest_exist && !have_tombstone;
            }

            if (add_source_file) {
                WT_ERR(
                  __wt_realloc_def(session, &dirallocsz, count_dest + count_src + 1, &entries));
                WT_ERR(__wt_strdup(session, dirlist_src[i], &entries[count_dest + count_src]));
                ++count_src;
            }

            if (single)
                goto done;
        }
    }

    if (!dest_folder_exists && !source_folder_exists)
        WT_ERR_MSG(session, ENOENT,
          "Cannot report contents of '%s'. Folder does not exist in the source or destination.",
          directory);

done:
err:
    __wt_free(session, path_dest);
    __wt_free(session, path_src);
    __wt_free(session, temp_path);
    if (dirlist_dest != NULL)
        WT_TRET(
          __live_restore_fs_directory_list_free(fs, wt_session, dirlist_dest, num_dest_files));
    if (dirlist_src != NULL)
        WT_TRET(__live_restore_fs_directory_list_free(fs, wt_session, dirlist_src, num_src_files));

    *dirlistp = entries;
    *countp = count_dest + count_src;

    if (ret != 0)
        WT_TRET(__live_restore_fs_directory_list_free(fs, wt_session, entries, *countp));
    return (ret);
}

/*
 * __live_restore_fs_directory_list --
 *     Get a list of files from a directory.
 */
static int
__live_restore_fs_directory_list(WT_FILE_SYSTEM *fs, WT_SESSION *wt_session, const char *directory,
  const char *prefix, char ***dirlistp, uint32_t *countp)
{
    return (__live_restore_fs_directory_list_worker(
      fs, wt_session, directory, prefix, dirlistp, countp, false));
}

/*
 * __live_restore_fs_directory_list_single --
 *     Get one file from a directory.
 */
static int
__live_restore_fs_directory_list_single(WT_FILE_SYSTEM *fs, WT_SESSION *wt_session,
  const char *directory, const char *prefix, char ***dirlistp, uint32_t *countp)
{
    return (__live_restore_fs_directory_list_worker(
      fs, wt_session, directory, prefix, dirlistp, countp, true));
}

/*
 * __live_restore_fs_directory_list_free --
 *     Free memory returned by the directory listing.
 */
static int
__live_restore_fs_directory_list_free(
  WT_FILE_SYSTEM *fs, WT_SESSION *wt_session, char **dirlist, uint32_t count)
{
    WTI_LIVE_RESTORE_FS *lr_fs;

    lr_fs = (WTI_LIVE_RESTORE_FS *)fs;

    return (lr_fs->os_file_system->fs_directory_list_free(
      lr_fs->os_file_system, wt_session, dirlist, count));
}

/*
 * __live_restore_fs_exist --
 *     Return if the file exists.
 */
static int
__live_restore_fs_exist(WT_FILE_SYSTEM *fs, WT_SESSION *wt_session, const char *name, bool *existp)
{
    return (__live_restore_fs_find_layer(fs, (WT_SESSION_IMPL *)wt_session, name, NULL, existp));
}

/*
 * __live_restore_alloc_extent --
 *     Allocate and populate a new extent with the provided parameters.
 */
static int
__live_restore_alloc_extent(WT_SESSION_IMPL *session, wt_off_t offset, size_t len,
  WTI_LIVE_RESTORE_HOLE_NODE *next, WTI_LIVE_RESTORE_HOLE_NODE **holep)
{
    WTI_LIVE_RESTORE_HOLE_NODE *new;

    WT_RET(__wt_calloc_one(session, &new));
    new->off = offset;
    new->len = len;
    new->next = next;

    *holep = new;
    return (0);
}

/*
 * __live_restore_fs_free_extent_list --
 *     Free the extents associated with a live restore file handle. Callers must hold the extent
 *     list write lock.
 */
static void
__live_restore_fs_free_extent_list(WT_SESSION_IMPL *session, WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh)
{
    WTI_LIVE_RESTORE_HOLE_NODE *hole;
    WTI_LIVE_RESTORE_HOLE_NODE *temp;

    WT_ASSERT_ALWAYS(session, __wt_rwlock_islocked(session, &lr_fh->ext_lock),
      "Live restore lock not taken when needed");

    hole = lr_fh->destination.hole_list_head;
    lr_fh->destination.hole_list_head = NULL;

    while (hole != NULL) {
        temp = hole;
        hole = hole->next;

        __wt_free(session, temp);
    }

    return;
}

/*
 * __live_restore_fh_lock --
 *     Lock/unlock a file.
 */
static int
__live_restore_fh_lock(WT_FILE_HANDLE *fh, WT_SESSION *wt_session, bool lock)
{
    WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh;

    lr_fh = (WTI_LIVE_RESTORE_FILE_HANDLE *)fh;
    WT_ASSERT((WT_SESSION_IMPL *)wt_session, lr_fh->destination.fh != NULL);
    return (lr_fh->destination.fh->fh_lock(lr_fh->destination.fh, wt_session, lock));
}

/*
 * __live_restore_remove_extlist_hole --
 *     Track that we wrote something by removing its hole from the extent list. Callers must hold
 *     the extent list write lock.
 */
static int
__live_restore_remove_extlist_hole(
  WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh, WT_SESSION_IMPL *session, wt_off_t offset, size_t len)
{
    WTI_LIVE_RESTORE_HOLE_NODE *hole, *tmp, *new, *prev_hole;
    wt_off_t write_end;

    WT_ASSERT_ALWAYS(session, __wt_rwlock_islocked(session, &lr_fh->ext_lock),
      "Live restore lock not taken when needed");
    __wt_verbose_debug3(session, WT_VERB_LIVE_RESTORE, "REMOVE HOLE %s: %" PRId64 "-%" PRId64,
      lr_fh->iface.name, offset, WTI_OFFSET_END(offset, len));

    write_end = WTI_OFFSET_END(offset, len);
    hole = lr_fh->destination.hole_list_head;
    prev_hole = NULL;
    while (hole != NULL) {
        if (write_end < hole->off)
            /* We won't find any more overlapping holes. Stop searching. */
            break;

        if (offset <= hole->off && write_end >= WTI_EXTENT_END(hole)) {
            /* The write fully overlaps a hole. Delete it. */
            __wt_verbose_debug3(session, WT_VERB_LIVE_RESTORE,
              "Fully overlaps hole %" PRId64 "-%" PRId64, hole->off, WTI_EXTENT_END(hole));

            tmp = hole;
            if (prev_hole == NULL)
                lr_fh->destination.hole_list_head = hole->next;
            else
                prev_hole->next = hole->next;
            hole = hole->next;
            __wt_free(session, tmp);
            continue;
        } else if (offset > hole->off && write_end < WTI_EXTENT_END(hole)) {
            /* The write is entirely within the hole. Split the hole in two. */

            __wt_verbose_debug3(session, WT_VERB_LIVE_RESTORE,
              "Fully contained by hole %" PRId64 "-%" PRId64, hole->off, WTI_EXTENT_END(hole));

            /* First create the hole to the right of the write. */
            WT_RET(__live_restore_alloc_extent(session, write_end + 1,
              (size_t)(WTI_EXTENT_END(hole) - write_end), hole->next, &new));

            /*
             * Then shrink the existing hole so it's to the left of the write and point it at the
             * new hole.
             */
            hole->len = (size_t)(offset - hole->off);
            hole->next = new;
        } else if (offset <= hole->off && WTI_OFFSET_IN_EXTENT(write_end, hole)) {
            /* The write starts before the hole and ends within it. Shrink the hole. */
            __wt_verbose_debug3(session, WT_VERB_LIVE_RESTORE,
              "Partial overlap to the left of hole %" PRId64 "-%" PRId64, hole->off,
              WTI_EXTENT_END(hole));

            hole->len = (size_t)(WTI_EXTENT_END(hole) - write_end);
            hole->off = write_end + 1;
        } else if (WTI_OFFSET_IN_EXTENT(offset, hole) && write_end >= WTI_EXTENT_END(hole)) {
            __wt_verbose_debug3(session, WT_VERB_LIVE_RESTORE,
              "Partial overlap to the right of hole %" PRId64 "-%" PRId64, hole->off,
              WTI_EXTENT_END(hole));
            /* The write starts within the hole and ends after it. Shrink the hole. */
            hole->len = (size_t)(offset - hole->off);
        } else
            /* No overlap. Safety check */
            WT_ASSERT(session, write_end < hole->off || offset > WTI_EXTENT_END(hole));

        prev_hole = hole;
        hole = hole->next;
    }
    return (0);
}

typedef enum { NONE, FULL, PARTIAL } WT_LIVE_RESTORE_SERVICE_STATE;

/* !!!
 * __live_restore_can_service_read --
 *     Return if a read can be serviced by the destination file. Callers must hold the extent list
 *     read lock at a minimum.
 *     There are three possible scenarios:
 *     - The read is entirely within a hole and we return NONE.
 *     - The read is entirely outside of all holes and we return FULL.
 *     - The read begins outside a hole and then ends inside, in which case we return PARTIAL.
 *       This scenario will only happen if background data migration occurs concurrently and has
 *       partially migrated the content we're reading. The background threads always copies data in
 *       order, so the partially filled hole can only start outside a hole and then continue into a
 *       hole.
 *     All other scenarios are considered impossible.
 */
static WT_LIVE_RESTORE_SERVICE_STATE
__live_restore_can_service_read(WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh, WT_SESSION_IMPL *session,
  wt_off_t offset, size_t len, WTI_LIVE_RESTORE_HOLE_NODE **holep)
{
    WTI_LIVE_RESTORE_HOLE_NODE *hole;
    wt_off_t read_end;
    bool read_begins_in_hole, read_ends_in_hole;

    if (lr_fh->destination.complete || lr_fh->source == NULL)
        goto done;

    WT_ASSERT_ALWAYS(session, __wt_rwlock_islocked(session, &lr_fh->ext_lock),
      "Live restore lock not taken when needed");

    read_end = WTI_OFFSET_END(offset, len);
    hole = lr_fh->destination.hole_list_head;
    while (hole != NULL) {
        if (read_end < hole->off)
            /* All subsequent holes are past the read. We won't find matching holes. */
            break;

        WT_ASSERT_ALWAYS(session, !(offset < hole->off && WTI_EXTENT_END(hole) < read_end),
          "Read (offset: %" PRId64 ", len: %" WT_SIZET_FMT ") encompasses a hole (offset: %" PRId64
          ", len: %" WT_SIZET_FMT ")",
          offset, len, hole->off, hole->len);

        read_begins_in_hole = WTI_OFFSET_IN_EXTENT(offset, hole);
        read_ends_in_hole = WTI_OFFSET_IN_EXTENT(read_end, hole);
        if (read_begins_in_hole && read_ends_in_hole) {
            /* Our read is entirely within a hole */
            __wt_verbose_debug3(session, WT_VERB_LIVE_RESTORE,
              "CANNOT SERVICE %s: Reading from hole. Read: %" PRId64 "-%" PRId64 ", hole: %" PRId64
              "-%" PRId64,
              lr_fh->iface.name, offset, read_end, hole->off, WTI_EXTENT_END(hole));
            return (NONE);
        } else if (!read_begins_in_hole && read_ends_in_hole) {
            /*
             * The block manager reads entire pages so we can expect all reads to exist entirely
             * inside or outside a hole during normal WiredTiger operation. The one exception is
             * when background migration threads are running as they will copy data chunks
             * regardless of page size. The background threads always migrate a file from start to
             * finish so the one case where we partially read from a hole is when the background
             * thread reads the first part of a page and then we read that page before the remainder
             * is migrated.
             */
            __wt_verbose_debug3(session, WT_VERB_LIVE_RESTORE,
              "PARTIAL READ %s: Reading from hole. Read: %" PRId64 "-%" PRId64 ", hole: %" PRId64
              "-%" PRId64,
              lr_fh->iface.name, offset, read_end, hole->off, WTI_EXTENT_END(hole));
            *holep = hole;
            return (PARTIAL);
        } else if (read_begins_in_hole && !read_ends_in_hole) {
            /* A partial read should never begin in a hole. */
            WT_ASSERT_ALWAYS(session, false,
              "Read (offset: %" PRId64 ", len: %" WT_SIZET_FMT
              ") begins in a hole but does not end in one (offset: %" PRId64
              ", "
              "len: %" WT_SIZET_FMT ")",
              offset, len, hole->off, hole->len);
        }

        hole = hole->next;
    }
    /*
     * If we got here we either traversed the full hole list and didn't find a hole, or the read is
     * prior to any holes.
     */
done:
    __wt_verbose_debug3(
      session, WT_VERB_LIVE_RESTORE, "CAN SERVICE %s: No hole found", lr_fh->iface.name);
    return (FULL);
}

/*
 * __live_restore_fh_write_int --
 *     Write to a file. Callers of this function must hold the extent list lock.
 */
static int
__live_restore_fh_write_int(
  WT_FILE_HANDLE *fh, WT_SESSION *wt_session, wt_off_t offset, size_t len, const void *buf)
{
    WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh;
    WT_SESSION_IMPL *session;

    lr_fh = (WTI_LIVE_RESTORE_FILE_HANDLE *)fh;
    session = (WT_SESSION_IMPL *)wt_session;

    WT_ASSERT_ALWAYS(session, __wt_rwlock_islocked(session, &lr_fh->ext_lock),
      "Live restore lock not taken when needed");
    __wt_verbose_debug3(session, WT_VERB_LIVE_RESTORE, "WRITE %s: %" PRId64 ", %" WT_SIZET_FMT,
      fh->name, offset, len);

    WT_RET(lr_fh->destination.fh->fh_write(lr_fh->destination.fh, wt_session, offset, len, buf));
    return (__live_restore_remove_extlist_hole(lr_fh, session, offset, len));
}

/*
 * __live_restore_fh_write --
 *     File write.
 */
static int
__live_restore_fh_write(
  WT_FILE_HANDLE *fh, WT_SESSION *wt_session, wt_off_t offset, size_t len, const void *buf)
{
    WT_DECL_RET;
    WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh;
    WT_SESSION_IMPL *session;

    lr_fh = (WTI_LIVE_RESTORE_FILE_HANDLE *)fh;
    session = (WT_SESSION_IMPL *)wt_session;

    WTI_WITH_LIVE_RESTORE_EXTENT_LIST_WRITE_LOCK(
      session, lr_fh, ret = __live_restore_fh_write_int(fh, wt_session, offset, len, buf));
    return (ret);
}

/*
 * __live_restore_fh_read --
 *     File read in a live restore file system.
 */
static int
__live_restore_fh_read(
  WT_FILE_HANDLE *fh, WT_SESSION *wt_session, wt_off_t offset, size_t len, void *buf)
{
    WT_DECL_RET;
    WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh;
    WT_SESSION_IMPL *session;
    char *read_data;

    lr_fh = (WTI_LIVE_RESTORE_FILE_HANDLE *)fh;
    session = (WT_SESSION_IMPL *)wt_session;

    __wt_verbose_debug3(session, WT_VERB_LIVE_RESTORE, "READ %s : %" PRId64 ", %" WT_SIZET_FMT,
      fh->name, offset, len);

    read_data = (char *)buf;

    __wt_readlock(session, &lr_fh->ext_lock);
    WTI_LIVE_RESTORE_HOLE_NODE *hole = NULL;

    /*
     * FIXME-WT-13828: WiredTiger will read the metadata file after creation but before anything has
     * been written in this case we forward the read to the empty metadata file in the destination.
     * Is this correct?
     */
    /*
     * The partial read length variables need to be initialized inside the else case to avoid clang
     * sanitizer complaining about dead stores. However if we use a switch case here _and_
     * initialize the variables inside the PARTIAL case then gcc complains about the switch jumping
     * over the variable declaration. Thus we use if/else and declare inside to keep both happy.
     */
    WT_LIVE_RESTORE_SERVICE_STATE read_state =
      __live_restore_can_service_read(lr_fh, session, offset, len, &hole);
    if (read_state == FULL) {
        __wt_verbose_debug3(session, WT_VERB_LIVE_RESTORE, "    READ FROM DEST (src is NULL? %s)",
          lr_fh->source == NULL ? "YES" : "NO");
        /* Read the full read from the destination. */
        WT_ERR(lr_fh->destination.fh->fh_read(
          lr_fh->destination.fh, wt_session, offset, len, read_data));
    } else if (read_state == PARTIAL) {
        /*
         * If a portion of the read region is serviceable, combine a read from the source and
         * destination.
         */
        /*
         *              <--read len--->
         * read:        |-------------|
         * extent list: |####|----hole----|
         *              ^    ^        |
         *              |    |        |
         *           read off|        |
         *                hole off    |
         * read dest:   |----|
         * read source:      |--------|
         *
         *
         */
        size_t dest_partial_read_len = (size_t)(hole->off - offset);
        size_t source_partial_read_len = len - dest_partial_read_len;

        /* First read the serviceable portion from the destination. */
        __wt_verbose_debug1(session, WT_VERB_LIVE_RESTORE,
          "    PARTIAL READ FROM DEST (offset: %" PRId64 ", len: %" WT_SIZET_FMT ")", offset,
          dest_partial_read_len);
        WT_ERR(lr_fh->destination.fh->fh_read(
          lr_fh->destination.fh, wt_session, offset, dest_partial_read_len, read_data));

        /* Now read the remaining data from the source. */
        __wt_verbose_debug1(session, WT_VERB_LIVE_RESTORE,
          "    PARTIAL READ FROM SOURCE (offset: %" PRId64 ", len: %" WT_SIZET_FMT ")", hole->off,
          source_partial_read_len);
        WT_ERR(lr_fh->source->fh_read(lr_fh->source, wt_session, hole->off, source_partial_read_len,
          read_data + dest_partial_read_len));
    } else {
        /* Interestingly you cannot not have a format in verbose. */
        __wt_verbose_debug3(session, WT_VERB_LIVE_RESTORE, "    READ FROM %s", "SOURCE");
        /* Read the full read from the source. */
        WT_ERR(lr_fh->source->fh_read(lr_fh->source, wt_session, offset, len, read_data));
    }

err:
    /*
     * We could, in theory, release this lock a lot earlier. However we need to consider how a
     * concurrent write could affect the read. Given the block manager should only read and write
     * full blocks it should be fine to unlock early. We would need to copy the hole->off and
     * hole->len from the hole before unlocking.
     *
     * Right now reads and writes are atomic if we unlock early we lose some guarantee of atomicity.
     */
    __wt_readunlock(session, &lr_fh->ext_lock);

    return (ret);
}

/*
 * __live_restore_fill_hole --
 *     Fill a single hole in the destination file. If the hole list is empty indicate using the
 *     finished parameter. Must be called while holding the extent list write lock.
 */
static int
__live_restore_fill_hole(WT_FILE_HANDLE *fh, WT_SESSION *wt_session, char *buf,
  WT_TIMER *start_timer, uint64_t *msg_count, bool *finishedp)
{
    WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh = (WTI_LIVE_RESTORE_FILE_HANDLE *)fh;
    WTI_LIVE_RESTORE_HOLE_NODE *hole = lr_fh->destination.hole_list_head;
    WT_SESSION_IMPL *session = (WT_SESSION_IMPL *)wt_session;

    WT_ASSERT(session, __wt_rwlock_islocked(session, &lr_fh->ext_lock));
    if (hole == NULL) {
        /* If there are no holes to fill we're done. */
        *finishedp = true;
        return (0);
    }

    __wt_verbose_debug3(session, WT_VERB_LIVE_RESTORE,
      "Found hole in %s at %" PRId64 "-%" PRId64 " during background migration. ", fh->name,
      hole->off, WTI_EXTENT_END(hole));

    /*
     * When encountering a large hole, break the read into small chunks. Split the hole into n
     * chunks: the first n - 1 chunks will read a full WT_LIVE_RESTORE_READ_SIZE buffer, and the
     * last chunk reads the remaining data. This loop is not obvious, effectively the read is
     * shrinking the hole in the stack below us. This is why we always read from the start at the
     * beginning of the loop.
     */
    size_t read_size = WT_MIN(hole->len, lr_fh->destination.back_pointer->read_size);
    uint64_t time_diff_ms;

    __wt_timer_evaluate_ms(session, start_timer, &time_diff_ms);
    __wt_verbose_debug3(session, WT_VERB_LIVE_RESTORE,
      "    BACKGROUND READ %s : %" PRId64 ", %" WT_SIZET_FMT, lr_fh->iface.name, hole->off,
      read_size);
    if ((time_diff_ms / (WT_THOUSAND * WT_PROGRESS_MSG_PERIOD)) > *msg_count) {
        __wt_verbose(session, WT_VERB_LIVE_RESTORE_PROGRESS,
          "Live restore running on %s for %" PRIu64 " seconds. Currently copying offset %" PRId64
          " of size %" WT_SIZET_FMT,
          fh->name, time_diff_ms / WT_THOUSAND, hole->off, lr_fh->source_size);
        *msg_count = time_diff_ms / (WT_THOUSAND * WT_PROGRESS_MSG_PERIOD);
    }
    WT_RET(lr_fh->source->fh_read(lr_fh->source, wt_session, hole->off, read_size, buf));
    return (__live_restore_fh_write_int(fh, wt_session, hole->off, read_size, buf));
}

/*
 * __wti_live_restore_fs_fill_holes --
 *     Copy all remaining data from the source to the destination. On completion this means there
 *     are no holes in the destination file's extent list. If we find one promote-read the content
 *     into the destination.
 *
 * NOTE!! This assumes there cannot be holes in source, and that any truncates/extensions of the
 *     destination file are already handled elsewhere.
 */
int
__wti_live_restore_fs_fill_holes(WT_FILE_HANDLE *fh, WT_SESSION *wt_session)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session = (WT_SESSION_IMPL *)wt_session;
    WT_TIMER timer;
    WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh = (WTI_LIVE_RESTORE_FILE_HANDLE *)fh;
    uint64_t msg_count = 0;
    char *buf = NULL;
    bool finished = false;

    WT_RET(
      __wt_calloc(session, 1, ((WTI_LIVE_RESTORE_FS *)S2C(session)->file_system)->read_size, &buf));

    __wt_timer_start(session, &timer);
    while (!finished) {
        WTI_WITH_LIVE_RESTORE_EXTENT_LIST_WRITE_LOCK(session, lr_fh,
          ret = __live_restore_fill_hole(fh, wt_session, buf, &timer, &msg_count, &finished));
        WT_ERR(ret);

        /*
         * Because this loop can run for a very long time, ensure the system has not entered a panic
         * state in the meantime.
         */
        WT_ERR(WT_SESSION_CHECK_PANIC(wt_session));
    }

    /*
     * Sync the file over. In theory we don't need this as losing any writes, on crash, that copy
     * data from source to destination should be safe. If the write doesn't complete then a hole
     * should remain and the same write will be performed on the startup. To avoid depending on that
     * property we choose to sync then file over anyway.
     */
    WT_ERR(lr_fh->destination.fh->fh_sync(lr_fh->destination.fh, wt_session));

err:
    __wt_free(session, buf);

    return (ret);
}

/*
 * __live_restore_fh_close --
 *     Close the file.
 */
static int
__live_restore_fh_close(WT_FILE_HANDLE *fh, WT_SESSION *wt_session)
{
    WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh;
    WT_SESSION_IMPL *session;

    lr_fh = (WTI_LIVE_RESTORE_FILE_HANDLE *)fh;
    session = (WT_SESSION_IMPL *)wt_session;
    __wt_verbose_debug2(
      session, WT_VERB_LIVE_RESTORE, "LIVE_RESTORE_FS: Closing file: %s", fh->name);

    /*
     * If we hit an error during file handle creation we'll call this function to free the partially
     * created handle. At this point fields may be uninitialized so we check for null pointers.
     */
    if (lr_fh->destination.fh != NULL) {
        /*
         * We cannot queue the turtle file in the live restore queue as we cannot open a cursor on
         * it, but it is critical that we ensure all gaps in it are migrated across. Thus the turtle
         * file is the one file we intentionally fill holes on close for. This is relatively cheap
         * given how small it is.
         */
        if (WT_SUFFIX_MATCH(fh->name, WT_METADATA_TURTLE)) {
            __wt_verbose_debug2(
              session, WT_VERB_FILEOPS, "%s", "LIVE_RESTORE_FS: Filling holes for turtle file.");
            WT_RET(__wti_live_restore_fs_fill_holes(fh, wt_session));
        }

        lr_fh->destination.fh->close(lr_fh->destination.fh, wt_session);
    }

    WTI_WITH_LIVE_RESTORE_EXTENT_LIST_WRITE_LOCK(
      session, lr_fh, __live_restore_fs_free_extent_list(session, lr_fh));
    __wt_rwlock_destroy(session, &lr_fh->ext_lock);

    if (lr_fh->source != NULL) /* It's possible that we never opened the file in the source. */
        lr_fh->source->close(lr_fh->source, wt_session);
    __wt_free(session, lr_fh->iface.name);
    __wt_free(session, lr_fh);

    return (0);
}

/*
 * __live_restore_fh_size --
 *     Get the size of a file in bytes, by file handle.
 */
static int
__live_restore_fh_size(WT_FILE_HANDLE *fh, WT_SESSION *wt_session, wt_off_t *sizep)
{
    WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh;
    wt_off_t destination_size;

    lr_fh = (WTI_LIVE_RESTORE_FILE_HANDLE *)fh;

    WT_RET(lr_fh->destination.fh->fh_size(lr_fh->destination.fh, wt_session, &destination_size));
    *sizep = destination_size;
    return (0);
}

/*
 * __live_restore_fh_sync --
 *     POSIX fsync. This only sync the destination as the source is readonly.
 */
static int
__live_restore_fh_sync(WT_FILE_HANDLE *fh, WT_SESSION *wt_session)
{
    WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh;

    lr_fh = (WTI_LIVE_RESTORE_FILE_HANDLE *)fh;
    return (lr_fh->destination.fh->fh_sync(lr_fh->destination.fh, wt_session));
}

/*
 * __live_restore_fh_truncate --
 *     Truncate a file. This operation is only applied to the destination file.
 */
static int
__live_restore_fh_truncate(WT_FILE_HANDLE *fh, WT_SESSION *wt_session, wt_off_t len)
{
    WT_DECL_RET;
    WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh;
    wt_off_t old_len, truncate_end, truncate_start;

    lr_fh = (WTI_LIVE_RESTORE_FILE_HANDLE *)fh;
    old_len = 0;
    /*
     * If we truncate a range we'll never need to read that range from the source file. Mark it as
     * such.
     */
    WT_RET(__live_restore_fh_size(fh, wt_session, &old_len));

    if (old_len == len)
        /* Sometimes we call truncate but don't change the length. Ignore */
        return (0);

    __wt_verbose_debug2((WT_SESSION_IMPL *)wt_session, WT_VERB_LIVE_RESTORE,
      "truncating file %s from %" PRId64 " to %" PRId64, fh->name, old_len, len);

    /*
     * Truncate can be used to shorten a file or to extend it. In both cases the truncated/extended
     * range doesn't need to be read from the source directory.
     */
    truncate_start = WT_MIN(len, old_len);
    truncate_end = WT_MAX(len, old_len);

    WT_SESSION_IMPL *session = (WT_SESSION_IMPL *)wt_session;

    WTI_WITH_LIVE_RESTORE_EXTENT_LIST_WRITE_LOCK(
      session, lr_fh,
      ret = __live_restore_remove_extlist_hole(
        lr_fh, session, truncate_start, (size_t)(truncate_end - truncate_start)););
    WT_RET(ret);

    return (lr_fh->destination.fh->fh_truncate(lr_fh->destination.fh, wt_session, len));
}

/*
 * __live_restore_fs_open_in_source --
 *     Open a file handle in the source.
 */
static int
__live_restore_fs_open_in_source(WTI_LIVE_RESTORE_FS *lr_fs, WT_SESSION_IMPL *session,
  WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh, uint32_t flags)
{
    WT_DECL_RET;
    WT_FILE_HANDLE *fh;

    char *path;

    path = NULL;

    /*
     * Clear the create flag. This comes from up the stack which has no concept of source or
     * destination.
     */
    FLD_CLR(flags, WT_FS_OPEN_CREATE);

    /* Open the file in the layer. */
    WT_ERR(__live_restore_fs_backing_filename(
      &lr_fs->source, session, lr_fs->destination.home, lr_fh->iface.name, &path));
    WT_ERR(lr_fs->os_file_system->fs_open_file(
      lr_fs->os_file_system, (WT_SESSION *)session, path, lr_fh->file_type, flags, &fh));

    lr_fh->source = fh;

err:
    __wt_free(session, path);
    return (ret);
}

#include <unistd.h>
/*
 * __live_restore_fh_find_holes_in_dest_file --
 *     When opening a file from destination create its existing hole list from the file system
 *     information. Any holes in the extent list are data that hasn't been copied from source yet.
 */
static int
__live_restore_fh_find_holes_in_dest_file(
  WT_SESSION_IMPL *session, char *filename, WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh)
{
    WT_DECL_RET;
    wt_off_t data_end_offset, file_size;
    int fd;

    data_end_offset = 0;
    __wt_verbose_debug2(
      session, WT_VERB_LIVE_RESTORE, "LIVE_RESTORE_FS: Opening file: %s", filename);
    WT_SYSCALL(((fd = open(filename, O_RDONLY)) == -1 ? -1 : 0), ret);
    if (ret != 0)
        WT_RET_MSG(session, ret, "Failed to open file descriptor on %s", filename);

    /* Check that we opened a valid file descriptor. */
    WT_ASSERT(session, fcntl(fd, F_GETFD) != -1 || errno != EBADF);
    WT_ERR(__live_restore_fh_size((WT_FILE_HANDLE *)lr_fh, (WT_SESSION *)session, &file_size));
    __wt_verbose_debug2(session, WT_VERB_LIVE_RESTORE, "File: %s", filename);
    __wt_verbose_debug2(session, WT_VERB_LIVE_RESTORE, "    len: %" PRId64, file_size);

    if (file_size > 0)
        /*
         * Initialize the file as one big hole. We'll then lseek the file to find data blocks and
         * remove those ranges from the hole list.
         */
        WT_ERR(__live_restore_alloc_extent(
          session, 0, (size_t)file_size, NULL, &lr_fh->destination.hole_list_head));

    /*
     * Find the next data block. data_end_offset is initialized to zero so we start from the
     * beginning of the file. lseek will find a block when it starts already positioned on the
     * block, so starting at zero ensures we'll find data blocks at the beginning of the file. This
     * logic is single threaded but removing holes from the extent list requires the lock.
     */
    __wt_writelock(session, &lr_fh->ext_lock);
    wt_off_t data_offset;
    while ((data_offset = lseek(fd, data_end_offset, SEEK_DATA)) != -1) {

        data_end_offset = lseek(fd, data_offset, SEEK_HOLE);
        /* All data must be followed by a hole */
        WT_ASSERT(session, data_end_offset != -1);
        WT_ASSERT(session, data_end_offset > data_offset - 1);

        __wt_verbose_debug2(session, WT_VERB_LIVE_RESTORE,
          "File: %s, has data from %" PRId64 "-%" PRId64, filename, data_offset, data_end_offset);
        WT_ERR(__live_restore_remove_extlist_hole(
          lr_fh, session, data_offset, (size_t)(data_end_offset - data_offset)));
    }

err:
    __wt_writeunlock(session, &lr_fh->ext_lock);
    WT_SYSCALL_TRET(close(fd), ret);
    return (ret);
}

/*
 * __live_restore_handle_verify_hole_list --
 *     Check that the generated hole list doesn't not contain holes that extend past the end of the
 *     source file. If it does we would read junk data and copy it into the destination file.
 */
static int
__live_restore_handle_verify_hole_list(WT_SESSION_IMPL *session, WTI_LIVE_RESTORE_FS *lr_fs,
  WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh, const char *name)
{
    WT_DECL_RET;
    WT_FILE_HANDLE *source_fh = NULL;
    char *source_path = NULL;

    if (lr_fh->destination.hole_list_head == NULL)
        return (0);

    bool source_exist = false;
    WT_ERR_NOTFOUND_OK(
      __live_restore_fs_has_file(lr_fs, &lr_fs->source, session, name, &source_exist), true);

    if (source_exist) {
        wt_off_t source_size;

        WT_ERR(__live_restore_fs_backing_filename(
          &lr_fs->source, session, lr_fs->destination.home, name, &source_path));
        WT_ERR(lr_fs->os_file_system->fs_open_file(lr_fs->os_file_system, (WT_SESSION *)session,
          source_path, lr_fh->file_type, 0, &source_fh));
        WT_ERR(lr_fs->os_file_system->fs_size(
          lr_fs->os_file_system, (WT_SESSION *)session, source_fh->name, &source_size));

        __wt_readlock(session, &lr_fh->ext_lock);
        WTI_LIVE_RESTORE_HOLE_NODE *final_hole;
        final_hole = lr_fh->destination.hole_list_head;
        while (final_hole->next != NULL)
            final_hole = final_hole->next;

        if (WTI_EXTENT_END(final_hole) >= source_size) {
            __wt_verbose_debug1(session, WT_VERB_LIVE_RESTORE,
              "Error: Hole list for %s has holes beyond the the end of the source file!", name);
            __live_restore_debug_dump_extent_list(session, lr_fh);
            __wt_readunlock(session, &lr_fh->ext_lock);
            WT_ERR_MSG(session, EINVAL,
              "Hole list for %s has holes beyond the the end of the source file!", name);
        }
        __wt_readunlock(session, &lr_fh->ext_lock);

    } else
        WT_ASSERT_ALWAYS(session, lr_fh->destination.hole_list_head == NULL,
          "Source file doesn't exist but there are holes in the destination file");

err:
    if (source_fh != NULL)
        source_fh->close(source_fh, &session->iface);

    if (source_path != NULL)
        __wt_free(session, source_path);

    return (ret);
}

/*
 * __live_restore_fs_open_in_destination --
 *     Open a file handle.
 */
static int
__live_restore_fs_open_in_destination(WTI_LIVE_RESTORE_FS *lr_fs, WT_SESSION_IMPL *session,
  WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh, const char *name, uint32_t flags, bool create)
{
    WT_DECL_RET;
    WT_FILE_HANDLE *fh;
    char *path;

    /* This function is only called for files. Directories are handled separately. */
    WT_ASSERT_ALWAYS(session, lr_fh->file_type != WT_FS_OPEN_FILE_TYPE_DIRECTORY,
      "Open in destination should not be called on directories");

    path = NULL;

    if (create)
        flags |= WT_FS_OPEN_CREATE;

    /* Open the file in the layer. */
    WT_ERR(__live_restore_fs_backing_filename(
      &lr_fs->destination, session, lr_fs->destination.home, lr_fh->iface.name, &path));
    WT_ERR(lr_fs->os_file_system->fs_open_file(
      lr_fs->os_file_system, (WT_SESSION *)session, path, lr_fh->file_type, flags, &fh));
    lr_fh->destination.fh = fh;
    lr_fh->destination.back_pointer = lr_fs;

    /* Get the list of holes of the file that need copying across from the source directory. */
    WT_ERR(__live_restore_fh_find_holes_in_dest_file(session, path, lr_fh));
    WT_ERR(__live_restore_handle_verify_hole_list(session, lr_fs, lr_fh, name));

err:
    __wt_free(session, path);
    return (ret);
}

/*
 * __live_restore_setup_lr_fh_directory --
 *     Populate a live restore file handle for a directory. Directories have special handling. If
 *     they don't exist in the destination they'll be created immediately (but not their contents)
 *     and immediately marked as complete. WiredTiger will never create or destroy a directory so we
 *     don't need to think about tombstones.
 */
static int
__live_restore_setup_lr_fh_directory(WT_SESSION_IMPL *session, WTI_LIVE_RESTORE_FS *lr_fs,
  const char *name, uint32_t flags, WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh)
{
    WT_DECL_RET;
    char *dest_folder_path = NULL;
    bool dest_exist = false, source_exist = false;

    WT_RET_NOTFOUND_OK(
      __live_restore_fs_has_file(lr_fs, &lr_fs->destination, session, name, &dest_exist));
    WT_RET_NOTFOUND_OK(
      __live_restore_fs_has_file(lr_fs, &lr_fs->source, session, name, &source_exist));

    if (!dest_exist && !source_exist && !LF_ISSET(WT_FS_OPEN_CREATE))
        WT_RET_MSG(session, ENOENT, "Directory %s does not exist in source or destination", name);

    WT_RET(__live_restore_create_file_path(
      session, &lr_fs->destination, (char *)name, &dest_folder_path));

    if (!dest_exist) {
        /*
         * The directory doesn't exist in the destination yet. We need to create it in all cases.
         * Our underlying posix file system doesn't support creating folders via WT_FS_OPEN_CREATE
         * so we create it manually.
         *
         * FIXME-WT-13971 Defaulting to permissions 0755. If the folder exists in the source should
         * we copy the permissions from the source?
         */
        mkdir(dest_folder_path, 0755);
    }

    WT_FILE_HANDLE *fh;
    WT_ERR(lr_fs->os_file_system->fs_open_file(lr_fs->os_file_system, (WT_SESSION *)session,
      dest_folder_path, lr_fh->file_type, flags, &fh));

    lr_fh->destination.fh = fh;

    /* There's no need for a hole list. The directory has already been fully copied */
    lr_fh->destination.hole_list_head = NULL;
    lr_fh->destination.back_pointer = lr_fs;
    lr_fh->destination.complete = true;

err:
    __wt_free(session, dest_folder_path);
    return (ret);
}

/*
 * __live_restore_setup_lr_fh_file --
 *     Populate a live restore file handle for a normal (non-directory) file.
 */
static int
__live_restore_setup_lr_fh_file(WT_SESSION_IMPL *session, WTI_LIVE_RESTORE_FS *lr_fs,
  const char *name, uint32_t flags, WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh)
{
    bool dest_exist = false, source_exist = false, have_tombstone = false;
    WT_SESSION *wt_session = (WT_SESSION *)session;

    WT_RET_NOTFOUND_OK(
      __live_restore_fs_has_file(lr_fs, &lr_fs->destination, session, name, &dest_exist));
    WT_RET_NOTFOUND_OK(
      __live_restore_fs_has_file(lr_fs, &lr_fs->source, session, name, &source_exist));
    WT_RET(__dest_has_tombstone(lr_fs, (char *)name, session, &have_tombstone));

    if (!dest_exist && !source_exist && !LF_ISSET(WT_FS_OPEN_CREATE))
        WT_RET_MSG(session, ENOENT, "File %s does not exist in source or destination", name);

    if (!dest_exist && have_tombstone && !LF_ISSET(WT_FS_OPEN_CREATE))
        WT_RET_MSG(session, ENOENT, "File %s has been deleted in the destination", name);

    /* Open it in the destination layer. */
    WT_RET(__live_restore_fs_open_in_destination(lr_fs, session, lr_fh, name, flags, !dest_exist));

    if (have_tombstone) {
        /*
         * Set the complete flag, we know that if there is a tombstone we should never look in the
         * source. Therefore the destination must be complete.
         */
        lr_fh->destination.complete = true;
        /* Opening files is single threaded but the remove extlist free requires the lock. */
        WTI_WITH_LIVE_RESTORE_EXTENT_LIST_WRITE_LOCK(
          session, lr_fh, __live_restore_fs_free_extent_list(session, lr_fh));
    } else {
        /*
         * If it exists in the source, open it. If it doesn't exist in the source then by definition
         * the destination file is complete.
         */
        WT_RET_NOTFOUND_OK(
          __live_restore_fs_has_file(lr_fs, &lr_fs->source, session, name, &source_exist));
        if (source_exist) {
            WT_RET(__live_restore_fs_open_in_source(lr_fs, session, lr_fh, flags));

            if (!dest_exist) {
                /*
                 * We're creating a new destination file which is backed by a source file. It
                 * currently has a length of zero, but we want its length to be the same as the
                 * source file.
                 */
                wt_off_t source_size;

                /* FIXME-WT-13971 - Determine if we should copy file permissions from the source. */

                WT_RET(lr_fh->source->fh_size(lr_fh->source, wt_session, &source_size));
                __wt_verbose_debug1(session, WT_VERB_LIVE_RESTORE,
                  "Creating destination file backed by source file. Copying size (%" PRId64
                  ") from source file",
                  source_size);
                lr_fh->source_size = (size_t)source_size;

                /*
                 * Set size by truncating. This is a positive length truncate so it actually extends
                 * the file. We're bypassing the live_restore layer so we don't try to modify the
                 * extents in hole_list_head.
                 */
                WT_RET(lr_fh->destination.fh->fh_truncate(
                  lr_fh->destination.fh, wt_session, source_size));

                /*
                 * Initialize the extent as one hole covering the entire file. We need to read
                 * everything from source.
                 */
                WT_ASSERT(session, lr_fh->destination.hole_list_head == NULL);
                WT_RET(__live_restore_alloc_extent(
                  session, 0, (size_t)source_size, NULL, &lr_fh->destination.hole_list_head));
            }
        } else
            lr_fh->destination.complete = true;
    }

    return (0);
}

/*
 * __live_restore_fs_open_file --
 *     Open a live restore file handle. This will: - If the file exists in the source, open it in
 *     both. - If it doesn't exist it'll only open it in the destination.
 */
static int
__live_restore_fs_open_file(WT_FILE_SYSTEM *fs, WT_SESSION *wt_session, const char *name,
  WT_FS_OPEN_FILE_TYPE file_type, uint32_t flags, WT_FILE_HANDLE **file_handlep)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session = (WT_SESSION_IMPL *)wt_session;
    WTI_LIVE_RESTORE_FS *lr_fs = (WTI_LIVE_RESTORE_FS *)fs;

    /* Set up the file handle. */
    WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh = NULL;
    WT_ERR(__wt_calloc_one(session, &lr_fh));
    WT_ERR(__wt_strdup(session, name, &lr_fh->iface.name));
    lr_fh->iface.file_system = fs;
    lr_fh->file_type = file_type;

    /* Initialize the jump table. */
    lr_fh->iface.close = __live_restore_fh_close;
    lr_fh->iface.fh_lock = __live_restore_fh_lock;
    lr_fh->iface.fh_read = __live_restore_fh_read;
    lr_fh->iface.fh_size = __live_restore_fh_size;
    lr_fh->iface.fh_sync = __live_restore_fh_sync;
    lr_fh->iface.fh_truncate = __live_restore_fh_truncate;
    lr_fh->iface.fh_write = __live_restore_fh_write;

    /* FIXME-WT-13820: These are unimplemented. */
    lr_fh->iface.fh_advise = NULL;
    lr_fh->iface.fh_sync_nowait = NULL;
    lr_fh->iface.fh_unmap = NULL;
    lr_fh->iface.fh_map_preload = NULL;
    lr_fh->iface.fh_map_discard = NULL;
    lr_fh->iface.fh_map = NULL;
    lr_fh->iface.fh_extend = NULL;
    lr_fh->iface.fh_extend_nolock = NULL;

    WT_ERR(__wt_rwlock_init(session, &lr_fh->ext_lock));

    /* FIXME-WT-13823 Handle the exclusive flag and other flags */

    if (file_type == WT_FS_OPEN_FILE_TYPE_DIRECTORY)
        WT_ERR(__live_restore_setup_lr_fh_directory(session, lr_fs, name, flags, lr_fh));
    else
        WT_ERR(__live_restore_setup_lr_fh_file(session, lr_fs, name, flags, lr_fh));

    *file_handlep = (WT_FILE_HANDLE *)lr_fh;

    if (0) {
err:
        if (lr_fh != NULL)
            WT_RET(__live_restore_fh_close((WT_FILE_HANDLE *)lr_fh, wt_session));
    }
    return (ret);
}

/*
 * __live_restore_fs_remove --
 *     Remove a file. We can only delete from the destination directory anyway.
 */
static int
__live_restore_fs_remove(
  WT_FILE_SYSTEM *fs, WT_SESSION *wt_session, const char *name, uint32_t flags)
{
    WT_DECL_RET;
    WTI_LIVE_RESTORE_FS *lr_fs;
    WTI_LIVE_RESTORE_FS_LAYER_TYPE layer;
    WT_SESSION_IMPL *session;
    char *path;
    bool exist;

    session = (WT_SESSION_IMPL *)wt_session;
    lr_fs = (WTI_LIVE_RESTORE_FS *)fs;

    exist = false;
    path = NULL;

    WT_RET(__live_restore_fs_find_layer(fs, session, name, &layer, &exist));
    if (!exist)
        return (0);

    /*
     * It's possible to call remove on a file that hasn't yet been created in the destination. In
     * these cases we only need to create the tombstone.
     */
    if (layer == WTI_LIVE_RESTORE_FS_LAYER_DESTINATION) {
        WT_ERR(__live_restore_fs_backing_filename(
          &lr_fs->destination, session, lr_fs->destination.home, name, &path));
        lr_fs->os_file_system->fs_remove(lr_fs->os_file_system, wt_session, path, flags);
    }

    /*
     * The tombstone here is useful as it tells us that we will never need to look in the source for
     * this file in the future. One such case is when a file is created, removed and then created
     * again with the same name.
     */
    __live_restore_fs_create_tombstone(fs, session, name, flags);

err:
    __wt_free(session, path);
    return (ret);
}

/*
 * __live_restore_fs_rename --
 *     Rename a file.
 */
static int
__live_restore_fs_rename(
  WT_FILE_SYSTEM *fs, WT_SESSION *wt_session, const char *from, const char *to, uint32_t flags)
{
    WT_DECL_RET;
    WTI_LIVE_RESTORE_FS *lr_fs;
    WTI_LIVE_RESTORE_FS_LAYER_TYPE which;
    WT_SESSION_IMPL *session;
    char *path_from, *path_to;
    bool exist;

    session = (WT_SESSION_IMPL *)wt_session;
    lr_fs = (WTI_LIVE_RESTORE_FS *)fs;

    exist = false;
    path_from = NULL;
    path_to = NULL;

    /*
     * WiredTiger frequently renames the turtle file, and some other files. This function is more
     * critical than it may seem at first.
     */
    __wt_verbose_debug1(
      session, WT_VERB_LIVE_RESTORE, "LIVE_RESTORE: Renaming file from: %s to %s", from, to);
    WT_RET(__live_restore_fs_find_layer(fs, session, from, &which, &exist));
    if (!exist)
        WT_RET_MSG(session, ENOENT, "Live restore cannot find: %s", from);

    /*
     * Any call to rename should succeed from WiredTiger's perspective thus if the file can't be
     * renamed as it does not exist in the destination that means something doesn't add up.
     */
    if (which != WTI_LIVE_RESTORE_FS_LAYER_DESTINATION)
        WT_RET_MSG(session, EINVAL, "Rename failed as file does not exist in destination");

    WT_ERR(__live_restore_fs_backing_filename(
      &lr_fs->destination, session, lr_fs->destination.home, from, &path_from));
    WT_ERR(__live_restore_fs_backing_filename(
      &lr_fs->destination, session, lr_fs->destination.home, to, &path_to));
    WT_ERR(lr_fs->os_file_system->fs_rename(
      lr_fs->os_file_system, wt_session, path_from, path_to, flags));

    /* Even if we don't modify a backing file we need to update metadata. */
    WT_ERR(__live_restore_fs_create_tombstone(fs, session, to, flags));
    WT_ERR(__live_restore_fs_create_tombstone(fs, session, from, flags));

err:
    __wt_free(session, path_from);
    __wt_free(session, path_to);
    return (ret);
}

/*
 * __live_restore_fs_size --
 *     Get the size of a file in bytes, by file name.
 */
static int
__live_restore_fs_size(
  WT_FILE_SYSTEM *fs, WT_SESSION *wt_session, const char *name, wt_off_t *sizep)
{
    WT_DECL_RET;
    WTI_LIVE_RESTORE_FS *lr_fs;
    WTI_LIVE_RESTORE_FS_LAYER_TYPE which;
    WT_SESSION_IMPL *session;
    char *path;
    bool exist;

    session = (WT_SESSION_IMPL *)wt_session;
    lr_fs = (WTI_LIVE_RESTORE_FS *)fs;

    exist = false;
    path = NULL;

    WT_RET(__live_restore_fs_find_layer(fs, session, name, &which, &exist));
    if (!exist)
        WT_RET_MSG(session, ENOENT, "Live restore cannot find: %s", name);

    /* The file will always exist in the destination. This the is authoritative file size. */
    WT_ASSERT(session, which == WTI_LIVE_RESTORE_FS_LAYER_DESTINATION);
    WT_RET(__live_restore_fs_backing_filename(
      &lr_fs->destination, session, lr_fs->destination.home, name, &path));
    ret = lr_fs->os_file_system->fs_size(lr_fs->os_file_system, wt_session, path, sizep);

    __wt_free(session, path);

    return (ret);
}

/*
 * __live_restore_fs_terminate --
 *     Terminate the file system.
 */
static int
__live_restore_fs_terminate(WT_FILE_SYSTEM *fs, WT_SESSION *wt_session)
{
    WTI_LIVE_RESTORE_FS *lr_fs;
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;
    lr_fs = (WTI_LIVE_RESTORE_FS *)fs;

    WT_ASSERT(session, lr_fs->os_file_system != NULL);
    WT_RET(lr_fs->os_file_system->terminate(lr_fs->os_file_system, wt_session));

    __wt_free(session, lr_fs->source.home);
    __wt_free(session, lr_fs);
    return (0);
}

/*
 * __validate_live_restore_path --
 *     Confirm that the given source directory is able to be opened.
 */
static int
__validate_live_restore_path(WT_FILE_SYSTEM *fs, WT_SESSION_IMPL *session, const char *path)
{
    WT_FILE_HANDLE *fh;
    /* Open the source directory. At this stage we do not validate what files it contains. */
    WT_RET(
      fs->fs_open_file(fs, (WT_SESSION *)session, path, WT_FS_OPEN_FILE_TYPE_DIRECTORY, 0, &fh));
    return (fh->close(fh, (WT_SESSION *)session));
}

/*
 * __wt_live_restore_setup_recovery --
 *     Perform necessary setup steps prior to recovery running. This is largely copying log files
 *     from the source to the destination. We also need to copy over prep log files as logging will
 *     expect these are available to be used. This needs to happen after __wt_logmgr_config to
 *     ensure the relevant logging configuration has been parsed.
 */
int
__wt_live_restore_setup_recovery(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn = S2C(session);
    WT_DECL_RET;

    WTI_LIVE_RESTORE_FS *lr_fs = (WTI_LIVE_RESTORE_FS *)conn->file_system;
    __wt_verbose_info(session, WT_VERB_LIVE_RESTORE,
      "WiredTiger started in live restore mode. Source path is: %s, Destination path is %s. The "
      "configured read size is %" WT_SIZET_FMT " bytes\n",
      lr_fs->source.home, lr_fs->destination.home, lr_fs->read_size);

    if (!F_ISSET(&conn->log_mgr, WT_LOG_CONFIG_ENABLED))
        return (0);

    u_int logcount = 0;
    char **logfiles = NULL;
    WT_DECL_ITEM(filename);
    WT_FH *fh = NULL;
    uint32_t lognum;

    /* Get a list of actual log files. */
    WT_ERR(__wt_log_get_files(session, WT_LOG_FILENAME, &logfiles, &logcount));
    for (u_int i = 0; i < logcount; i++) {
        WT_ERR(__wt_log_extract_lognum(session, logfiles[i], &lognum));
        /* Call log open file to generate the full path to the log file. */
        WT_ERR(__wt_log_openfile(session, lognum, 0, &fh));
        /*
         * We intentionally do not call the WiredTiger copy and sync function as we are copying
         * between layers and that function copies between two paths. This is the same "path" from
         * the perspective of a function higher in the stack.
         */
        ret = __wti_live_restore_fs_fill_holes(fh->handle, (WT_SESSION *)session);
        WT_TRET(__wt_close(session, &fh));
        WT_ERR(ret);
    }
    WT_ERR(__wt_fs_directory_list_free(session, &logfiles, logcount));

    /* Get a list of prep log files. */
    WT_ERR(__wt_log_get_files(session, WTI_LOG_PREPNAME, &logfiles, &logcount));
    for (u_int i = 0; i < logcount; i++) {
        WT_ERR(__wt_scr_alloc(session, 0, &filename));
        WT_ERR(__wt_log_extract_lognum(session, logfiles[i], &lognum));
        /* We cannot utilize the log open file code as it doesn't support prep logs. */
        WT_ERR(__wt_log_filename(session, lognum, WTI_LOG_PREPNAME, filename));
        WT_ERR(__wt_open(
          session, (char *)filename->data, WT_FS_OPEN_FILE_TYPE_LOG, WT_FS_OPEN_ACCESS_SEQ, &fh));
        ret = __wti_live_restore_fs_fill_holes(fh->handle, (WT_SESSION *)session);
        WT_TRET(__wt_close(session, &fh));
        WT_ERR(ret);
    }

err:
    WT_TRET(__wt_fs_directory_list_free(session, &logfiles, logcount));
    __wt_scr_free(session, &filename);
    return (ret);
}

/*
 * __wt_os_live_restore_fs --
 *     Initialize a live restore file system configuration.
 */
int
__wt_os_live_restore_fs(
  WT_SESSION_IMPL *session, const char *cfg[], const char *destination, WT_FILE_SYSTEM **fsp)
{
    WT_DECL_RET;
    WTI_LIVE_RESTORE_FS *lr_fs;

    WT_RET(__wt_calloc_one(session, &lr_fs));
    WT_ERR(__wt_os_posix(session, &lr_fs->os_file_system));

    /* Initialize the FS jump table. */
    lr_fs->iface.fs_directory_list = __live_restore_fs_directory_list;
    lr_fs->iface.fs_directory_list_single = __live_restore_fs_directory_list_single;
    lr_fs->iface.fs_directory_list_free = __live_restore_fs_directory_list_free;
    lr_fs->iface.fs_exist = __live_restore_fs_exist;
    lr_fs->iface.fs_open_file = __live_restore_fs_open_file;
    lr_fs->iface.fs_remove = __live_restore_fs_remove;
    lr_fs->iface.fs_rename = __live_restore_fs_rename;
    lr_fs->iface.fs_size = __live_restore_fs_size;
    lr_fs->iface.terminate = __live_restore_fs_terminate;

    /* Initialize the layers. */
    lr_fs->destination.home = destination;
    lr_fs->destination.which = WTI_LIVE_RESTORE_FS_LAYER_DESTINATION;

    WT_CONFIG_ITEM cval;
    WT_ERR(__wt_config_gets(session, cfg, "live_restore.path", &cval));
    WT_ERR(__wt_strndup(session, cval.str, cval.len, &lr_fs->source.home));

    WT_ERR(__validate_live_restore_path(lr_fs->os_file_system, session, lr_fs->source.home));

    lr_fs->source.which = WTI_LIVE_RESTORE_FS_LAYER_SOURCE;

    /* Configure the background thread count maximum. */
    WT_ERR(__wt_config_gets(session, cfg, "live_restore.threads_max", &cval));
    lr_fs->background_threads_max = (uint8_t)cval.val;

    /* Configure the read size. */
    WT_ERR(__wt_config_gets(session, cfg, "live_restore.read_size", &cval));
    lr_fs->read_size = (uint64_t)cval.val;
    if (!__wt_ispo2((uint32_t)lr_fs->read_size))
        WT_ERR_MSG(session, EINVAL, "the live restore read size must be a power of two");

    /* Update the callers pointer. */
    *fsp = (WT_FILE_SYSTEM *)lr_fs;

    /* Flag that a live restore file system is in use. */
    F_SET(S2C(session), WT_CONN_LIVE_RESTORE_FS);
    if (0) {
err:
        __wt_free(session, lr_fs->source.home);
        __wt_free(session, lr_fs);
    }
    return (ret);
}
