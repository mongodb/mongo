/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *  All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"
#include "live_restore_private.h"

WT_STAT_MSECS_HIST_INCR_FUNC(live_restore, live_restore_hist_source_read_latency)

static int __live_restore_fs_directory_list_free(
  WT_FILE_SYSTEM *fs, WT_SESSION *wt_session, char **dirlist, uint32_t count);

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
__live_restore_fs_backing_filename(WT_SESSION_IMPL *session, WTI_LIVE_RESTORE_FS *lr_fs,
  WTI_LIVE_RESTORE_FS_LAYER_TYPE layer, const char *name, char **pathp)
{
    WT_DECL_RET;
    size_t len;
    char *buf = NULL;
    const char *filename = name;

    /*
     * Name must start with the destination directory. If name is an absolute path like
     * "/home/dest_home/file.txt" then destination directory which is derived from conn->home will
     * be "/home/dest_home".
     */
    WT_ASSERT_ALWAYS(session, WT_PREFIX_MATCH(name, lr_fs->destination.home),
      "Provided name '%s' does not start with the destination home folder path '%s'", name,
      lr_fs->destination.home);

    if (layer == WTI_LIVE_RESTORE_FS_LAYER_DESTINATION) {
        WT_RET(__wt_strdup(session, filename, pathp));
    } else {
        /*
         * By default the live restore file path is identical to the file in the destination
         * directory, which will include the destination folder. We need to replace this destination
         * folder's path with the source directory's path.
         */
        filename += strlen(lr_fs->destination.home);

        /* +1 for the null terminator. */
        len = strlen(lr_fs->source.home) + strlen(filename) + 1;
        WT_ERR(__wt_calloc(session, 1, len, &buf));
        WT_ERR(__wt_snprintf(buf, len, "%s%s", lr_fs->source.home, filename));

        *pathp = buf;
        __wt_verbose_debug3(session, WT_VERB_LIVE_RESTORE,
          "Generated SOURCE path: %s. layer->home = %s, name = %s", buf, lr_fs->source.home, name);
    }

    if (0) {
err:
        __wt_free(session, buf);
    }
    return (ret);
}

/*
 * __live_restore_create_stop_file_path --
 *     Generate the stop file path for a file.
 */
static int
__live_restore_create_stop_file_path(WT_SESSION_IMPL *session, const char *name, char **out)
{
    size_t p, suffix_len;

    p = strlen(name);
    suffix_len = strlen(WTI_LIVE_RESTORE_STOP_FILE_SUFFIX);

    WT_RET(__wt_malloc(session, p + suffix_len + 1, out));
    memcpy(*out, name, p);
    memcpy(*out + p, WTI_LIVE_RESTORE_STOP_FILE_SUFFIX, suffix_len + 1);
    return (0);
}

/*
 * __live_restore_fs_create_stop_file_locked --
 *     Create a stop file for the given file. The live restore state lock must be held.
 */
static int
__live_restore_fs_create_stop_file_locked(
  WTI_LIVE_RESTORE_FS *lr_fs, WT_SESSION_IMPL *session, const char *name, uint32_t flags)
{
    WT_DECL_RET;
    WT_FILE_HANDLE *fh;
    uint32_t open_flags;
    char *path, *path_marker;
    path = path_marker = NULL;

    WT_ASSERT_SPINLOCK_OWNED(session, &lr_fs->state_lock);
    if (__wti_live_restore_migration_complete(session))
        return (0);

    WT_ERR(__live_restore_fs_backing_filename(
      session, lr_fs, WTI_LIVE_RESTORE_FS_LAYER_DESTINATION, name, &path));
    WT_ERR(__live_restore_create_stop_file_path(session, path, &path_marker));

    __wt_verbose_debug2(session, WT_VERB_LIVE_RESTORE, "Creating stop file: %s", path_marker);

    open_flags = WT_FS_OPEN_CREATE;
    if (LF_ISSET(WT_FS_DURABLE | WT_FS_OPEN_DURABLE))
        FLD_SET(open_flags, WT_FS_OPEN_DURABLE);

    WT_ERR(lr_fs->os_file_system->fs_open_file(lr_fs->os_file_system, &session->iface, path_marker,
      WT_FS_OPEN_FILE_TYPE_DATA, open_flags, &fh));
    WT_ERR(fh->close(fh, &session->iface));

err:
    __wt_free(session, path);
    __wt_free(session, path_marker);
    return (ret);
}

/*
 * __live_restore_fs_create_stop_file --
 *     Create a stop file for the given file.
 */
static int
__live_restore_fs_create_stop_file(
  WT_FILE_SYSTEM *fs, WT_SESSION_IMPL *session, const char *name, uint32_t flags)
{
    WT_DECL_RET;
    WTI_LIVE_RESTORE_FS *lr_fs = (WTI_LIVE_RESTORE_FS *)fs;
    WTI_WITH_LIVE_RESTORE_STATE_LOCK(
      session, lr_fs, ret = __live_restore_fs_create_stop_file_locked(lr_fs, session, name, flags));
    return (ret);
}

/*
 * __dest_has_stop_file --
 *     Check whether the destination directory contains a stop file for a given file.
 */
static int
__dest_has_stop_file(
  WTI_LIVE_RESTORE_FS *lr_fs, const char *name, WT_SESSION_IMPL *session, bool *existp)
{
    WT_DECL_RET;
    char *path_marker;

    path_marker = NULL;

    WT_ERR(__live_restore_create_stop_file_path(session, name, &path_marker));

    lr_fs->os_file_system->fs_exist(
      lr_fs->os_file_system, (WT_SESSION *)session, path_marker, existp);
    __wt_verbose_debug2(
      session, WT_VERB_LIVE_RESTORE, "Stop file check for %s (Y/N)? %s", name, *existp ? "Y" : "N");

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

    WT_ERR(__live_restore_fs_backing_filename(session, lr_fs, layer->which, name, &path));
    WT_ERR(lr_fs->os_file_system->fs_exist(lr_fs->os_file_system, &session->iface, path, existsp));
err:
    __wt_free(session, path);

    return (ret);
}

/*
 * __live_restore_fs_find_layer --
 *     Return which layer a file exists in, or if it doesn't exist in any layer. If a file exists in
 *     both the source and destination return it is in the destination.
 */
static int
__live_restore_fs_find_layer(WT_FILE_SYSTEM *fs, WT_SESSION_IMPL *session, const char *name,
  WTI_LIVE_RESTORE_FS_LAYER_TYPE *whichp)
{
    WTI_LIVE_RESTORE_FS *lr_fs = (WTI_LIVE_RESTORE_FS *)fs;

    WT_ASSERT(session, whichp != NULL);

    *whichp = WTI_LIVE_RESTORE_FS_LAYER_NONE;

    bool exists = false;
    WT_RET(__live_restore_fs_has_file(lr_fs, &lr_fs->destination, session, name, &exists));
    if (exists) {
        /* The file exists in the destination we don't need to look any further. */
        *whichp = WTI_LIVE_RESTORE_FS_LAYER_DESTINATION;
        return (0);
    }

    /*
     * If the file is only in the source and there is a stop file in the destination then we've
     * either moved or deleted the file in the destination, meaning it no longer exists for the
     * user.
     */
    bool has_stop = false;
    WT_RET(__dest_has_stop_file((WTI_LIVE_RESTORE_FS *)fs, name, session, &has_stop));
    if (has_stop)
        return (0);

    /*
     * If migration completes and the file isn't in the destination it must have been deleted or
     * moved by the user. We can't depend on stop files here as post-migration clean up may have
     * deleted the stop file already.
     */
    if (__wti_live_restore_migration_complete(session))
        return (0);

    WT_RET(__live_restore_fs_has_file(lr_fs, &lr_fs->source, session, name, &exists));
    if (exists) {
        *whichp = WTI_LIVE_RESTORE_FS_LAYER_SOURCE;
    }

    return (0);
}

