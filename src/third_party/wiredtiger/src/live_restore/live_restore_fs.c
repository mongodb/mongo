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

/*
 * __live_restore_fs_backing_filename --
 *     Convert a live restore file path (e..g WT_TEST/WiredTiger.wt) to the actual path of the
 *     backing file. This can be the file in the destination directory (which is identical to the
 *     live restore path), or the file in the source directory. The function allocates memory for
 *     the path string and expects the caller to free it.
 */
static int
__live_restore_fs_backing_filename(
  WT_LIVE_RESTORE_FS_LAYER *layer, WT_SESSION_IMPL *session, const char *name, char **pathp)
{
    WT_DECL_RET;
    size_t len;
    char *buf, *temp_name;

    temp_name = buf = NULL;

    if (__wt_absolute_path(name))
        WT_RET_MSG(session, EINVAL, "Not a relative pathname: %s", name);

    if (layer->which == WT_LIVE_RESTORE_FS_LAYER_DESTINATION) {
        WT_RET(__wt_strdup(session, name, pathp));
    } else {
        char *filename;
        /*
         * On MacOS basename takes a non-const original string. Make a local copy on the off chance
         * it modifies the string.
         */
        WT_ERR(__wt_strdup(session, name, &temp_name));
        /*
         * By default the live restore file path is identical to the file in the destination
         * directory, which will include the destination folder. We need to replace this destination
         * folder's path with the source directory's path.
         */
        filename = basename(temp_name);
        /* +1 for the path separator, +1 for the null terminator. */
        len = strlen(layer->home) + 1 + strlen(filename) + 1;
        WT_ERR(__wt_calloc(session, 1, len, &buf));
        WT_ERR(__wt_snprintf(buf, len, "%s%s%s", layer->home, __wt_path_separator(), filename));

        *pathp = buf;
        __wt_verbose_debug3(session, WT_VERB_FILEOPS,
          "Generated SOURCE path: %s\n layer->home = %s, name = %s\n", buf, layer->home, name);
    }

    if (0) {
err:
        __wt_free(session, buf);
    }
    __wt_free(session, temp_name);
    return (ret);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
/*
 * __live_restore_debug_dump_extent_list --
 *     Dump the contents of a file handle's extent list.
 */
static void
__live_restore_debug_dump_extent_list(WT_SESSION_IMPL *session, WT_LIVE_RESTORE_FILE_HANDLE *lr_fh)
{
    WT_LIVE_RESTORE_HOLE_LIST *hole;
    WT_LIVE_RESTORE_HOLE_LIST *prev;
    bool list_valid;

    prev = NULL;
    __wt_verbose_debug1(
      session, WT_VERB_FILEOPS, "Dumping extent list for %s\n", lr_fh->iface.name);
    hole = lr_fh->destination.hole_list;
    list_valid = true;

    while (hole != NULL) {

        /* Sanity check. This hole doesn't overlap with the previous hole */
        if (prev != NULL) {
            if (WT_EXTENT_END(prev) >= hole->off) {
                __wt_verbose_debug1(session, WT_VERB_FILEOPS,
                  "Error: Holes overlap prev: %" PRId64 "-%" PRId64 ", hole: %" PRId64 "-%" PRId64
                  "\n",
                  prev->off, WT_EXTENT_END(prev), hole->off, WT_EXTENT_END(hole));
                list_valid = false;
            }
        }
        __wt_verbose_debug1(
          session, WT_VERB_FILEOPS, "Hole: %" PRId64 "-%" PRId64, hole->off, WT_EXTENT_END(hole));

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
    WT_LIVE_RESTORE_FS *lr_fs;
    uint32_t open_flags;
    char *path, *path_marker;

    lr_fs = (WT_LIVE_RESTORE_FS *)fs;
    path = path_marker = NULL;

    WT_ERR(__live_restore_fs_backing_filename(&lr_fs->destination, session, name, &path));
    WT_ERR(__live_restore_create_tombstone_path(
      session, path, WT_LIVE_RESTORE_FS_TOMBSTONE_SUFFIX, &path_marker));

    open_flags = WT_FS_OPEN_CREATE;
    if (LF_ISSET(WT_FS_DURABLE | WT_FS_OPEN_DURABLE))
        FLD_SET(open_flags, WT_FS_OPEN_DURABLE);

    WT_ERR(lr_fs->os_file_system->fs_open_file(lr_fs->os_file_system, &session->iface, path_marker,
      WT_FS_OPEN_FILE_TYPE_DATA, open_flags, &fh));
    WT_ERR(fh->close(fh, &session->iface));

    __wt_verbose_debug2(session, WT_VERB_FILEOPS, "Creating tombstone: %s", path_marker);

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
__dest_has_tombstone(WT_LIVE_RESTORE_FILE_HANDLE *lr_fh, WT_SESSION_IMPL *session, bool *existp)
{
    WT_DECL_RET;
    WT_LIVE_RESTORE_FS *lr_fs;
    char *path_marker;

    lr_fs = lr_fh->destination.back_pointer;
    path_marker = NULL;

    WT_ERR(__live_restore_create_tombstone_path(
      session, lr_fh->destination.fh->name, WT_LIVE_RESTORE_FS_TOMBSTONE_SUFFIX, &path_marker));

    lr_fs->os_file_system->fs_exist(
      lr_fs->os_file_system, (WT_SESSION *)session, path_marker, existp);
    __wt_verbose_debug2(session, WT_VERB_FILEOPS, "Tombstone check for %s (Y/N)? %s",
      lr_fh->destination.fh->name, *existp ? "Y" : "N");

err:
    __wt_free(session, path_marker);
    return (ret);
}

/*
 * __live_restore_fs_has_file --
 *     Set a boolean to indicate if the given file name exists in the provided layer.
 */
static int
__live_restore_fs_has_file(WT_LIVE_RESTORE_FS *lr_fs, WT_LIVE_RESTORE_FS_LAYER *layer,
  WT_SESSION_IMPL *session, const char *name, bool *existsp)
{
    WT_DECL_RET;
    char *path;

    path = NULL;

    WT_ERR(__live_restore_fs_backing_filename(layer, session, name, &path));
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
  WT_LIVE_RESTORE_FS_LAYER_TYPE *whichp, bool *existp)
{
    WT_LIVE_RESTORE_FS *lr_fs;

    WT_ASSERT(session, existp != NULL);

    *existp = false;
    lr_fs = (WT_LIVE_RESTORE_FS *)fs;

    WT_RET(__live_restore_fs_has_file(lr_fs, &lr_fs->destination, session, name, existp));
    if (*existp) {
        /* The file exists in the destination we don't need to look any further. */
        if (whichp != NULL)
            *whichp = WT_LIVE_RESTORE_FS_LAYER_DESTINATION;
        return (0);
    }

    WT_RET(__live_restore_fs_has_file(lr_fs, &lr_fs->source, session, name, existp));
    if (*existp) {
        /* The file exists in the source we don't need to look any further. */
        if (whichp != NULL)
            *whichp = WT_LIVE_RESTORE_FS_LAYER_SOURCE;
    }

    return (0);
}

/*
 * __live_restore_fs_notsup --
 *     Return an error message indicating the given functionality is not supported.
 */
static int
__live_restore_fs_notsup(WT_SESSION *wt_session)
{
    WT_RET_MSG((WT_SESSION_IMPL *)wt_session, ENOTSUP, "Unsupported fs operation");
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
/*
 * __live_restore_fs_directory_list --
 *     Get a list of files from a directory.
 */
static int
__live_restore_fs_directory_list(WT_FILE_SYSTEM *fs, WT_SESSION *wt_session, const char *directory,
  const char *prefix, char ***dirlistp, uint32_t *countp)
{
    return (__live_restore_fs_notsup(wt_session));
}

/*
 * __live_restore_fs_directory_list_single --
 *     Get one file from a directory.
 */
static int
__live_restore_fs_directory_list_single(WT_FILE_SYSTEM *fs, WT_SESSION *wt_session,
  const char *directory, const char *prefix, char ***dirlistp, uint32_t *countp)
{
    return (__live_restore_fs_notsup(wt_session));
}

/*
 * __live_restore_fs_directory_list_free --
 *     Free memory returned by the directory listing.
 */
static int
__live_restore_fs_directory_list_free(
  WT_FILE_SYSTEM *fs, WT_SESSION *wt_session, char **dirlist, uint32_t count)
{
    return (__live_restore_fs_notsup(wt_session));
}
#pragma GCC diagnostic pop

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
  WT_LIVE_RESTORE_HOLE_LIST *next, WT_LIVE_RESTORE_HOLE_LIST **holep)
{
    WT_LIVE_RESTORE_HOLE_LIST *new;

    WT_RET(__wt_calloc_one(session, &new));
    new->off = offset;
    new->len = len;
    new->next = next;

    *holep = new;
    return (0);
}

/*
 * __live_restore_fs_free_extent_list --
 *     Free the extents associated with a live restore file handle.
 */
static void
__live_restore_fs_free_extent_list(WT_SESSION_IMPL *session, WT_LIVE_RESTORE_FILE_HANDLE *lr_fh)
{
    WT_LIVE_RESTORE_HOLE_LIST *hole;
    WT_LIVE_RESTORE_HOLE_LIST *temp;

    hole = lr_fh->destination.hole_list;
    lr_fh->destination.hole_list = NULL;

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
    WT_LIVE_RESTORE_FILE_HANDLE *lr_fh;

    lr_fh = (WT_LIVE_RESTORE_FILE_HANDLE *)fh;
    WT_ASSERT((WT_SESSION_IMPL *)wt_session, lr_fh->destination.fh != NULL);
    return (lr_fh->destination.fh->fh_lock(lr_fh->destination.fh, wt_session, lock));
}

/*
 * __live_restore_remove_extlist_hole --
 *     Track that we wrote something by removing its hole from the extent list.
 */
static int
__live_restore_remove_extlist_hole(
  WT_LIVE_RESTORE_FILE_HANDLE *lr_fh, WT_SESSION_IMPL *session, wt_off_t offset, size_t len)
{
    WT_LIVE_RESTORE_HOLE_LIST *hole, *tmp, *new, *prev_hole;
    wt_off_t write_end;

    __wt_verbose_debug2(session, WT_VERB_FILEOPS, "REMOVE HOLE %s: %" PRId64 "-%" PRId64,
      lr_fh->iface.name, offset, WT_OFFSET_END(offset, len));

    write_end = WT_OFFSET_END(offset, len);

    /* FIXME-WT-13825 - We need to make sure we're thread safe when touching the hole_list. */
    hole = lr_fh->destination.hole_list;
    prev_hole = NULL;
    while (hole != NULL) {

        if (write_end < hole->off) {
            /* We won't find any more overlapping holes. Stop searching. */
            break;
        }

        if (offset <= hole->off && write_end >= WT_EXTENT_END(hole)) {
            /* The write fully overlaps a hole. Delete it. */
            __wt_verbose_debug3(session, WT_VERB_FILEOPS,
              "Fully overlaps hole %" PRId64 "-%" PRId64, hole->off, WT_EXTENT_END(hole));

            tmp = hole;
            if (prev_hole == NULL)
                lr_fh->destination.hole_list = hole->next;
            else
                prev_hole->next = hole->next;
            hole = hole->next;
            __wt_free(session, tmp);
            continue;

        } else if (offset > hole->off && write_end < WT_EXTENT_END(hole)) {
            /* The write is entirely within the hole. Split the hole in two. */

            __wt_verbose_debug3(session, WT_VERB_FILEOPS,
              "Fully contained by hole %" PRId64 "-%" PRId64, hole->off, WT_EXTENT_END(hole));

            /* First create the hole to the right of the write. */
            WT_RET(__live_restore_alloc_extent(
              session, write_end + 1, (size_t)(WT_EXTENT_END(hole) - write_end), hole->next, &new));

            /*
             * Then shrink the existing hole so it's to the left of the write and point it at the
             * new hole.
             */
            hole->len = (size_t)(offset - hole->off);
            hole->next = new;

        } else if (offset <= hole->off && WT_OFFSET_IN_EXTENT(write_end, hole)) {
            /* The write starts before the hole and ends within it. Shrink the hole. */
            __wt_verbose_debug3(session, WT_VERB_FILEOPS,
              "Partial overlap to the left of hole %" PRId64 "-%" PRId64, hole->off,
              WT_EXTENT_END(hole));

            hole->len = (size_t)(WT_EXTENT_END(hole) - write_end);
            hole->off = write_end + 1;

        } else if (WT_OFFSET_IN_EXTENT(offset, hole) && write_end >= WT_EXTENT_END(hole)) {
            __wt_verbose_debug3(session, WT_VERB_FILEOPS,
              "Partial overlap to the right of hole %" PRId64 "-%" PRId64, hole->off,
              WT_EXTENT_END(hole));
            /* The write starts within the hole and ends after it. Shrink the hole. */
            hole->len = (size_t)(offset - hole->off);

        } else
            /* No overlap. Safety check */
            WT_ASSERT(session, write_end < hole->off || offset > WT_EXTENT_END(hole));

        prev_hole = hole;
        hole = hole->next;
    }
    return (0);
}

/*
 * __live_restore_can_service_read --
 *     Return if a read can be serviced by the destination file. This assumes that the block manager
 *     is the only thing that perform reads and it only reads and writes full blocks. If that
 *     changes this code will unceremoniously fall over.
 */
static bool
__live_restore_can_service_read(
  WT_LIVE_RESTORE_FILE_HANDLE *lr_fh, WT_SESSION_IMPL *session, wt_off_t offset, size_t len)
{
    WT_LIVE_RESTORE_HOLE_LIST *hole;
    wt_off_t read_end;
    bool read_begins_in_hole, read_ends_in_hole;

    read_end = WT_OFFSET_END(offset, len);

    hole = lr_fh->destination.hole_list;
    while (hole != NULL) {

        if (read_end < hole->off)
            /* All subsequent holes are past the read. We won't find matching holes */
            break;

        read_begins_in_hole = WT_OFFSET_IN_EXTENT(offset, hole);
        read_ends_in_hole = WT_OFFSET_IN_EXTENT(read_end, hole);
        if (read_begins_in_hole && read_ends_in_hole) {
            /* Our read is entirely within a hole */
            __wt_verbose_debug3(session, WT_VERB_FILEOPS,
              "CANNOT SERVICE %s: Reading from hole. Read: %" PRId64 "-%" PRId64 ", hole: %" PRId64
              "-%" PRId64,
              lr_fh->iface.name, offset, read_end, hole->off, WT_EXTENT_END(hole));
            return (false);
        } else if (read_begins_in_hole != read_ends_in_hole) {
            /*
             * The read starts in a hole but doesn't finish in it, or vice versa. This breaks
             * assumptions we make about how the block manager works and is intentionally
             * unimplemented.
             */
            WT_ASSERT_ALWAYS(session, false, "Read partially covers a hole");
        }

        hole = hole->next;
    }

    __wt_verbose_debug3(
      session, WT_VERB_FILEOPS, "CAN SERVICE %s: No hole found", lr_fh->iface.name);
    return (true);
}

/*
 * __live_restore_fh_write --
 *     File write.
 */
static int
__live_restore_fh_write(
  WT_FILE_HANDLE *fh, WT_SESSION *wt_session, wt_off_t offset, size_t len, const void *buf)
{
    WT_LIVE_RESTORE_FILE_HANDLE *lr_fh;
    WT_SESSION_IMPL *session;

    lr_fh = (WT_LIVE_RESTORE_FILE_HANDLE *)fh;
    session = (WT_SESSION_IMPL *)wt_session;

    __wt_verbose_debug1(
      session, WT_VERB_FILEOPS, "WRITE %s: %" PRId64 ", %lu", fh->name, offset, len);
    WT_RET(lr_fh->destination.fh->fh_write(lr_fh->destination.fh, wt_session, offset, len, buf));
    WT_RET(lr_fh->destination.fh->fh_sync(lr_fh->destination.fh, wt_session));
    WT_RET(__live_restore_remove_extlist_hole(lr_fh, session, offset, len));
    return (0);
}

/*
 * __read_promote --
 *     Write out the contents of a read into the destination. This will be overkill for cases where
 *     a read is performed to service a write.
 */
static int
__read_promote(WT_LIVE_RESTORE_FILE_HANDLE *lr_fh, WT_SESSION_IMPL *session, wt_off_t offset,
  size_t len, char *read)
{
    __wt_verbose_debug2(session, WT_VERB_FILEOPS, "    READ PROMOTE %s : %" PRId64 ", %lu",
      lr_fh->iface.name, offset, len);
    WT_RET(
      __live_restore_fh_write((WT_FILE_HANDLE *)lr_fh, (WT_SESSION *)session, offset, len, read));

    return (0);
}

/*
 * __live_restore_fh_read --
 *     File read in a live restore file system.
 */
static int
__live_restore_fh_read(
  WT_FILE_HANDLE *fh, WT_SESSION *wt_session, wt_off_t offset, size_t len, void *buf)
{
    WT_LIVE_RESTORE_FILE_HANDLE *lr_fh;
    WT_SESSION_IMPL *session;
    char *read_data;

    lr_fh = (WT_LIVE_RESTORE_FILE_HANDLE *)fh;
    session = (WT_SESSION_IMPL *)wt_session;

    __wt_verbose_debug1(
      session, WT_VERB_FILEOPS, "READ %s : %" PRId64 ", %lu", fh->name, offset, len);

    read_data = (char *)buf;

    /*
     * FIXME-WT-13828: WiredTiger will read the metadata file after creation but before anything has
     * been written in this case we forward the read to the empty metadata file in the destination.
     * Is this correct?
     */
    if (lr_fh->destination.complete || lr_fh->source == NULL ||
      __live_restore_can_service_read(lr_fh, session, offset, len)) {
        /*
         * FIXME-WT-13797: Right now if complete is true source will always be null. So the if
         * statement here has redundancy is there a time when we need it? Maybe with the background
         * thread.
         */
        __wt_verbose_debug2(session, WT_VERB_FILEOPS, "    READ FROM DEST (src is NULL? %s)",
          lr_fh->source == NULL ? "YES" : "NO");
        /* Read the full read from the destination. */
        WT_RET(lr_fh->destination.fh->fh_read(
          lr_fh->destination.fh, wt_session, offset, len, read_data));
    } else {
        /* Interestingly you cannot not have a format in verbose. */
        __wt_verbose_debug2(session, WT_VERB_FILEOPS, "    READ FROM %s", "SOURCE");
        /* Read the full read from the source. */
        WT_RET(lr_fh->source->fh_read(lr_fh->source, wt_session, offset, len, read_data));
        /* Promote the read */
        WT_RET(__read_promote(lr_fh, session, offset, len, read_data));
    }

    return (0);
}

/*
 * __live_restore_fs_fill_holes_on_file_close --
 *     On file close make sure we've copied across all data from source to destination. This means
 *     there are no holes in the destination file's extent list. If we find one promote read the
 *     content into the destination.
 *
 * NOTE!! This assumes there cannot be holes in source, and that any truncates/extensions of the
 *     destination file are already handled elsewhere.
 */
static int
__live_restore_fs_fill_holes_on_file_close(WT_FILE_HANDLE *fh, WT_SESSION *wt_session)
{
    WT_LIVE_RESTORE_FILE_HANDLE *lr_fh;
    WT_LIVE_RESTORE_HOLE_LIST *hole;
    /*
     * FIXME-WT-13810 Using 4MB buffer as a placeholder. When we find a large hole we should break
     * the read into small chunks
     */
    char buf[4096000];

    lr_fh = (WT_LIVE_RESTORE_FILE_HANDLE *)fh;
    hole = lr_fh->destination.hole_list;

    while (hole != NULL) {
        __wt_verbose_debug3((WT_SESSION_IMPL *)wt_session, WT_VERB_FILEOPS,
          "Found hole in %s at %" PRId64 "-%" PRId64 " during file close. Filling", fh->name,
          hole->off, WT_EXTENT_END(hole));
        WT_RET(__live_restore_fh_read(fh, wt_session, hole->off, hole->len, buf));
        hole = lr_fh->destination.hole_list;
    }

    return (0);
}

/*
 * __live_restore_fh_close --
 *     Close the file.
 */
static int
__live_restore_fh_close(WT_FILE_HANDLE *fh, WT_SESSION *wt_session)
{
    WT_LIVE_RESTORE_FILE_HANDLE *lr_fh;
    WT_SESSION_IMPL *session;

    lr_fh = (WT_LIVE_RESTORE_FILE_HANDLE *)fh;
    session = (WT_SESSION_IMPL *)wt_session;
    __wt_verbose_debug1(session, WT_VERB_FILEOPS, "LIVE_RESTORE_FS: Closing file: %s\n", fh->name);

    /*
     * FIXME-WT-13809: This should be superseded by background thread migration. Right now it exists
     * as a solution to handle certain testing cases. Once the background thread is implemented the
     * test will need to handle situations where a full restore hasn't completed by the end of the
     * test. Calling this in a production environment will produce very slow file closes as we copy
     * all remaining data to the destination.
     */
    WT_RET(__live_restore_fs_fill_holes_on_file_close(fh, wt_session));

    lr_fh->destination.fh->close(lr_fh->destination.fh, wt_session);
    __live_restore_fs_free_extent_list(session, lr_fh);

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
    WT_LIVE_RESTORE_FILE_HANDLE *lr_fh;
    wt_off_t destination_size;

    lr_fh = (WT_LIVE_RESTORE_FILE_HANDLE *)fh;

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
    WT_LIVE_RESTORE_FILE_HANDLE *lr_fh;

    lr_fh = (WT_LIVE_RESTORE_FILE_HANDLE *)fh;
    return (lr_fh->destination.fh->fh_sync(lr_fh->destination.fh, wt_session));
}

/*
 * __live_restore_fh_truncate --
 *     Truncate a file. This operation is only applied to the destination file.
 */
static int
__live_restore_fh_truncate(WT_FILE_HANDLE *fh, WT_SESSION *wt_session, wt_off_t len)
{
    WT_LIVE_RESTORE_FILE_HANDLE *lr_fh;
    wt_off_t old_len, truncate_start, truncate_end;

    lr_fh = (WT_LIVE_RESTORE_FILE_HANDLE *)fh;
    old_len = 0;
    /*
     * If we truncate a range we'll never need to read that range from the source file. Mark it as
     * such.
     */
    WT_RET(__live_restore_fh_size(fh, wt_session, &old_len));

    if (old_len == len)
        /* Sometimes we call truncate but don't change the length. Ignore */
        return (0);

    __wt_verbose_debug2((WT_SESSION_IMPL *)wt_session, WT_VERB_FILEOPS,
      "truncating file %s from %" PRId64 " to %" PRId64, fh->name, old_len, len);

    /*
     * Truncate can be used to shorten a file or to extend it. In both cases the truncated/extended
     * range doesn't need to be read from the source directory.
     */
    truncate_start = WT_MIN(len, old_len);
    truncate_end = WT_MAX(len, old_len);

    WT_RET(__live_restore_remove_extlist_hole(lr_fh, (WT_SESSION_IMPL *)wt_session, truncate_start,
      (size_t)(truncate_end - truncate_start)));

    return (lr_fh->destination.fh->fh_truncate(lr_fh->destination.fh, wt_session, len));
}

/*
 * __live_restore_fs_open_in_source --
 *     Open a file handle in the source.
 */
static int
__live_restore_fs_open_in_source(WT_LIVE_RESTORE_FS *lr_fs, WT_SESSION_IMPL *session,
  WT_LIVE_RESTORE_FILE_HANDLE *lr_fh, uint32_t flags)
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
    WT_ERR(__live_restore_fs_backing_filename(&lr_fs->source, session, lr_fh->iface.name, &path));
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
  WT_SESSION_IMPL *session, char *filename, WT_LIVE_RESTORE_FILE_HANDLE *lr_fh)
{
    WT_DECL_RET;
    wt_off_t data_end_offset, file_size;
    int fd;

    data_end_offset = 0;
    WT_SYSCALL(((fd = open(filename, O_RDONLY)) == -1 ? -1 : 0), ret);
    WT_ERR(ret);

    /* Check that we opened a valid file descriptor. */
    WT_ASSERT(session, fcntl(fd, F_GETFD) != -1 || errno != EBADF);
    WT_ERR(__live_restore_fh_size((WT_FILE_HANDLE *)lr_fh, (WT_SESSION *)session, &file_size));
    __wt_verbose_debug2(session, WT_VERB_FILEOPS, "File: %s", filename);
    __wt_verbose_debug2(session, WT_VERB_FILEOPS, "    len: %" PRId64, file_size);

    if (file_size > 0)
        /*
         * Initialize the file as one big hole. We'll then lseek the file to find data blocks and
         * remove those ranges from the hole list.
         */
        WT_ERR(__live_restore_alloc_extent(
          session, 0, (size_t)file_size, NULL, &lr_fh->destination.hole_list));

    /*
     * Find the next data block. data_end_offset is initialized to zero so we start from the
     * beginning of the file. lseek will find a block when it starts already positioned on the
     * block, so starting at zero ensures we'll find data blocks at the beginning of the file.
     */
    wt_off_t data_offset;
    while ((data_offset = lseek(fd, data_end_offset, SEEK_DATA)) != -1) {

        data_end_offset = lseek(fd, data_offset, SEEK_HOLE);
        /* All data must be followed by a hole */
        WT_ASSERT(session, data_end_offset != -1);
        WT_ASSERT(session, data_end_offset > data_offset - 1);

        __wt_verbose_debug1(session, WT_VERB_FILEOPS,
          "File: %s, has data from %" PRId64 "-%" PRId64, filename, data_offset, data_end_offset);
        WT_ERR(__live_restore_remove_extlist_hole(
          lr_fh, session, data_offset, (size_t)(data_end_offset - data_offset)));
    }

err:
    WT_SYSCALL_TRET(close(fd), ret);
    return (ret);
}

/*
 * __live_restore_fs_open_in_destination --
 *     Open a file handle.
 */
static int
__live_restore_fs_open_in_destination(WT_LIVE_RESTORE_FS *lr_fs, WT_SESSION_IMPL *session,
  WT_LIVE_RESTORE_FILE_HANDLE *lr_fh, uint32_t flags, bool create)
{
    WT_DECL_RET;
    WT_FILE_HANDLE *fh;
    char *path;

    path = NULL;

    if (create)
        flags |= WT_FS_OPEN_CREATE;

    /* Open the file in the layer. */
    WT_ERR(
      __live_restore_fs_backing_filename(&lr_fs->destination, session, lr_fh->iface.name, &path));
    WT_ERR(lr_fs->os_file_system->fs_open_file(
      lr_fs->os_file_system, (WT_SESSION *)session, path, lr_fh->file_type, flags, &fh));
    lr_fh->destination.fh = fh;
    lr_fh->destination.back_pointer = lr_fs;

    /* Get the list of holes of the file that need copying across from the source directory. */
    WT_ASSERT(session, lr_fh->file_type != WT_FS_OPEN_FILE_TYPE_DIRECTORY);
    WT_ERR(__live_restore_fh_find_holes_in_dest_file(session, path, lr_fh));
err:
    __wt_free(session, path);
    return (ret);
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
    WT_LIVE_RESTORE_FILE_HANDLE *lr_fh;
    WT_LIVE_RESTORE_FS *lr_fs;
    WT_LIVE_RESTORE_FS_LAYER_TYPE which;
    WT_SESSION_IMPL *session;
    bool dest_exist, source_exist, have_tombstone, readonly;

    session = (WT_SESSION_IMPL *)wt_session;
    lr_fs = (WT_LIVE_RESTORE_FS *)fs;

    dest_exist = source_exist = false;
    lr_fh = NULL;
    have_tombstone = false;
    WT_UNUSED(have_tombstone);
    readonly = LF_ISSET(WT_FS_OPEN_READONLY);
    WT_UNUSED(readonly);
    WT_UNUSED(which);

    /* FIXME-WT-13808 Handle WT_FS_OPEN_FILE_TYPE_DIRECTORY */

    /* Set up the file handle. */
    WT_ERR(__wt_calloc_one(session, &lr_fh));
    WT_ERR(__wt_strdup(session, name, &lr_fh->iface.name));
    lr_fh->iface.file_system = fs;
    lr_fh->file_type = file_type;

    /* FIXME-WT-13823 Handle the exclusive flag and other flags */

    /* Open it in the destination layer. */
    WT_ERR_NOTFOUND_OK(
      __live_restore_fs_has_file(lr_fs, &lr_fs->destination, session, name, &dest_exist), true);
    WT_ERR(__live_restore_fs_open_in_destination(lr_fs, session, lr_fh, flags, !dest_exist));

    WT_ERR(__dest_has_tombstone(lr_fh, session, &have_tombstone));
    if (have_tombstone) {
        /*
         * Set the complete flag, we know that if there is a tombstone we should never look in the
         * source. Therefore the destination must be complete.
         */
        lr_fh->destination.complete = true;
        __live_restore_fs_free_extent_list(session, lr_fh);
    } else {
        /*
         * If it exists in the source, open it. If it doesn't exist in the source then by definition
         * the destination file is complete.
         */
        WT_ERR_NOTFOUND_OK(
          __live_restore_fs_has_file(lr_fs, &lr_fs->source, session, name, &source_exist), true);
        if (source_exist) {
            WT_ERR(__live_restore_fs_open_in_source(lr_fs, session, lr_fh, flags));

            if (!dest_exist) {
                /*
                 * We're creating a new destination file which is backed by a source file. It
                 * currently has a length of zero, but we want its length to be the same as the
                 * source file.
                 */
                wt_off_t source_size;

                WT_ERR(lr_fh->source->fh_size(lr_fh->source, wt_session, &source_size));
                __wt_verbose_debug1(session, WT_VERB_FILEOPS,
                  "Creating destination file backed by source file. Copying size (%" PRId64
                  ") from source "
                  "file",
                  source_size);

                /*
                 * Set size by truncating. This is a positive length truncate so it actually extends
                 * the file. We're bypassing the live_restore layer so we don't try to modify the
                 * extents in hole_list.
                 */
                WT_ERR(lr_fh->destination.fh->fh_truncate(
                  lr_fh->destination.fh, wt_session, source_size));

                /*
                 * Initialize the extent as one hole covering the entire file. We need to read
                 * everything from source.
                 */
                WT_ASSERT(session, lr_fh->destination.hole_list == NULL);
                WT_ERR(__live_restore_alloc_extent(
                  session, 0, (size_t)source_size, NULL, &lr_fh->destination.hole_list));
            }
        } else
            lr_fh->destination.complete = true;
    }

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
    WT_LIVE_RESTORE_FS *lr_fs;
    WT_LIVE_RESTORE_FS_LAYER_TYPE layer;
    WT_SESSION_IMPL *session;
    char *path;
    bool exist;

    session = (WT_SESSION_IMPL *)wt_session;
    lr_fs = (WT_LIVE_RESTORE_FS *)fs;

    exist = false;
    path = NULL;

    WT_RET(__live_restore_fs_find_layer(fs, session, name, &layer, &exist));
    if (!exist)
        return (0);

    /*
     * It's possible to call remove on a file that hasn't yet been created in the destination. In
     * these cases we only need to create the tombstone.
     */
    if (layer == WT_LIVE_RESTORE_FS_LAYER_DESTINATION) {
        WT_ERR(__live_restore_fs_backing_filename(&lr_fs->destination, session, name, &path));
        lr_fs->os_file_system->fs_remove(lr_fs->os_file_system, wt_session, path, flags);
    }

    /*
     * The tombstone here is useful as it tells us that we will never need to look in the
     * destination for this file in the future. One such case is when a file is created, removed and
     * then created again with the same name.
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
    WT_LIVE_RESTORE_FS *lr_fs;
    WT_LIVE_RESTORE_FS_LAYER_TYPE which;
    WT_SESSION_IMPL *session;
    char *path_from, *path_to;
    bool exist;

    session = (WT_SESSION_IMPL *)wt_session;
    lr_fs = (WT_LIVE_RESTORE_FS *)fs;

    exist = false;
    path_from = NULL;
    path_to = NULL;

    /*
     * WiredTiger frequently renames the turtle file, and some other files. This function is more
     * critical than it may seem at first.
     */

    __wt_verbose_debug1(
      session, WT_VERB_FILEOPS, "LIVE_RESTORE: Renaming file from: %s to %s\n", from, to);
    WT_RET(__live_restore_fs_find_layer(fs, session, from, &which, &exist));
    if (!exist)
        return (ENOENT);

    if (which == WT_LIVE_RESTORE_FS_LAYER_DESTINATION) {
        WT_ERR(__live_restore_fs_backing_filename(&lr_fs->destination, session, from, &path_from));
        WT_ERR(__live_restore_fs_backing_filename(&lr_fs->destination, session, to, &path_to));
        WT_ERR(lr_fs->os_file_system->fs_rename(
          lr_fs->os_file_system, wt_session, path_from, path_to, flags));
    }

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
    WT_LIVE_RESTORE_FS *lr_fs;
    WT_LIVE_RESTORE_FS_LAYER_TYPE which;
    WT_SESSION_IMPL *session;
    char *path;
    bool exist;

    session = (WT_SESSION_IMPL *)wt_session;
    lr_fs = (WT_LIVE_RESTORE_FS *)fs;

    exist = false;
    path = NULL;

    WT_RET(__live_restore_fs_find_layer(fs, session, name, &which, &exist));
    if (!exist)
        return (ENOENT);

    /* The file will always exist in the destination. This the is authoritative file size. */
    WT_ASSERT(session, which == WT_LIVE_RESTORE_FS_LAYER_DESTINATION);
    WT_RET(__live_restore_fs_backing_filename(&lr_fs->destination, session, name, &path));
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
    WT_LIVE_RESTORE_FS *lr_fs;
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;
    lr_fs = (WT_LIVE_RESTORE_FS *)fs;

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
 * __wt_os_live_restore_fs --
 *     Initialize a live restore file system configuration.
 */
int
__wt_os_live_restore_fs(
  WT_SESSION_IMPL *session, const char *cfg[], const char *destination, WT_FILE_SYSTEM **fsp)
{
    WT_DECL_RET;
    WT_LIVE_RESTORE_FS *lr_fs;

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
    lr_fs->destination.which = WT_LIVE_RESTORE_FS_LAYER_DESTINATION;

    WT_CONFIG_ITEM cval;
    WT_ERR(__wt_config_gets(session, cfg, "live_restore.path", &cval));
    WT_ERR(__wt_strndup(session, cval.str, cval.len, &lr_fs->source.home));

    WT_ERR(__validate_live_restore_path(lr_fs->os_file_system, session, lr_fs->source.home));

    lr_fs->source.which = WT_LIVE_RESTORE_FS_LAYER_SOURCE;

    /* Configure the background thread count maximum. */
    WT_ERR(__wt_config_gets(session, cfg, "live_restore.threads_max", &cval));
    lr_fs->background_threads_max = (uint8_t)cval.val;

    __wt_verbose_debug1(session, WT_VERB_FILEOPS,
      "WiredTiger started in live restore mode! Source path is: %s, Destination path is %s",
      lr_fs->source.home, destination);

    /* Update the callers pointer. */
    *fsp = (WT_FILE_SYSTEM *)lr_fs;
    if (0) {
err:
        __wt_free(session, lr_fs->source.home);
        __wt_free(session, lr_fs);
    }
    return (ret);
}
