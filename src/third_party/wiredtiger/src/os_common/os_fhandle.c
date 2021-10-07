/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __fhandle_method_finalize --
 *     Initialize any NULL WT_FH structure methods to not-supported. Doing this means that custom
 *     file systems with incomplete implementations won't dereference NULL pointers.
 */
static int
__fhandle_method_finalize(WT_SESSION_IMPL *session, WT_FILE_HANDLE *handle, bool readonly)
{
#define WT_HANDLE_METHOD_REQ(name) \
    if (handle->name == NULL)      \
    WT_RET_MSG(session, EINVAL, "a WT_FILE_HANDLE.%s method must be configured", #name)

    WT_HANDLE_METHOD_REQ(close);
    /* not required: fh_advise */
    /* not required: fh_extend */
    /* not required: fh_extend_nolock */
    WT_HANDLE_METHOD_REQ(fh_lock);
    /* not required: fh_map */
    /* not required: fh_map_discard */
    /* not required: fh_map_preload */
    /* not required: fh_unmap */
    WT_HANDLE_METHOD_REQ(fh_read);
    WT_HANDLE_METHOD_REQ(fh_size);
    if (!readonly)
        WT_HANDLE_METHOD_REQ(fh_sync);
    /* not required: fh_sync_nowait */
    /* not required: fh_truncate */
    if (!readonly)
        WT_HANDLE_METHOD_REQ(fh_write);

    return (0);
}

/*
 * __wt_handle_is_open --
 *     Return if there's an open handle matching a name.
 */
bool
__wt_handle_is_open(WT_SESSION_IMPL *session, const char *name)
{
    WT_CONNECTION_IMPL *conn;
    WT_FH *fh;
    uint64_t bucket, hash;
    bool found;

    conn = S2C(session);
    found = false;

    hash = __wt_hash_city64(name, strlen(name));
    bucket = hash & (conn->hash_size - 1);

    __wt_spin_lock(session, &conn->fh_lock);

    TAILQ_FOREACH (fh, &conn->fhhash[bucket], hashq)
        if (strcmp(name, fh->name) == 0) {
            found = true;
            break;
        }

    __wt_spin_unlock(session, &conn->fh_lock);

    return (found);
}

/*
 * __handle_search --
 *     Search for a matching handle.
 */
static bool
__handle_search(WT_SESSION_IMPL *session, const char *name, WT_FH *newfh, WT_FH **fhp)
{
    WT_CONNECTION_IMPL *conn;
    WT_FH *fh;
    uint64_t bucket, hash;
    bool found;

    *fhp = NULL;

    conn = S2C(session);
    found = false;

    hash = __wt_hash_city64(name, strlen(name));
    bucket = hash & (conn->hash_size - 1);

    __wt_spin_lock(session, &conn->fh_lock);

    /*
     * If we already have the file open, increment the reference count and return a pointer.
     */
    TAILQ_FOREACH (fh, &conn->fhhash[bucket], hashq)
        if (strcmp(name, fh->name) == 0) {
            ++fh->ref;
            *fhp = fh;
            found = true;
            break;
        }

    /* If we don't find a match, optionally add a new entry. */
    if (!found && newfh != NULL) {
        newfh->name_hash = hash;
        WT_FILE_HANDLE_INSERT(conn, newfh, bucket);
        (void)__wt_atomic_add32(&conn->open_file_count, 1);

        ++newfh->ref;
        *fhp = newfh;
    }

    __wt_spin_unlock(session, &conn->fh_lock);

    return (found);
}

/*
 * __open_verbose_file_type_tag --
 *     Return a string describing a file type.
 */
static const char *
__open_verbose_file_type_tag(WT_FS_OPEN_FILE_TYPE file_type)
{

    /*
     * WT_FS_OPEN_FILE_TYPE is an enum and the switch exhaustively lists the cases, but clang, lint
     * and gcc argue over whether or not the switch is exhaustive, or if a temporary variable
     * inserted into the mix is set but never read. Break out of the switch, returning some value in
     * all cases, just to shut everybody up.
     */
    switch (file_type) {
    case WT_FS_OPEN_FILE_TYPE_CHECKPOINT:
        return ("checkpoint");
    case WT_FS_OPEN_FILE_TYPE_DATA:
        return ("data");
    case WT_FS_OPEN_FILE_TYPE_DIRECTORY:
        return ("directory");
    case WT_FS_OPEN_FILE_TYPE_LOG:
        return ("log");
    case WT_FS_OPEN_FILE_TYPE_REGULAR:
        break;
    }
    return ("regular");
}

/*
 * __open_verbose --
 *     Optionally output a verbose message on handle open.
 */
static inline int
__open_verbose(
  WT_SESSION_IMPL *session, const char *name, WT_FS_OPEN_FILE_TYPE file_type, u_int flags)
{
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    const char *sep;

    if (!WT_VERBOSE_ISSET(session, WT_VERB_FILEOPS))
        return (0);

    /*
     * It's useful to track file opens when debugging platforms, take some effort to output good
     * tracking information.
     */
    WT_RET(__wt_scr_alloc(session, 0, &tmp));
    sep = " (";
#define WT_FS_OPEN_VERBOSE_FLAG(f, name)                          \
    if (LF_ISSET(f)) {                                            \
        WT_ERR(__wt_buf_catfmt(session, tmp, "%s%s", sep, name)); \
        sep = ", ";                                               \
    }

    WT_FS_OPEN_VERBOSE_FLAG(WT_FS_OPEN_CREATE, "create");
    WT_FS_OPEN_VERBOSE_FLAG(WT_FS_OPEN_DIRECTIO, "direct-IO");
    WT_FS_OPEN_VERBOSE_FLAG(WT_FS_OPEN_EXCLUSIVE, "exclusive");
    WT_FS_OPEN_VERBOSE_FLAG(WT_FS_OPEN_FIXED, "fixed");
    WT_FS_OPEN_VERBOSE_FLAG(WT_FS_OPEN_READONLY, "readonly");

    if (tmp->size != 0)
        WT_ERR(__wt_buf_catfmt(session, tmp, ")"));

    __wt_verbose(session, WT_VERB_FILEOPS, "%s: file-open: type %s%s", name,
      __open_verbose_file_type_tag(file_type), tmp->size == 0 ? "" : (char *)tmp->data);

err:
    __wt_scr_free(session, &tmp);
    return (ret);
}

/*
 * __wt_open --
 *     Open a file handle.
 */
int
__wt_open(WT_SESSION_IMPL *session, const char *name, WT_FS_OPEN_FILE_TYPE file_type, u_int flags,
  WT_FH **fhp)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_FH *fh;
    WT_FILE_SYSTEM *file_system;
    char *path;
    bool lock_file, open_called;

    WT_ASSERT(session, file_type != 0); /* A file type is required. */

    *fhp = NULL;

    conn = S2C(session);
    file_system = __wt_fs_file_system(session);
    fh = NULL;
    open_called = false;
    path = NULL;

    WT_RET(__open_verbose(session, name, file_type, flags));

    /* Check if the handle is already open. */
    if (__handle_search(session, name, NULL, &fh)) {
        *fhp = fh;
        return (0);
    }

    /* Allocate and initialize the handle. */
    WT_ERR(__wt_calloc_one(session, &fh));
    WT_ERR(__wt_strdup(session, name, &fh->name));

    fh->file_type = file_type;

    /*
     * If this is a read-only connection, open all files read-only except the lock file.
     *
     * The only file created in read-only mode is the lock file.
     */
    if (F_ISSET(conn, WT_CONN_READONLY)) {
        lock_file = strcmp(name, WT_SINGLETHREAD) == 0;
        if (!lock_file)
            LF_SET(WT_FS_OPEN_READONLY);
        WT_ASSERT(session, lock_file || !LF_ISSET(WT_FS_OPEN_CREATE));
    }

    /* Create the path to the file. */
    if (!LF_ISSET(WT_FS_OPEN_FIXED))
        WT_ERR(__wt_filename(session, name, &path));

    /* Call the underlying open function. */
    WT_ERR(file_system->fs_open_file(
      file_system, &session->iface, path == NULL ? name : path, file_type, flags, &fh->handle));
    open_called = true;

    WT_ERR(__fhandle_method_finalize(session, fh->handle, LF_ISSET(WT_FS_OPEN_READONLY)));

    /*
     * Repeat the check for a match: if there's no match, link our newly created handle onto the
     * database's list of files.
     */
    if (__handle_search(session, name, fh, fhp)) {
err:
        if (open_called)
            WT_TRET(fh->handle->close(fh->handle, (WT_SESSION *)session));
        if (fh != NULL) {
            __wt_free(session, fh->name);
            __wt_free(session, fh);
        }
    }

    __wt_free(session, path);
    return (ret);
}