/*
 * __live_restore_fs_directory_list_worker --
 *     The list is a combination of files from the destination and source directories. For
 *     destination files, exclude any files matching the marker paths. For source files, exclude
 *     files that have associated stop files or are already present in the destination directory.
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
    char **dirlist_dest, **dirlist_src, **entries, *path_dest, *path_src;
    bool dest_exist = false, have_stop = false;
    bool dest_folder_exists = false, source_folder_exists = false;
    uint32_t num_src_files = 0, num_dest_files = 0;
    WT_DECL_ITEM(filename);

    *dirlistp = dirlist_dest = dirlist_src = entries = NULL;
    path_dest = path_src = NULL;

    __wt_verbose_debug1(session, WT_VERB_LIVE_RESTORE,
      "DIRECTORY LIST %s (single ? %s) : ", directory, single ? "YES" : "NO");
    WT_ASSERT_SPINLOCK_OWNED(session, &lr_fs->state_lock);

    /* Get files from destination. */
    WT_ERR(__live_restore_fs_backing_filename(
      session, lr_fs, WTI_LIVE_RESTORE_FS_LAYER_DESTINATION, directory, &path_dest));

    WT_ERR(lr_fs->os_file_system->fs_exist(
      lr_fs->os_file_system, wt_session, path_dest, &dest_folder_exists));

    if (dest_folder_exists) {
        WT_ERR(lr_fs->os_file_system->fs_directory_list(
          lr_fs->os_file_system, wt_session, path_dest, prefix, &dirlist_dest, &num_dest_files));

        for (uint32_t i = 0; i < num_dest_files; ++i)
            /*
             * The caller utilizes prefix to identify files necessary to it's module. Avoid
             * returning live restore specific files to the caller.
             */
            if (!WT_SUFFIX_MATCH(dirlist_dest[i], WTI_LIVE_RESTORE_STOP_FILE_SUFFIX) &&
              !WT_SUFFIX_MATCH(dirlist_dest[i], WTI_LIVE_RESTORE_TEMP_FILE_SUFFIX)) {
                WT_ERR(__wt_realloc_def(session, &dirallocsz, count_dest + 1, &entries));
                WT_ERR(__wt_strdup(session, dirlist_dest[i], &entries[count_dest]));
                ++count_dest;

                if (single)
                    goto done;
            }
    }

    /*
     * Once we're past the background migration stage we never need to access the source directory
     * again.
     */
    if (__wti_live_restore_migration_complete(session))
        goto done;

    /* Get files from source. */
    WT_ERR(__live_restore_fs_backing_filename(
      session, lr_fs, WTI_LIVE_RESTORE_FS_LAYER_SOURCE, directory, &path_src));

    WT_ERR(lr_fs->os_file_system->fs_exist(
      lr_fs->os_file_system, wt_session, path_src, &source_folder_exists));

    if (source_folder_exists) {
        WT_ERR(__wt_scr_alloc(session, 0, &filename));
        WT_ERR(lr_fs->os_file_system->fs_directory_list(
          lr_fs->os_file_system, wt_session, path_src, prefix, &dirlist_src, &num_src_files));

        for (uint32_t i = 0; i < num_src_files; ++i) {
            /*
             * If a file in source hasn't been background migrated yet we need to add it to the
             * list.
             */
            bool add_source_file = false;
            /*
             * Stop files should never exist in the source directory. We check this on startup but
             * add a sanity check here.
             */
            WT_ASSERT_ALWAYS(session,
              !WT_SUFFIX_MATCH(dirlist_src[i], WTI_LIVE_RESTORE_STOP_FILE_SUFFIX),
              "'%s' found in the source directory! Stop files should only exist in the destination",
              dirlist_src[i]);
            if (!dest_folder_exists)
                add_source_file = true;
            else {
                /*
                 * We're iterating files in the source, but we want to check if they exist in the
                 * destination, so create the file path to the backing destination file.
                 */
                WT_ERR(__wt_filename_construct(
                  session, path_dest, dirlist_src[i], UINTMAX_MAX, UINT32_MAX, filename));
                WT_ERR_NOTFOUND_OK(__live_restore_fs_has_file(lr_fs, &lr_fs->destination, session,
                                     (char *)filename->data, &dest_exist),
                  false);
                WT_ERR(__dest_has_stop_file(lr_fs, (char *)filename->data, session, &have_stop));

                add_source_file = !dest_exist && !have_stop;
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
    __wt_scr_free(session, &filename);
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
    WT_DECL_RET;
    WTI_LIVE_RESTORE_FS *lr_fs = (WTI_LIVE_RESTORE_FS *)fs;
    /*
     * We could fail to list a file if the live restore state changes between us walking the
     * destination and walking the source. Lock around the entire function to keep things simple and
     * avoid this scenario.
     */
    WTI_WITH_LIVE_RESTORE_STATE_LOCK((WT_SESSION_IMPL *)wt_session, lr_fs,
      ret = __live_restore_fs_directory_list_worker(
        fs, wt_session, directory, prefix, dirlistp, countp, false));
    return (ret);
}

/*
 * __live_restore_fs_directory_list_single --
 *     Get one file from a directory.
 */
static int
__live_restore_fs_directory_list_single(WT_FILE_SYSTEM *fs, WT_SESSION *wt_session,
  const char *directory, const char *prefix, char ***dirlistp, uint32_t *countp)
{
    WT_DECL_RET;
    WTI_LIVE_RESTORE_FS *lr_fs = (WTI_LIVE_RESTORE_FS *)fs;
    WTI_WITH_LIVE_RESTORE_STATE_LOCK((WT_SESSION_IMPL *)wt_session, lr_fs,
      ret = __live_restore_fs_directory_list_worker(
        fs, wt_session, directory, prefix, dirlistp, countp, true));
    return (ret);
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
 *     Return if the file exists. "Exists" means that the file is visible to the user of the live
 *     restore file system. If the file is present in the source directory but the user has deleted
 *     it in the destination then the file doesn't exist.
 */
static int
__live_restore_fs_exist(WT_FILE_SYSTEM *fs, WT_SESSION *wt_session, const char *name, bool *existp)
{
    WTI_LIVE_RESTORE_FS_LAYER_TYPE layer;
    WT_RET(__live_restore_fs_find_layer(fs, (WT_SESSION_IMPL *)wt_session, name, &layer));

    *existp = layer != WTI_LIVE_RESTORE_FS_LAYER_NONE;

    return (0);
}

/*
 * __live_restore_fh_free_bitmap --
 *     Free the bitmap associated with a live restore file handle. Callers must hold the file handle
 *     write lock.
 */
static void
__live_restore_fh_free_bitmap(WT_SESSION_IMPL *session, WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh)
{
    WT_ASSERT_ALWAYS(session, __wt_rwlock_islocked(session, &lr_fh->lock),
      "Live restore lock not taken when needed");
    __wt_free(session, lr_fh->bitmap);
    lr_fh->nbits = 0;
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
    WT_ASSERT((WT_SESSION_IMPL *)wt_session, lr_fh->destination != NULL);
    return (lr_fh->destination->fh_lock(lr_fh->destination, wt_session, lock));
}

/*
 * __live_restore_fh_fill_bit_range --
 *     Track that we wrote something by removing its hole from the file handle. Callers must hold
 *     the file handle write lock.
 */
static void
__live_restore_fh_fill_bit_range(
  WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh, WT_SESSION_IMPL *session, wt_off_t offset, size_t len)
{
    WT_ASSERT_ALWAYS(session, __wt_rwlock_islocked(session, &lr_fh->lock),
      "Live restore lock not taken when needed");

    /* If the file is complete or the write falls outside the bitmap, return. */
    if (WTI_DEST_COMPLETE(lr_fh))
        return;

    WT_ASSERT_ALWAYS(session, offset % lr_fh->allocsize == 0,
      "Fill offset must always be a multiple of alloc size");
    WT_ASSERT_ALWAYS(
      session, len % lr_fh->allocsize == 0, "Fill length must always be a multiple of alloc size");

    /*
     * Don't compute the offset before checking if the destination is complete, it depends on
     * allocsize which may not exist if the destination is complete.
     */
    uint64_t offset_bit = WTI_OFFSET_TO_BIT(offset);
    if (offset_bit >= lr_fh->nbits)
        return;

    WT_ASSERT_ALWAYS(session, (WTI_OFFSET_END(offset, len) % lr_fh->allocsize) == 0,
      "Offset end must always be a multiple of alloc size");
    uint64_t fill_end_bit = WTI_OFFSET_TO_BIT(WTI_OFFSET_END(offset, len)) - 1;
    bool partial_fill = false;
    if (fill_end_bit >= lr_fh->nbits) {
        partial_fill = true;
        fill_end_bit = lr_fh->nbits - 1;
    }
    __wt_verbose_debug3(session, WT_VERB_LIVE_RESTORE, "REMOVE%s HOLE %s: %" PRId64 "-%" PRId64,
      partial_fill ? " PARTIAL" : "", lr_fh->iface.name, offset, WTI_OFFSET_END(offset, len));
    __bit_nset(lr_fh->bitmap, offset_bit, fill_end_bit);
    return;
}

/*
 * __live_restore_encode_bitmap --
 *     Encode a live restore bitmap as a hexadecimal string. The caller must free the bitmap string.
 */
static int
__live_restore_encode_bitmap(
  WT_SESSION_IMPL *session, WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh, WT_ITEM *buf)
{
    WT_ASSERT_ALWAYS(session, __wt_rwlock_islocked(session, &lr_fh->lock),
      "Live restore lock not taken when needed");
    if (lr_fh->nbits == 0 || WTI_DEST_COMPLETE(lr_fh))
        return (0);
    size_t bitmap_byte_count = lr_fh->nbits / 8;
    if (lr_fh->nbits % 8 != 0)
        bitmap_byte_count++;
    return (__wt_raw_to_hex(session, lr_fh->bitmap, bitmap_byte_count, buf));
}

/*
 * __live_restore_dump_bitmap --
 *     Dump the live restore bitmap for a file handle. This function should only be called in the
 *     error path.
 */
static int
__live_restore_dump_bitmap(WT_SESSION_IMPL *session, WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh)
{
    WT_DECL_RET;
    WT_ITEM buf;

    WT_CLEAR(buf);

    __wt_verbose_debug1(session, WT_VERB_LIVE_RESTORE,
      "%s: Dumping bitmap, nbits (%" PRIu64 "), address (%p)", lr_fh->iface.name, lr_fh->nbits,
      (void *)lr_fh->bitmap);
    if (lr_fh->nbits > 0) {
        WT_ERR(__live_restore_encode_bitmap(session, lr_fh, &buf));
        __wt_verbose_debug1(
          session, WT_VERB_LIVE_RESTORE, "%s: %s", lr_fh->iface.name, (char *)buf.data);
    }
err:
    __wt_buf_free(session, &buf);
    return (ret);
}

/* !!!
 * __live_restore_can_service_read --
 *     Return if a read can be serviced by the destination file. Callers must hold the file handle
 *     read lock at a minimum.
 *     There are three possible scenarios:
 *     - Returns true if the read is entirely outside of all holes.
 *     - Returns false if:
 *         - The read is entirely within a hole.
 *         - The read begins outside a hole and then ends inside.
 *           This scenario will only happen if background data migration occurs concurrently and has
 *           partially migrated the content we're reading. The background threads always copy data
 *           in order, so the partially filled hole can only start outside a hole and then continue
 *           into a hole. However, since WiredTiger reads/writes are always whole blocks, a partial
 *           read implies that no writes have occurred on the page yet. Otherwise, the entire page
 *           in bitmap would have been set to -1, and we will find that the read is entirely outside
 *           all holes, which makes it safe to return false and read from the source.
 */
static bool
__live_restore_can_service_read(WT_SESSION_IMPL *session, WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh,
  wt_off_t offset, size_t len, wt_off_t *hole_begin_off)
{
    *hole_begin_off = -1;
    /*
     * The read will be serviced out of the destination if the read is beyond the length of the
     * source file.
     */
    if (WTI_DEST_COMPLETE(lr_fh) || offset >= WTI_BITMAP_END(lr_fh))
        return (true);
    /* Sanity check. */
    WT_ASSERT(session, lr_fh->allocsize != 0);
    WT_ASSERT_ALWAYS(session, __wt_rwlock_islocked(session, &lr_fh->lock),
      "Live restore lock not taken when needed");
    uint64_t read_end_bit =
      WTI_OFFSET_TO_BIT(WT_MIN(WTI_OFFSET_END(offset, len), WTI_BITMAP_END(lr_fh)));
    uint64_t read_start_bit = WTI_OFFSET_TO_BIT(offset);

    /*
     * The bits associated with a page must follow the pattern 1*0*, zero or more 1s followed by
     * zero or more 0s, as a page can never begin inside a hole and end outside it. See the function
     * comment for further details.
     */
    uint64_t current_bit = read_start_bit;
    /* Iterate through all set bits(1s) first. */
    while (current_bit < read_end_bit && __bit_test(lr_fh->bitmap, current_bit))
        current_bit++;

    if (current_bit == read_end_bit) {
        __wt_verbose_debug3(
          session, WT_VERB_LIVE_RESTORE, "CAN SERVICE %s: No hole found", lr_fh->iface.name);
        return (true);
    }

    /* We've found a hole. Return its offset to the caller. */
    *hole_begin_off = WTI_BIT_TO_OFFSET(current_bit);

    /* We need to iterate through those unset bits to verify this is a valid bit range. */
    while (current_bit < read_end_bit && !__bit_test(lr_fh->bitmap, current_bit))
        current_bit++;
    /*
     * If we still haven't reached the end of the read range then this is an invalid case where a
     * set bit appears after an unset bit in the range, e.g. 11000011, 00001111, or 00110011.
     */
    if (current_bit != read_end_bit) {
        WT_IGNORE_RET(__live_restore_dump_bitmap(session, lr_fh));
        WT_ASSERT_ALWAYS(session, false,
          "Read (offset: %" PRId64 ", len: %" WT_SIZET_FMT ") found a set bit (offset: %" PRId64
          ") after a hole.",
          offset, len, WTI_BIT_TO_OFFSET(current_bit));
    }
    return (false);
}

/*
 * __live_restore_fh_write_destination --
 *     Write to the destination file handle.
 */
static int
__live_restore_fh_write_destination(WT_SESSION_IMPL *session, WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh,
  wt_off_t offset, size_t len, const void *buf)
{
    __wt_verbose_debug3(session, WT_VERB_LIVE_RESTORE, "WRITE %s: %" PRId64 ", %" WT_SIZET_FMT,
      lr_fh->iface.name, offset, len);
    return (
      lr_fh->destination->fh_write(lr_fh->destination, (WT_SESSION *)session, offset, len, buf));
}

/*
 * __live_restore_fh_write_int --
 *     Write to a file. Callers of this function must hold the file handle lock.
 */
static int
__live_restore_fh_write_int(WT_FILE_HANDLE *fh, WT_SESSION *wt_session, wt_off_t offset, size_t len,
  const void *buf, bool background_thread)
{
    WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh;
    WT_SESSION_IMPL *session;

    lr_fh = (WTI_LIVE_RESTORE_FILE_HANDLE *)fh;
    session = (WT_SESSION_IMPL *)wt_session;

    WT_ASSERT_ALWAYS(session, __wt_rwlock_islocked(session, &lr_fh->lock),
      "Live restore lock not taken when needed");
    WT_RET(__live_restore_fh_write_destination(session, lr_fh, offset, len, buf));
    if (background_thread)
        WT_STAT_CONN_INCRV(session, live_restore_bytes_copied, len);
    __live_restore_fh_fill_bit_range(lr_fh, session, offset, len);
    return (0);
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

    /*
     * Fast path writes if the destination is complete. This pointer is only ever cleared in a
     * multithreaded context, if we read a valid pointer we will take the slow path with the same
     * result.
     */
    if (WTI_DEST_COMPLETE(lr_fh))
        return (__live_restore_fh_write_destination(session, lr_fh, offset, len, buf));

    WTI_WITH_LIVE_RESTORE_FH_WRITE_LOCK(
      session, lr_fh, ret = __live_restore_fh_write_int(fh, wt_session, offset, len, buf, false));
    return (ret);
}

/*
 * __live_restore_fh_read_destination --
 *     Read from the destination file handle.
 */
static int
__live_restore_fh_read_destination(
  WT_SESSION_IMPL *session, WT_FILE_HANDLE *destination, wt_off_t offset, size_t len, void *buf)
{
    __wt_verbose_debug3(session, WT_VERB_LIVE_RESTORE, "%s", "    READ FROM DEST");
    return (destination->fh_read(destination, (WT_SESSION *)session, offset, len, buf));
}

/*
 * __live_restore_fh_read_source --
 *     Read data from the source directory and update appropriate statistics.
 */
static int
__live_restore_fh_read_source(
  WT_SESSION_IMPL *session, WT_FILE_HANDLE *source, wt_off_t off, size_t len, void *buf)
{
    uint64_t time_start, time_stop;

    __wt_verbose_debug3(session, WT_VERB_LIVE_RESTORE, "%s", "    READ FROM SOURCE");

    time_start = __wt_clock(session);
    WT_RET(source->fh_read(source, (WT_SESSION *)session, off, len, buf));
    time_stop = __wt_clock(session);
    __wt_stat_msecs_hist_incr_live_restore(session, WT_CLOCKDIFF_MS(time_stop, time_start));
    WT_STAT_CONN_INCR(session, live_restore_source_read_count);
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
    WT_DECL_RET;
    WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh;
    WT_SESSION_IMPL *session;
    char *read_data;

    lr_fh = (WTI_LIVE_RESTORE_FILE_HANDLE *)fh;
    session = (WT_SESSION_IMPL *)wt_session;

    __wt_verbose_debug3(session, WT_VERB_LIVE_RESTORE, "READ %s : %" PRId64 ", %" WT_SIZET_FMT,
      fh->name, offset, len);

    read_data = (char *)buf;

    /*
     * Fast path reads if the destination is complete. This pointer is only ever cleared in a
     * multithreaded context, if we read a valid pointer we will take the slow path with the same
     * result.
     */
    if (WTI_DEST_COMPLETE(lr_fh))
        WT_RET(__live_restore_fh_read_destination(session, lr_fh->destination, offset, len, buf));

    __wt_readlock(session, &lr_fh->lock);
    wt_off_t hole_begin_off;
    if (__live_restore_can_service_read(session, lr_fh, offset, len, &hole_begin_off)) {
        /* Perform a full read from the destination. */
        WT_ERR(
          __live_restore_fh_read_destination(session, lr_fh->destination, offset, len, read_data));
    } else {
        /* Otherwise read from the source. */
        WT_ERR(__live_restore_fh_read_source(session, lr_fh->source, offset, len, read_data));

#ifdef HAVE_DIAGNOSTIC
        /*
         * If we found a read can be partially serviced it's because the page has been partially
         * migrated by a background thread. This means the data in the source and destination should
         * be identical, and we verify this as a safety check.
         */
        /*
         *!!!
         *              <--read len--->
         * read:        |-------------|
         *      bitmap: |####|----hole----|
         *              ^    ^        |
         *              |    |        |
         *           read off|        |
         *                hole off    |
         * read dest:   |----|
         * read source:      |--------|
         *
         *
         */
        char *tmp_buf = NULL;
        if (hole_begin_off > offset) {
            size_t dest_partial_read_len = (size_t)(hole_begin_off - offset);
            WT_ERR(__wt_malloc(session, dest_partial_read_len, &tmp_buf));

            /* Read the serviceable portion from the destination. */
            ret = __live_restore_fh_read_destination(
              session, lr_fh->destination, offset, dest_partial_read_len, tmp_buf);

            if (ret == 0)
                /*
                 * We only need to check the destination portion matches the portion in the source.
                 */
                WT_ASSERT(session, strncmp(read_data, tmp_buf, dest_partial_read_len) == 0);

            __wt_free(session, tmp_buf);

            if (ret != 0)
                goto err;
        }
#endif
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
    __wt_readunlock(session, &lr_fh->lock);
    return (ret);
}

/*
 * __live_restore_fh_close_source --
 *     Close and free the source file handle.
 */
static int
__live_restore_fh_close_source(
  WT_SESSION_IMPL *session, WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh, bool lock)
{
    WT_DECL_RET;
    if (lock)
        __wt_writelock(session, &lr_fh->lock);

    if (lr_fh->source != NULL) {
        __wt_verbose_debug1(
          session, WT_VERB_LIVE_RESTORE, "Closing source fh %s", lr_fh->iface.name);
        WT_ERR(lr_fh->source->close(lr_fh->source, (WT_SESSION *)session));
        lr_fh->source = NULL;
    }

    /* We can also free the bitmap here as it is no longer relevant.*/
    __live_restore_fh_free_bitmap(session, lr_fh);
err:
    if (lock)
        __wt_writeunlock(session, &lr_fh->lock);
    return (ret);
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

    WT_RET(lr_fh->destination->fh_size(lr_fh->destination, wt_session, &destination_size));
    *sizep = destination_size;
    return (0);
}

/*
 * __live_restore_compute_read_end_bit --
 *     Compute the last possible bit for hole filling.
 */
static int
__live_restore_compute_read_end_bit(WT_SESSION_IMPL *session, WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh,
  wt_off_t buf_size, uint64_t first_clear_bit, uint64_t *end_bitp)
{
    wt_off_t read_start = WTI_BIT_TO_OFFSET(first_clear_bit);
    wt_off_t file_size;
    /*
     * In theory we have truncated the destination file to be smaller than the source file. This
     * would be better tracked with a variable on the lr_fh itself but for now we can work around it
     * by reading the size of the destination file.
     */
    WT_RET(__live_restore_fh_size((WT_FILE_HANDLE *)lr_fh, (WT_SESSION *)session, &file_size));
    file_size = WT_MIN(file_size, WTI_BITMAP_END(lr_fh));
    wt_off_t largest_possible_read = WT_MIN(file_size, read_start + buf_size);
    /* Subtract 1 as the read end is served from the nbits - 1th bit.*/
    uint64_t max_read_bit = WTI_OFFSET_TO_BIT(largest_possible_read) - 1;
    uint64_t current_bit;
    for (current_bit = first_clear_bit;
         current_bit < max_read_bit && !__bit_test(lr_fh->bitmap, current_bit + 1); current_bit++)
        ;
    *end_bitp = current_bit;
    return (0);
}

/*
 * __live_restore_fill_hole --
 *     Fill a single hole in the destination file. If the hole list is empty indicate using the
 *     finished parameter. Must be called while holding the file handle write lock.
 */
static int
__live_restore_fill_hole(WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh, WT_SESSION *wt_session, char *buf,
  wt_off_t buf_size, wt_off_t *read_offsetp, bool *finishedp)
{
    WT_SESSION_IMPL *session = (WT_SESSION_IMPL *)wt_session;

    WT_ASSERT_ALWAYS(session, __wt_rwlock_islocked(session, &lr_fh->lock),
      "Live restore lock not taken when needed");
    uint64_t first_clear_bit;
    /*
     * If there are no clear bits then every hole in the file has been filled. Indicate that the
     * file has finished restoring.
     */
    if (__bit_ffc(lr_fh->bitmap, lr_fh->nbits, &first_clear_bit) == -1) {
        *finishedp = true;
        return (0);
    }

    /* Walk the unset bit list until the read_size is reached. */
    wt_off_t read_start = WTI_BIT_TO_OFFSET(first_clear_bit);
    uint64_t read_end_bit;
    WT_RET(__live_restore_compute_read_end_bit(
      session, lr_fh, buf_size, first_clear_bit, &read_end_bit));
    wt_off_t read_end = WTI_BIT_TO_OFFSET(read_end_bit + 1);
    size_t read_size = (size_t)(read_end - read_start);

    __wt_verbose_debug3(session, WT_VERB_LIVE_RESTORE,
      "Found hole in %s at %" PRId64 "-%" PRId64 " during background migration. ",
      lr_fh->iface.name, read_start, read_end);

    __wt_verbose_debug3(session, WT_VERB_LIVE_RESTORE,
      "    BACKGROUND READ %s : %" PRId64 ", %" WT_SIZET_FMT, lr_fh->iface.name, read_start,
      read_size);

    *read_offsetp = read_start;
    WT_RET(__live_restore_fh_read_source(session, lr_fh->source, read_start, read_size, buf));
    return (__live_restore_fh_write_int(
      (WT_FILE_HANDLE *)lr_fh, wt_session, read_start, read_size, buf, true));
}

/*
 * __wti_live_restore_fs_restore_file --
 *     Restore a file in the destination by filling any holes with data from the source. Mark the
 *     file handle complete if the full restore is able to take place.
 */
int
__wti_live_restore_fs_restore_file(WT_FILE_HANDLE *fh, WT_SESSION *wt_session)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session = (WT_SESSION_IMPL *)wt_session;

    __wt_verbose_debug2(session, WT_VERB_LIVE_RESTORE, "%s: Restoring in the background", fh->name);

    /*
     * Live restore dirties btrees to ensure its bitmap updates are persisted, through the
     * application write path this is a non issue as the application writes would also dirty the
     * btree but for the background thread which does not perform writes in the traditional sense it
     * is a requirement.
     *
     * However, there are a number of edge cases in dirtying the btree, particularly with regards to
     * the btree->original flag. It is possible to backup a database with trees still in the
     * "original" state, on restore the background thread would visit the file, mark the btree as
     * dirty but also original. This is an invalid state. To workaround this we also disable bulk
     * loading into the btree which has the side effect of making it no longer original.
     *
     * Now consider what happens if an application opens a bulk cursor concurrently with live
     * restore which can disable bulk cursors. WiredTiger does not guarantee that opening a bulk
     * cursor must succeed so the application be prepared to handle EBUSY. Additionally, the
     * background worker threads in live restore open cursor on any file they intend to restore.
     * This cursor prevents the application bulk cursor from opening which would then generate an
     * EBUSY for the application. If the application succeeded in opening the bulk cursor prior to
     * the live restore worker opening its cursor, then the live restore worker will get EBUSY and
     * return the item to the queue.
     *
     * Historically, while fixing this issue, we ran into an edge case where the application would
     * call schema->alter on a btree which was original. Live restore had concurrently dirtied the
     * btree but not modified the original state, schema alter expected that after a system wide
     * checkpoint the tree being altered would be in a clean state. The original flag being true
     * prevents checkpoints from being taken which means that the tree never left the dirty state
     * and schema alter would fail crashing the application. This is the second reason we disable
     * bulk loading on the btree and clear the original flag.
     */
    __wt_btree_disable_bulk(session);

    char *buf = NULL;
    WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh = (WTI_LIVE_RESTORE_FILE_HANDLE *)fh;
    /*
     * It is possible for the user to specify a read size that is not aligned to our bitmap. In
     * which case we change it to be the file allocation size.
     */
    size_t buf_size = WT_MAX(lr_fh->back_pointer->read_size, lr_fh->allocsize);
    WT_RET(__wt_calloc(session, 1, buf_size, &buf));

    WT_TIMER timer;
    uint64_t msg_count = 0;
    bool finished = false;
    __wt_timer_start(session, &timer);
    for (;;) {
        wt_off_t read_offset = 0;
        uint64_t time_diff_ms;
        WTI_WITH_LIVE_RESTORE_FH_WRITE_LOCK(session, lr_fh,
          ret = __live_restore_fill_hole(
            lr_fh, wt_session, buf, (wt_off_t)buf_size, &read_offset, &finished));
        WT_ERR(ret);

        if (finished) {
            __wt_verbose_debug1(session, WT_VERB_LIVE_RESTORE,
              "%s: Finished background restoration, closing source file", fh->name);
            WT_ERR(__live_restore_fh_close_source(session, lr_fh, true));

            /*
             * Dirty the tree again to ensure the live restore metadata is written out by the next
             * checkpoint.
             */
            __wt_tree_modify_set(session);
            break;
        }
        __wt_timer_evaluate_ms(session, &timer, &time_diff_ms);
        if ((time_diff_ms / (WT_THOUSAND * WT_PROGRESS_MSG_PERIOD)) > msg_count) {
            __wt_verbose(session, WT_VERB_LIVE_RESTORE_PROGRESS,
              "Live restore running on %s for %" PRIu64
              " seconds. Currently copying offset %" PRId64 " of file size %" PRId64,
              lr_fh->iface.name, time_diff_ms / WT_THOUSAND, read_offset, WTI_BITMAP_END(lr_fh));
            msg_count = time_diff_ms / (WT_THOUSAND * WT_PROGRESS_MSG_PERIOD);

            /*
             * Dirty the tree periodically to ensure the live restore metadata is written out by the
             * next checkpoint.
             */
            __wt_tree_modify_set(session);
        }

        /*
         * Because this loop can run for a while, ensure the system has not entered a panic state or
         * closing state in the meantime.
         */
        WT_ERR(WT_SESSION_CHECK_PANIC(wt_session));
        if (F_ISSET_ATOMIC_32(S2C(session), WT_CONN_CLOSING))
            break;
    }
err:
    __wt_free(session, buf);
    return (ret);
}

/*
 * __wti_live_restore_cleanup_stop_files --
 *     Remove all stop files from the database.
 */
int
__wti_live_restore_cleanup_stop_files(WT_SESSION_IMPL *session)
{
    WT_DECL_RET;
    WT_CONNECTION_IMPL *conn = S2C(session);
    WTI_LIVE_RESTORE_FS *fs = (WTI_LIVE_RESTORE_FS *)conn->file_system;
    WT_FILE_SYSTEM *os_fs = fs->os_file_system;
    WT_SESSION *wt_session = (WT_SESSION *)session;

    char **files;
    uint32_t count = 0;
    WT_DECL_ITEM(buf);
    WT_DECL_ITEM(filepath);

    WT_RET(__wt_scr_alloc(session, 0, &filepath));

    /* Remove stop files in the destination directory. */
    WT_RET(os_fs->fs_directory_list(os_fs, wt_session, fs->destination.home, NULL, &files, &count));
    for (uint32_t i = 0; i < count; i++) {
        if (WT_SUFFIX_MATCH(files[i], WTI_LIVE_RESTORE_STOP_FILE_SUFFIX)) {
            WT_ERR(__wt_filename_construct(
              session, fs->destination.home, files[i], UINTMAX_MAX, UINT32_MAX, filepath));
            __wt_verbose_info(
              session, WT_VERB_LIVE_RESTORE, "Removing stop file %s", (char *)filepath->data);
            WT_ERR(os_fs->fs_remove(os_fs, wt_session, (char *)filepath->data, 0));
        }
    }
    if (F_ISSET(&conn->log_mgr, WT_LOG_CONFIG_ENABLED)) {
        WT_ERR(__wt_scr_alloc(session, 1024, &buf));

        /*
         * The log path is the only WiredTiger-owned subdirectory that can exist. Check its contents
         * explicitly.
         */
        WT_ERR(__wt_filename_construct(session, fs->destination.home,
          (char *)conn->log_mgr.log_path, UINTMAX_MAX, UINT32_MAX, filepath));
        /* FIXME-WT-14047: Currently we do not support absolute log paths. */
        WT_ASSERT(session, !__wt_absolute_path((char *)conn->log_mgr.log_path));

        WT_ERR(os_fs->fs_directory_list_free(os_fs, wt_session, files, count));
        WT_ERR(os_fs->fs_directory_list(
          os_fs, wt_session, (char *)filepath->data, NULL, &files, &count));
        for (uint32_t i = 0; i < count; i++) {
            if (WT_SUFFIX_MATCH(files[i], WTI_LIVE_RESTORE_STOP_FILE_SUFFIX)) {
                WT_ERR(__wt_buf_fmt(session, buf, "%s/%s", (char *)filepath->data, files[i]));
                __wt_verbose_info(session, WT_VERB_LIVE_RESTORE,
                  "Removing log directory stop file %s", (char *)buf->data);
                WT_ERR(os_fs->fs_remove(os_fs, wt_session, buf->data, 0));
            }
        }
    }
err:
    WT_TRET(os_fs->fs_directory_list_free(os_fs, wt_session, files, count));
    __wt_scr_free(session, &filepath);
    __wt_scr_free(session, &buf);
    return (ret);
}

/*
 * __live_restore_fh_close --
 *     Close the file.
 */
static int
__live_restore_fh_close(WT_FILE_HANDLE *fh, WT_SESSION *wt_session)
{
    WT_DECL_RET;
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
    if (lr_fh->destination != NULL)
        WT_RET(lr_fh->destination->close(lr_fh->destination, wt_session));

    ret = __live_restore_fh_close_source(session, lr_fh, true);
    __wt_rwlock_destroy(session, &lr_fh->lock);
    __wt_free(session, lr_fh->iface.name);
    __wt_free(session, lr_fh);

    return (ret);
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
    return (lr_fh->destination->fh_sync(lr_fh->destination, wt_session));
}

/*
 * __live_restore_fh_truncate --
 *     Truncate a file. This operation is only applied to the destination file.
 */
static int
__live_restore_fh_truncate(WT_FILE_HANDLE *fh, WT_SESSION *wt_session, wt_off_t len)
{
    WT_DECL_RET;
    wt_off_t old_len = 0;

    WT_RET(__live_restore_fh_size(fh, wt_session, &old_len));
    /* Sometimes we call truncate but don't change the length. Ignore */
    if (old_len == len)
        return (0);

    __wt_verbose_debug2((WT_SESSION_IMPL *)wt_session, WT_VERB_LIVE_RESTORE,
      "truncating file %s from %" PRId64 " to %" PRId64, fh->name, old_len, len);

    WT_SESSION_IMPL *session = (WT_SESSION_IMPL *)wt_session;
    WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh = (WTI_LIVE_RESTORE_FILE_HANDLE *)fh;

    bool locked = false;
    if (!WTI_DEST_COMPLETE(lr_fh)) {
        /* Lock so we can't race with background threads moving chunks. */
        __wt_writelock(session, &lr_fh->lock);
        locked = true;
    }

    WT_ERR(lr_fh->destination->fh_truncate(lr_fh->destination, wt_session, len));
    /* Only modify the bitmap if we are shortening the file and we have taken the lock. */
    if (old_len > len && locked) {
        /*
         * Set the relevant bits in the bitmap. This won't be persisted across a crash without a
         * metadata write. We catch this rare scenario with an assertion on file reopen.
         */
        __live_restore_fh_fill_bit_range(lr_fh, session, len, (size_t)(old_len - len));
    }
err:
    if (locked)
        __wt_writeunlock(session, &lr_fh->lock);
    return (ret);
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
    __wt_verbose_debug2(
      session, WT_VERB_LIVE_RESTORE, "%s: Opening source file", lr_fh->iface.name);
    /* Open the file in the layer. */
    WT_ERR(__live_restore_fs_backing_filename(
      session, lr_fs, WTI_LIVE_RESTORE_FS_LAYER_SOURCE, lr_fh->iface.name, &path));
    WT_ERR(lr_fs->os_file_system->fs_open_file(lr_fs->os_file_system, (WT_SESSION *)session, path,
      lr_fh->file_type, flags | WT_FS_OPEN_READONLY, &fh));

    lr_fh->source = fh;

err:
    __wt_free(session, path);
    return (ret);
}

/*
 * __live_restore_compute_nbits --
 *     Compute the number of bits needed for the bitmap, based off the destination file size.
 */
static int
__live_restore_compute_nbits(
  WT_SESSION_IMPL *session, WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh, uint64_t *nbitsp)
{
    wt_off_t size;
    WT_RET(lr_fh->destination->fh_size(lr_fh->destination, (WT_SESSION *)session, &size));
    WT_ASSERT_ALWAYS(session, size % lr_fh->allocsize == 0,
      "The file size isn't a multiple of the file allocation size!");
    *nbitsp = (uint64_t)size / (uint64_t)lr_fh->allocsize;
    return (0);
}

/*
 * __live_restore_decode_bitmap --
 *     Decode a bitmap from a hex string.
 */
static int
__live_restore_decode_bitmap(WT_SESSION_IMPL *session, const char *bitmap_str,
  uint64_t metadata_nbits, WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh)
{
    WT_DECL_RET;
    WT_ASSERT_ALWAYS(session, bitmap_str != NULL, "Live restore bitmap string is NULL");

    WT_RET(__bit_alloc(session, metadata_nbits, &lr_fh->bitmap));
    lr_fh->nbits = metadata_nbits;

    WT_ITEM buf;
    WT_CLEAR(buf);
    WT_ERR(__wt_hex_to_raw(session, bitmap_str, &buf));
    memcpy(lr_fh->bitmap, buf.mem, buf.size);

    uint64_t file_size_nbits;
    WT_ERR(__live_restore_compute_nbits(session, lr_fh, &file_size_nbits));
    /*
     * We may have truncated a file and the filesize on disk is shorter than that in the bitmap
     * itself. This would happen even without a crash, we can assert that all the bits in the file
     * past the file size nbits are set.
     */
    if (file_size_nbits < metadata_nbits)
        for (uint64_t i = file_size_nbits; i < metadata_nbits; i++)
            WT_ASSERT_ALWAYS(session, __bit_test(lr_fh->bitmap, i),
              "Live restore bitmap corruption detected in %s, bit %" PRIu64 " of %" PRIu64
              " is unset!",
              lr_fh->iface.name, i, metadata_nbits);
err:
    __wt_buf_free(session, &buf);
    return (ret);
}

/*
 * __wt_live_restore_metadata_to_fh --
 *     Reconstruct the bitmap in memory from the relevant metadata entries for the given file.
 */
int
__wt_live_restore_metadata_to_fh(
  WT_SESSION_IMPL *session, WT_FILE_HANDLE *fh, WT_LIVE_RESTORE_FH_META *lr_fh_meta)
{
    WT_DECL_RET;
    WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh = (WTI_LIVE_RESTORE_FILE_HANDLE *)fh;
    if (!F_ISSET(S2C(session), WT_CONN_LIVE_RESTORE_FS))
        return (0);

    WT_ASSERT_ALWAYS(session, lr_fh->bitmap == NULL,
      "Reading in bitmap information from metadata but bitmap information already exists in "
      "memory");

    __wt_writelock(session, &lr_fh->lock);
    lr_fh->allocsize = lr_fh_meta->allocsize;

    /* If there is no source file there is nothing to migrate and therefore no metadata to parse. */
    if (WTI_DEST_COMPLETE(lr_fh)) {
        __wt_writeunlock(session, &lr_fh->lock);
        return (0);
    }

    /*
     * !!!
     * While the live restore is in progress, the bit count reported by in the live restore metadata
     * can hold three states:
     *  (0)         : This means the file has not started migration.
     *  (-1)        : This indicates the file has finished migration and the bitmap is empty.
     *  (nbits > 0) : The number of bits in the bitmap. This is set when the file is in the
     *                process of being migrated.
     */
    if (lr_fh_meta->nbits == 0) {
        WT_ASSERT(session, !WTI_DEST_COMPLETE(lr_fh));
        uint64_t nbits;
        WT_ERR(__live_restore_compute_nbits(session, lr_fh, &nbits));
        WT_ERR(__bit_alloc(session, nbits, &lr_fh->bitmap));
        lr_fh->nbits = nbits;
        __wt_writeunlock(session, &lr_fh->lock);
        return (0);
    } else if (lr_fh_meta->nbits > 0) {
        /* We shouldn't be reconstructing a bitmap if the live restore has finished. */
        WT_ASSERT(session, !__wti_live_restore_migration_complete(session));
        __wt_verbose_debug3(session, WT_VERB_LIVE_RESTORE,
          "Reconstructing bitmap for %s, bitmap_sz %" PRId64 ", bitmap_str %s", fh->name,
          lr_fh_meta->nbits, lr_fh_meta->bitmap_str);
        /* Reconstruct a pre-existing bitmap. */
        WT_ERR(__live_restore_decode_bitmap(
          session, lr_fh_meta->bitmap_str, (uint64_t)lr_fh_meta->nbits, lr_fh));
    } else {
        WT_ASSERT(session, lr_fh_meta->nbits == -1);
        /*
         * Our file open logic always opens the backing source file when it exists, but since we've
         * completed migration we don't need it.
         */
        WT_ERR(__live_restore_fh_close_source(session, lr_fh, false));
    }

    if (0) {
err:
        __live_restore_fh_free_bitmap(session, lr_fh);
    }
    __wt_writeunlock(session, &lr_fh->lock);
    return (ret);
}

/*
 * __wt_live_restore_fh_to_metadata --
 *     Given a WiredTiger file handle generate a metadata string. If live restore is not running
 *     return a WT_NOTFOUND error.
 */
int
__wt_live_restore_fh_to_metadata(WT_SESSION_IMPL *session, WT_FILE_HANDLE *fh, WT_ITEM *meta_string)
{
    WT_DECL_RET;
    if (!F_ISSET(S2C(session), WT_CONN_LIVE_RESTORE_FS))
        return (WT_NOTFOUND);

    WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh = (WTI_LIVE_RESTORE_FILE_HANDLE *)fh;

    WT_ITEM buf;
    WT_CLEAR(buf);

    __wt_readlock(session, &lr_fh->lock);
    if (lr_fh->nbits > 0) {
        WT_ERR(__live_restore_encode_bitmap(session, lr_fh, &buf));
        WT_ERR(__wt_buf_catfmt(session, meta_string, ",live_restore=(bitmap=%s,nbits=%" PRIu64 ")",
          (char *)buf.data, lr_fh->nbits));
        __wt_verbose_debug3(session, WT_VERB_LIVE_RESTORE,
          "%s: Appending live restore bitmap (%s, %" PRIu64 ") to metadata", fh->name,
          (char *)buf.data, lr_fh->nbits);
    } else {
        /* -1 indicates the file has completed migration. */
        WT_ERR(__wt_buf_catfmt(session, meta_string, ",live_restore=(bitmap=,nbits=-1)"));
        __wt_verbose_debug3(
          session, WT_VERB_LIVE_RESTORE, "%s: Appending empty live restore metadata", fh->name);
    }
err:
    __wt_readunlock(session, &lr_fh->lock);
    __wt_buf_free(session, &buf);

    return (ret);
}

/*
 * __wt_live_restore_clean_metadata_string --
 *     This function is only called when taking a backup. It validates the live restore metadata
 *     string, either aborting on an unrecoverable error or cleaning up an outdated config value.
 */
int
__wt_live_restore_clean_metadata_string(WT_SESSION_IMPL *session, char *value)
{
    WT_CONFIG_ITEM v;
    WT_DECL_RET;

    WT_ASSERT_ALWAYS(session, !__wt_live_restore_migration_in_progress(session),
      "Cleaning the metadata string should only be called for non-live restore file systems");

    ret = __wt_config_getones(session, value, "live_restore", &v);
    WT_RET_NOTFOUND_OK(ret);
    if (ret != WT_NOTFOUND && v.len != 0) {

        WT_CONFIG_ITEM cval;
        WT_RET(__wt_config_subgets(session, &v, "bitmap", &cval));
        /*
         * This function is only called when taking a backup. If we find unset bits in the bitmap
         * that means the file is only partially migrated from source and we're about to take a
         * backup of a partially populated file.
         */
        WT_ASSERT_ALWAYS(
          session, cval.len == 0, "Found non-empty bitmap when cleaning config string");

        WT_RET(__wt_config_subgets(session, &v, "nbits", &cval));

        /*
         * Live restore uses -1 in the nbits field to indicate the file has been fully migrated.
         * However, if this value is copied into a backup, future live restores using this backup as
         * a source will see the nbits=-1 value and assume a file that still needs migrating has
         * already been migrated. Set it to 0 now to indicate it is yet to be migrated.
         */
        if (WT_STRING_LIT_MATCH("-1", cval.str, 2)) {
            wt_off_t nbits_val_str_offset = cval.str - value;
            /*
             * We need to overwrite two characters, but only need to write one. Add a redundant
             * comma so we don't need to resize the string. The config parser will ignore it.
             */
            value[nbits_val_str_offset] = '0';
            value[nbits_val_str_offset + 1] = ',';
        } else
            /*
             * There are only two possible values for nbits here. Either nbits=-1 because the file
             * underwent a complete live restore, or nbits=0 because we're backing up a database
             * that didn't undergo live restore. Any other value indicates a file has been partially
             * live restored and is missing data.
             */
            WT_ASSERT_ALWAYS(session, WT_STRING_LIT_MATCH("0", cval.str, 1),
              "Invalid live restore metadata detected while cleaning string");
    }

    return (0);
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
      session, lr_fs, WTI_LIVE_RESTORE_FS_LAYER_DESTINATION, name, &path));
    WT_ERR(lr_fs->os_file_system->fs_open_file(
      lr_fs->os_file_system, (WT_SESSION *)session, path, lr_fh->file_type, flags, &fh));
    lr_fh->destination = fh;
    lr_fh->back_pointer = lr_fs;
err:
    __wt_free(session, path);
    return (ret);
}

/*
 * __live_restore_setup_lr_fh_directory --
 *     Populate a live restore file handle for a directory. Directories are created by the user and
 *     therefore must always exist in the destination.
 */
static int
__live_restore_setup_lr_fh_directory(WT_SESSION_IMPL *session, WTI_LIVE_RESTORE_FS *lr_fs,
  const char *name, uint32_t flags, WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh)
{
    bool dest_exist = false;

    WT_RET_NOTFOUND_OK(
      __live_restore_fs_has_file(lr_fs, &lr_fs->destination, session, name, &dest_exist));

    /*
     * WiredTiger never creates directories(except when MongoDB enables directory per db usage). The
     * user must do this themselves.
     */
    if (!dest_exist)
        WT_RET_MSG(session, ENOENT, "Directory %s does not exist in the destination", name);

    WT_FILE_HANDLE *fh;
    WT_RET(lr_fs->os_file_system->fs_open_file(
      lr_fs->os_file_system, (WT_SESSION *)session, name, lr_fh->file_type, flags, &fh));

    lr_fh->destination = fh;

    /* There's no need for a hole list. The directory has already been fully copied */
    lr_fh->bitmap = NULL;
    lr_fh->back_pointer = lr_fs;

    return (0);
}

/*
 * __live_restore_remove_temporary_file --
 *     Remove a temporary file and log a message if it exists.
 */
static int
__live_restore_remove_temporary_file(
  WT_SESSION_IMPL *session, WT_FILE_SYSTEM *os_fs, char *dest_path, char **tmp_file_path)
{
    size_t tmp_file_path_len = strlen(dest_path) + strlen(WTI_LIVE_RESTORE_TEMP_FILE_SUFFIX) + 1;
    WT_RET(__wt_calloc(session, 1, tmp_file_path_len, tmp_file_path));
    WT_RET(__wt_snprintf(
      *tmp_file_path, tmp_file_path_len, "%s" WTI_LIVE_RESTORE_TEMP_FILE_SUFFIX, dest_path));
    /* Delete any existing temporary file. Also report a warning if it existed already. */
    bool exists = false;
    WT_RET(os_fs->fs_exist(os_fs, (WT_SESSION *)session, *tmp_file_path, &exists));
    if (!exists)
        return (0);
    __wt_verbose_info(session, WT_VERB_LIVE_RESTORE,
      "Found existing temporary file: %s deleting it!", *tmp_file_path);
    return (os_fs->fs_remove(os_fs, (WT_SESSION *)session, *tmp_file_path, 0));
}

/*
 * __live_restore_fs_atomic_copy_file --
 *     Atomically copy an entire file from the source to the destination. This replaces the normal
 *     background migration logic. We intentionally do not call the WiredTiger copy and sync
 *     function as we are copying between layers and that function copies between two paths. This is
 *     the same "path" from the perspective of a function higher in the stack.
 */
static int
__live_restore_fs_atomic_copy_file(WT_SESSION_IMPL *session, WTI_LIVE_RESTORE_FS *lr_fs,
  WT_FS_OPEN_FILE_TYPE type, const char *filename)
{
    WT_DECL_RET;
    WT_FILE_HANDLE *source_fh = NULL, *dest_fh = NULL;
    WT_SESSION *wt_session = (WT_SESSION *)session;
    size_t read_size = lr_fs->read_size, len;
    wt_off_t source_size;
    char *buf = NULL, *source_path = NULL, *dest_path = NULL, *tmp_dest_path = NULL;
    bool dest_closed = false;

    WT_ASSERT_ALWAYS(session, !__wti_live_restore_migration_complete(session),
      "Attempting to atomically copy a file outside of the migration phase!");

    WT_ASSERT(session, type == WT_FS_OPEN_FILE_TYPE_LOG || type == WT_FS_OPEN_FILE_TYPE_REGULAR);
    __wt_verbose_debug2(session, WT_VERB_LIVE_RESTORE,
      "Atomically copying %s file (%s) from source to dest.\n",
      type == WT_FS_OPEN_FILE_TYPE_LOG ? "log" : "regular", filename);

    /* Get the full source and destination file names. */
    WT_ERR(__live_restore_fs_backing_filename(
      session, lr_fs, WTI_LIVE_RESTORE_FS_LAYER_SOURCE, filename, &source_path));
    WT_ERR(__live_restore_fs_backing_filename(
      session, lr_fs, WTI_LIVE_RESTORE_FS_LAYER_DESTINATION, filename, &dest_path));

    /* In theory we may have crashed during a temporary file copy, remove that file now. */
    WT_ERR(__live_restore_remove_temporary_file(
      session, lr_fs->os_file_system, dest_path, &tmp_dest_path));

    /* Open both files and create the temporary destination file. */
    WT_ERR(lr_fs->os_file_system->fs_open_file(
      lr_fs->os_file_system, wt_session, source_path, type, WT_FS_OPEN_READONLY, &source_fh));
    WT_ERR(lr_fs->os_file_system->fs_open_file(lr_fs->os_file_system, wt_session, tmp_dest_path,
      type, WT_FS_OPEN_CREATE | WT_FS_OPEN_EXCLUSIVE, &dest_fh));

    WT_ERR(
      lr_fs->os_file_system->fs_size(lr_fs->os_file_system, wt_session, source_path, &source_size));

    /*
     * Break the copy into small chunks. Split the file into n chunks: the first n - 1 chunks will
     * read a full read_size buffer, and the last chunk reads the remaining data.
     */
    WT_ERR(__wt_calloc(session, 1, read_size, &buf));
    for (wt_off_t off = 0; off < source_size; off += (wt_off_t)len) {
        len = WT_MIN((size_t)(source_size - off), read_size);
        WT_ERR(__live_restore_fh_read_source(session, source_fh, off, len, buf));
        WT_ERR(dest_fh->fh_write(dest_fh, wt_session, off, len, buf));

        /* Check the system has not entered a panic state since the copy can take a long time. */
        WT_ERR(WT_SESSION_CHECK_PANIC(wt_session));
    }

    /*
     * Sync the file over. Then rename it so on completion it is an "atomic" operation.
     */
    WT_ERR(dest_fh->fh_sync(dest_fh, wt_session));
    WT_ERR(dest_fh->close(dest_fh, wt_session));
    dest_closed = true;
    WT_ERR(lr_fs->os_file_system->fs_rename(
      lr_fs->os_file_system, wt_session, tmp_dest_path, dest_path, 0));

err:
    if (source_fh != NULL)
        WT_TRET(source_fh->close(source_fh, wt_session));
    if (!dest_closed && dest_fh != NULL)
        WT_TRET(dest_fh->close(dest_fh, wt_session));
    __wt_free(session, buf);
    __wt_free(session, source_path);
    __wt_free(session, dest_path);
    __wt_free(session, tmp_dest_path);
    return (ret);
}

/*
 * __live_restore_fs_create_destination_data_file --
 *     Create a destination file backed by a source file for the first time, atomically sizing it.
 */
static int
__live_restore_fs_create_destination_data_file(WT_SESSION_IMPL *session, WTI_LIVE_RESTORE_FS *lr_fs,
  WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh, const char *name)
{
    WT_DECL_RET;

    char *dest_path, *tmp_dest_path;

    /* This function should only ever be called for data files. */
    WT_ASSERT(session, lr_fh->file_type == WT_FS_OPEN_FILE_TYPE_DATA);

    WT_RET(__live_restore_fs_backing_filename(
      session, lr_fs, WTI_LIVE_RESTORE_FS_LAYER_DESTINATION, name, &dest_path));

    WT_FILE_HANDLE *dest_fh = NULL;
    WT_SESSION *wt_session = (WT_SESSION *)session;
    wt_off_t source_size;
    bool closed = false;
    /* We may have crashed during a temporary file copy, remove that file now. */
    WT_ERR(__live_restore_remove_temporary_file(
      session, lr_fs->os_file_system, dest_path, &tmp_dest_path));

    WT_ERR(lr_fs->os_file_system->fs_open_file(lr_fs->os_file_system, wt_session, tmp_dest_path,
      WT_FS_OPEN_FILE_TYPE_DATA, WT_FS_OPEN_CREATE | WT_FS_OPEN_EXCLUSIVE, &dest_fh));

    /* Get the source size. */
    WT_ERR(lr_fh->source->fh_size(lr_fh->source, wt_session, &source_size));
    WT_ASSERT(session, source_size != 0);
    __wt_verbose_debug1(session, WT_VERB_LIVE_RESTORE,
      "%s: Creating destination file backed by source file", tmp_dest_path);
    /*
     * We're creating a new destination file which is backed by a source file. It currently has a
     * length of zero, but we want its length to be the same as the source file. Set its size by
     * truncating. This is a positive length truncate so it actually extends the file. We're
     * bypassing the live_restore layer so we don't try to modify the relevant bitmap entries.
     */
    WT_ERR(dest_fh->fh_truncate(dest_fh, wt_session, source_size));

    /* Sync the truncate, then rename the file so on completion it is an "atomic" operation. */
    WT_ERR(dest_fh->fh_sync(dest_fh, wt_session));
    WT_ERR(dest_fh->close(dest_fh, wt_session));
    closed = true;
    WT_ERR(lr_fs->os_file_system->fs_rename(
      lr_fs->os_file_system, wt_session, tmp_dest_path, dest_path, 0));

err:
    if (dest_fh != NULL && !closed)
        WT_TRET(dest_fh->close(dest_fh, wt_session));
    __wt_free(session, dest_path);
    __wt_free(session, tmp_dest_path);
    return (ret);
}

/*
 * __live_restore_setup_lr_fh_file_data --
 *     Open a data file type (probably a b-tree). In live restore these are the only types of files
 *     that we track holes for.
 */
static int
__live_restore_setup_lr_fh_file_data(WT_SESSION_IMPL *session, WTI_LIVE_RESTORE_FS *lr_fs,
  const char *name, uint32_t flags, WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh, bool dest_exist,
  bool source_exist)
{
    /* Open the source file and setup the destination file if needed. */
    if (source_exist) {
        /*
         * In theory we have already completed the migration for this file which would mean this
         * open call is redundant and the file will be immediately closed out. But we know the flags
         * here and also whether or not the source file exists so it's easier to open it here and
         * close it out later than to determine that information when importing the bitmap.
         */
        WT_RET(__live_restore_fs_open_in_source(lr_fs, session, lr_fh, flags));
        if (!dest_exist)
            WT_RET(__live_restore_fs_create_destination_data_file(session, lr_fs, lr_fh, name));
    }
    WT_RET(__live_restore_fs_open_in_destination(lr_fs, session, lr_fh, name, flags, !dest_exist));
    return (0);
}

/*
 * __live_restore_setup_lr_fh_file_regular --
 *     Populate a live restore file handle for a regular file. Regular files include log files and
 *     are copied on open.
 */
static int
__live_restore_setup_lr_fh_file_regular(WT_SESSION_IMPL *session, WTI_LIVE_RESTORE_FS *lr_fs,
  const char *name, uint32_t flags, WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh, WT_FS_OPEN_FILE_TYPE type,
  bool dest_exist, bool source_exist)
{
    if (!dest_exist && source_exist)
        /* Atomically copy across the file. */
        WT_RET(__live_restore_fs_atomic_copy_file(session, lr_fs, type, name));

    WT_RET(__live_restore_fs_open_in_destination(lr_fs, session, lr_fh, name, flags, !dest_exist));
    return (0);
}

/*
 * __live_restore_setup_lr_fh_file --
 *     Setup a live restore file handle for a file. This function does some initial file state
 *     investigation before calling separate functions depending on the type of file.
 */
static int
__live_restore_setup_lr_fh_file(WT_SESSION_IMPL *session, WTI_LIVE_RESTORE_FS *lr_fs,
  const char *name, WT_FS_OPEN_FILE_TYPE file_type, uint32_t flags,
  WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh)
{
    WT_DECL_RET;
    char *buf = NULL, *path = NULL;
    /*!!!
     * All non directory open file calls end up here, which means we need to handle:
     *  - WT_FS_OPEN_FILE_TYPE_CHECKPOINT
     *  - WT_FS_OPEN_FILE_TYPE_DATA
     *  - WT_FS_OPEN_FILE_TYPE_LOG
     *  - WT_FS_OPEN_FILE_TYPE_REGULAR
     *
     * Right now we handle everything but the checkpoint type which appears to be unused. Log and
     * regular files are treated the same in that they are atomically copied on open. Then for any
     * subsequent open they will be immediately complete.
     *
     * Data type files are the b-trees, they are not copied on open and are expected to go through
     * the bitmap import path.
     */
    WT_ASSERT(session, file_type != WT_FS_OPEN_FILE_TYPE_CHECKPOINT);

    /*!!!
     * We need to handle a number of scenario in this function providing us with a somewhat complex
     * decision tree. The relevant pieces of state for any file are:
     *   - Whether the live restore is complete or not.
     *   - Whether a stop file exists for that file.
     *   - Whether that file exists in the destination or the source.
     *   - Flag combinations such as create and exclusive.
     *
     * First determine if live restore is complete, whether the stop file exists and if we need to
     * check the source file based off that information.
     */

    bool dest_exist = false, have_stop = false,
         check_source = !__wti_live_restore_migration_complete(session);

    WT_RET_NOTFOUND_OK(
      __live_restore_fs_has_file(lr_fs, &lr_fs->destination, session, name, &dest_exist));
    if (check_source) {
        WT_RET(__dest_has_stop_file(lr_fs, (char *)name, session, &have_stop));
        check_source = !have_stop;
    }

    bool source_exist = false;
    if (check_source) {
        WT_RET_NOTFOUND_OK(
          __live_restore_fs_has_file(lr_fs, &lr_fs->source, session, name, &source_exist));
    }

    bool create = LF_ISSET(WT_FS_OPEN_CREATE);
    if ((dest_exist || source_exist) && create && LF_ISSET(WT_FS_OPEN_EXCLUSIVE))
        WT_RET_MSG(
          session, EEXIST, "File %s already exist, cannot be created due to exclusive flag", name);
    if (!dest_exist && !source_exist && !create)
        WT_RET_MSG(session, ENOENT, "File %s doesn't exist but create flag not specified", name);
    if (!dest_exist && have_stop && !create)
        WT_RET_MSG(session, ENOENT, "File %s has been deleted in the destination", name);

#if defined(__APPLE__) || defined(__linux__)
    /*
     * MongoDB uses a nested directory structure for directory per db and directory for indexes
     * configurations, live restore needs to detect this from the file path, if the directories do
     * not exist then we need to create them manually. A file with nested directory is like:
     * "home/home_dest/sub_dir1/.../sub_dirN/file.wt".
     */
    /* FIXME-WT-14051 - Add live restore support to Windows. */
    if (!dest_exist) {
        WT_ERR(__live_restore_fs_backing_filename(
          session, lr_fs, WTI_LIVE_RESTORE_FS_LAYER_DESTINATION, name, &path));
        char *p = path + strlen(lr_fs->destination.home) + 1;
        size_t len;
        bool dir_exist = false;
        for (; (p = strstr(p, __wt_path_separator())) != NULL; p++) {
            len = (size_t)(p - path);
            /* +1 for the null terminator. */
            WT_ERR(__wt_calloc(session, 1, len + 1, &buf));
            WT_ERR(__wt_snprintf(buf, len + 1, "%.*s", (int)len, path));

            lr_fs->os_file_system->fs_exist(
              lr_fs->os_file_system, (WT_SESSION *)session, buf, &dir_exist);
            if (!dir_exist) {
                WT_SYSCALL(mkdir(buf, 0755), ret);
                /* Handle mkdir() failure, allow EEXIST if another thread had created the dir. */
                if (ret != 0) {
                    WT_ERR_ERROR_OK(ret, EEXIST, true);
                    struct stat stats;
                    WT_SYSCALL(stat(buf, &stats), ret);
                    WT_ERR(ret);
                    if (!S_ISDIR(stats.st_mode))
                        WT_ERR(ENOTDIR);
                }
            }
            __wt_free(session, buf);
        }
        __wt_free(session, path);
    }
#endif

    if (file_type == WT_FILE_TYPE_DATA)
        WT_ERR(__live_restore_setup_lr_fh_file_data(
          session, lr_fs, name, flags, lr_fh, dest_exist, source_exist));
    else
        WT_ERR(__live_restore_setup_lr_fh_file_regular(
          session, lr_fs, name, flags, lr_fh, file_type, dest_exist, source_exist));

    if (0) {
err:
        __wt_free(session, path);
        __wt_free(session, buf);
    }
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

    WT_ERR(__wt_rwlock_init(session, &lr_fh->lock));

    if (file_type == WT_FS_OPEN_FILE_TYPE_DIRECTORY)
        WT_ERR(__live_restore_setup_lr_fh_directory(session, lr_fs, name, flags, lr_fh));
    else
        WT_ERR(__live_restore_setup_lr_fh_file(session, lr_fs, name, file_type, flags, lr_fh));

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

    session = (WT_SESSION_IMPL *)wt_session;
    lr_fs = (WTI_LIVE_RESTORE_FS *)fs;

    path = NULL;

    WT_RET(__live_restore_fs_find_layer(fs, session, name, &layer));
    switch (layer) {
    case WTI_LIVE_RESTORE_FS_LAYER_NONE:
        return (ENOENT);
    case WTI_LIVE_RESTORE_FS_LAYER_DESTINATION:
        WT_ERR(__live_restore_fs_backing_filename(
          session, lr_fs, WTI_LIVE_RESTORE_FS_LAYER_DESTINATION, name, &path));
        WT_ERR(lr_fs->os_file_system->fs_remove(lr_fs->os_file_system, wt_session, path, flags));
        /* FALLTHROUGH */
    case WTI_LIVE_RESTORE_FS_LAYER_SOURCE:
        /*
         * It's possible to call remove on a file that hasn't yet been created in the destination.
         * In these cases we only need to create the stop file.
         */
        WT_ERR(__live_restore_fs_create_stop_file(fs, session, name, flags));
        break;
    }
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

    session = (WT_SESSION_IMPL *)wt_session;
    lr_fs = (WTI_LIVE_RESTORE_FS *)fs;

    path_from = NULL;
    path_to = NULL;

    /*
     * WiredTiger frequently renames the turtle file, and some other files. This function is more
     * critical than it may seem at first.
     */
    __wt_verbose_debug1(
      session, WT_VERB_LIVE_RESTORE, "LIVE_RESTORE: Renaming file from: %s to %s", from, to);

    WT_RET(__live_restore_fs_find_layer(fs, session, from, &which));
    if (which == WTI_LIVE_RESTORE_FS_LAYER_NONE)
        WT_RET_MSG(session, ENOENT, "Live restore cannot find: %s", from);

    /*
     * A call to rename must succeed from the perspective of WiredTiger, it knows that the file that
     * it wants to rename exists. As a result of deprecating schema->rename WiredTiger will only
     * ever rename regular files. Thus files can never be in a partially migrated state during a
     * rename.
     *
     * The typical rename scenario is when WiredTiger creates a new temporary turtle file,
     * initializes it and then renames it over the top of the existing one. The act of creation
     * followed by writing to it means that it must exist in the destination for it to be renamed.
     *
     * We leverage this and the regular file copy-on-open behavior to enforce that renamed files
     * must first exist in the destination. Sadly at this level we cannot check whether a file is a
     * regular file or not as we only have access to the file system, not the individual file
     * handles.
     */
    if (which != WTI_LIVE_RESTORE_FS_LAYER_DESTINATION)
        WT_RET_MSG(session, EINVAL, "Rename failed as file does not exist in destination");

    WT_ERR(__live_restore_fs_backing_filename(
      session, lr_fs, WTI_LIVE_RESTORE_FS_LAYER_DESTINATION, from, &path_from));
    WT_ERR(__live_restore_fs_backing_filename(
      session, lr_fs, WTI_LIVE_RESTORE_FS_LAYER_DESTINATION, to, &path_to));
    WT_ERR(lr_fs->os_file_system->fs_rename(
      lr_fs->os_file_system, wt_session, path_from, path_to, flags));

    /* Even if we don't modify a backing file we need to update metadata. */
    WT_ERR(__live_restore_fs_create_stop_file(fs, session, to, flags));
    WT_ERR(__live_restore_fs_create_stop_file(fs, session, from, flags));

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

    session = (WT_SESSION_IMPL *)wt_session;
    lr_fs = (WTI_LIVE_RESTORE_FS *)fs;

    path = NULL;

    WT_RET(__live_restore_fs_find_layer(fs, session, name, &which));
    if (which == WTI_LIVE_RESTORE_FS_LAYER_NONE)
        WT_RET_MSG(session, ENOENT, "Live restore cannot find: %s", name);

    /* Get the file size from the destination if possible, otherwise fallback to the source. */
    WT_RET(__live_restore_fs_backing_filename(session, lr_fs, which, name, &path));
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

    __wt_spin_destroy(session, &lr_fs->state_lock);
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
    WTI_LIVE_RESTORE_FS *lr_fs;

    /* FIXME-WT-14223: Remove this once readonly database connections are supported. */
    if (F_ISSET(S2C(session), WT_CONN_READONLY))
        WT_RET_MSG(session, EINVAL, "live restore is incompatible with readonly mode");

    WT_RET(__wt_calloc_one(session, &lr_fs));
#if defined(__APPLE__) || defined(__linux__)
    /* FIXME-WT-14051 - Add live restore support to Windows. */
    WT_ERR(__wt_os_posix(session, &lr_fs->os_file_system));
#endif

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

    WT_ERR(__wt_spin_init(session, &lr_fs->state_lock, "live restore state lock"));

    /*
     * To initialize the live restore file system we need to read its state from the turtle file,
     * but to open the turtle file we need a working file system. Temporarily set WiredTiger's file
     * system to the underlying file system so we can open the turtle file in the destination. We'll
     * set the correct live restore file as soon as possible.
     */
    *fsp = lr_fs->os_file_system;
    WT_ERR(__wti_live_restore_validate_directories(session, lr_fs));
    WT_ERR(__wti_live_restore_init_state(session, lr_fs));

    /* Now set the proper live restore file system. */
    *fsp = (WT_FILE_SYSTEM *)lr_fs;

    /* Flag that a live restore file system is in use. */
    F_SET(S2C(session), WT_CONN_LIVE_RESTORE_FS);
    if (0) {
err:
        /*
         * If we swapped in the posix file system don't terminate it. It'll get terminated later
         * when cleaning up the connection.
         */
        if (*fsp != lr_fs->os_file_system && lr_fs->os_file_system != NULL)
            WT_TRET(lr_fs->os_file_system->terminate(lr_fs->os_file_system, (WT_SESSION *)session));
        __wt_free(session, lr_fs->source.home);
        __wt_free(session, lr_fs);
    }
    return (ret);
}

#ifdef HAVE_UNITTEST
int
__ut_live_restore_encode_bitmap(
  WT_SESSION_IMPL *session, WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh, WT_ITEM *buf)
{
    return (__live_restore_encode_bitmap(session, lr_fh, buf));
}

int
__ut_live_restore_decode_bitmap(WT_SESSION_IMPL *session, const char *bitmap_str, uint64_t nbits,
  WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh)
{
    return (__live_restore_decode_bitmap(session, bitmap_str, nbits, lr_fh));
}

void
__ut_live_restore_fh_fill_bit_range(
  WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh, WT_SESSION_IMPL *session, wt_off_t offset, size_t len)
{
    __live_restore_fh_fill_bit_range(lr_fh, session, offset, len);
}

int
__ut_live_restore_compute_read_end_bit(WT_SESSION_IMPL *session,
  WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh, wt_off_t buf_size, uint64_t first_clear_bit,
  uint64_t *end_bitp)
{
    return (
      __live_restore_compute_read_end_bit(session, lr_fh, buf_size, first_clear_bit, end_bitp));
}

int
__ut_live_restore_fill_hole(WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh, WT_SESSION *wt_session, char *buf,
  wt_off_t buf_size, wt_off_t *read_offsetp, bool *finishedp)
{
    return (__live_restore_fill_hole(lr_fh, wt_session, buf, buf_size, read_offsetp, finishedp));
}
#endif