/*
 * __handle_close --
 *     Final close of a handle.
 */
static int
__handle_close(WT_SESSION_IMPL *session, WT_FH *fh, bool locked)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    uint64_t bucket;

    conn = S2C(session);

    if (fh->ref != 0)
        __wt_errx(session, "Closing a file handle with open references: %s", fh->name);

    /* Remove from the list. */
    bucket = fh->name_hash & (conn->hash_size - 1);
    WT_FILE_HANDLE_REMOVE(conn, fh, bucket);
    (void)__wt_atomic_sub32(&conn->open_file_count, 1);

    if (locked)
        __wt_spin_unlock(session, &conn->fh_lock);

    /* Discard underlying resources. */
    WT_TRET(fh->handle->close(fh->handle, (WT_SESSION *)session));

    __wt_free(session, fh->name);
    __wt_free(session, fh);

    return (ret);
}

/*
 * __wt_close --
 *     Close a file handle.
 */
int
__wt_close(WT_SESSION_IMPL *session, WT_FH **fhp)
{
    WT_CONNECTION_IMPL *conn;
    WT_FH *fh;

    conn = S2C(session);

    if (*fhp == NULL)
        return (0);
    fh = *fhp;
    *fhp = NULL;

    /* Track handle-close as a file operation, so open and close match. */
    __wt_verbose(session, WT_VERB_FILEOPS, "%s: file-close", fh->name);

    /*
     * If the reference count hasn't gone to 0, or if it's an in-memory object, we're done.
     *
     * Assert the reference count is correct, but don't let it wrap.
     */
    __wt_spin_lock(session, &conn->fh_lock);
    WT_ASSERT(session, fh->ref > 0);
    if ((fh->ref > 0 && --fh->ref > 0)) {
        __wt_spin_unlock(session, &conn->fh_lock);
        return (0);
    }

    return (__handle_close(session, fh, true));
}

/*
 * __wt_fsync_background_chk --
 *     Return if background fsync is supported.
 */
bool
__wt_fsync_background_chk(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_FH *fh;
    WT_FILE_HANDLE *handle;
    bool supported;

    conn = S2C(session);
    supported = true;
    __wt_spin_lock(session, &conn->fh_lock);
    /*
     * Look for the first data file handle and see if the fsync nowait function is supported.
     */
    TAILQ_FOREACH (fh, &conn->fhqh, q) {
        handle = fh->handle;
        if (fh->file_type != WT_FS_OPEN_FILE_TYPE_DATA)
            continue;
        /*
         * If we don't have a function, return false, otherwise return true. In any case, we are
         * done with the loop.
         */
        if (handle->fh_sync_nowait == NULL)
            supported = false;
        break;
    }
    __wt_spin_unlock(session, &conn->fh_lock);
    return (supported);
}

/*
 * __fsync_background --
 *     Background fsync for a single dirty file handle.
 */
static int
__fsync_background(WT_SESSION_IMPL *session, WT_FH *fh)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_FILE_HANDLE *handle;
    uint64_t now;

    conn = S2C(session);
    WT_STAT_CONN_INCR(session, fsync_all_fh_total);

    handle = fh->handle;
    if (handle->fh_sync_nowait == NULL || fh->written < WT_CAPACITY_FILE_THRESHOLD)
        return (0);

    /* Only sync data files. */
    if (fh->file_type != WT_FS_OPEN_FILE_TYPE_DATA)
        return (0);

    now = __wt_clock(session);
    if (fh->last_sync == 0 || WT_CLOCKDIFF_SEC(now, fh->last_sync) > 0) {
        __wt_spin_unlock(session, &conn->fh_lock);

        /*
         * We set the false flag to indicate a non-blocking background fsync, but there is no
         * guarantee that it doesn't block. If we wanted to detect if it is blocking, adding a clock
         * call and checking the time would be done here.
         */
        ret = __wt_fsync(session, fh, false);
        if (ret == 0) {
            WT_STAT_CONN_INCR(session, fsync_all_fh);
            fh->last_sync = now;
            fh->written = 0;
        }

        __wt_spin_lock(session, &conn->fh_lock);
    }
    return (ret);
}

/*
 * __wt_fsync_background --
 *     Background fsync for all dirty file handles.
 */
int
__wt_fsync_background(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_FH *fh, *fhnext;

    conn = S2C(session);
    __wt_spin_lock(session, &conn->fh_lock);
    TAILQ_FOREACH_SAFE(fh, &conn->fhqh, q, fhnext)
    {
        /*
         * The worker routine will unlock the list to avoid holding it locked over an fsync.
         * Increment the count on the current and next handles to guarantee their validity.
         */
        if (fhnext != NULL)
            ++fhnext->ref;
        ++fh->ref;

        WT_TRET(__fsync_background(session, fh));

        /*
         * The file handle reference may have gone to 0, in which case we're responsible for the
         * close. Configure the close routine to drop the lock, which means we must re-acquire it.
         */
        if (--fh->ref == 0) {
            WT_TRET(__handle_close(session, fh, true));
            __wt_spin_lock(session, &conn->fh_lock);
        }

        /*
         * Decrement the next element's reference count. It might have gone to 0 as well, in which
         * case we'll close it in the next loop iteration.
         */
        if (fhnext != NULL)
            --fhnext->ref;
    }
    __wt_spin_unlock(session, &conn->fh_lock);
    return (ret);
}

/*
 * __wt_close_connection_close --
 *     Close any open file handles at connection close.
 */
int
__wt_close_connection_close(WT_SESSION_IMPL *session)
{
    WT_DECL_RET;
    WT_FH *fh, *fh_tmp;

    WT_TAILQ_SAFE_REMOVE_BEGIN(fh, &S2C(session)->fhqh, q, fh_tmp)
    {
        WT_TRET(__handle_close(session, fh, false));
    }
    WT_TAILQ_SAFE_REMOVE_END
    return (ret);
}

/*
 * __wt_file_zero --
 *     Zero out the file from offset for size bytes.
 */
int
__wt_file_zero(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t start_off, wt_off_t size)
{
    WT_DECL_ITEM(zerobuf);
    WT_DECL_RET;
    WT_THROTTLE_TYPE type;
    uint64_t bufsz, off, partial, wrlen;

    zerobuf = NULL;
    bufsz = WT_MIN((uint64_t)size, WT_MEGABYTE);
    /*
     * For now logging is the only type and statistic. This needs updating if block manager decides
     * to use this function.
     */
    type = WT_THROTTLE_LOG;
    WT_STAT_CONN_INCR(session, log_zero_fills);
    WT_RET(__wt_scr_alloc(session, bufsz, &zerobuf));
    memset(zerobuf->mem, 0, zerobuf->memsize);
    off = (uint64_t)start_off;
    while (off < (uint64_t)size) {
        /*
         * We benefit from aligning our writes when we can. Log files will typically want to start
         * to zero after the log header and the bufsz is a sector-aligned size. So align when we
         * can.
         */
        partial = off % bufsz;
        if (partial != 0)
            wrlen = bufsz - partial;
        else
            wrlen = bufsz;
        /*
         * Check if we're writing a partial amount at the end too.
         */
        if ((uint64_t)size - off < bufsz)
            wrlen = (uint64_t)size - off;
        __wt_capacity_throttle(session, wrlen, type);
        WT_ERR(__wt_write(session, fh, (wt_off_t)off, (size_t)wrlen, zerobuf->mem));
        off += wrlen;
    }
err:
    __wt_scr_free(session, &zerobuf);
    return (ret);
}
