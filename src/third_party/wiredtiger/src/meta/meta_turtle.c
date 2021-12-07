/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __metadata_config --
 *     Return the default configuration information for the metadata file.
 */
static int
__metadata_config(WT_SESSION_IMPL *session, char **metaconfp)
{
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    const char *cfg[] = {WT_CONFIG_BASE(session, file_meta), NULL, NULL};

    *metaconfp = NULL;

    /* Create a turtle file with default values. */
    WT_RET(__wt_scr_alloc(session, 0, &buf));
    WT_ERR(
      __wt_buf_fmt(session, buf, "key_format=S,value_format=S,id=%d,version=(major=%d,minor=%d)",
        WT_METAFILE_ID, WT_BTREE_MAJOR_VERSION_MAX, WT_BTREE_MINOR_VERSION_MAX));
    cfg[1] = buf->data;
    ret = __wt_config_collapse(session, cfg, metaconfp);

err:
    __wt_scr_free(session, &buf);
    return (ret);
}

/*
 * __metadata_init --
 *     Create the metadata file.
 */
static int
__metadata_init(WT_SESSION_IMPL *session)
{
    WT_DECL_RET;

    /*
     * We're single-threaded, but acquire the schema lock regardless: the lower level code checks
     * that it is appropriately synchronized.
     */
    WT_WITH_SCHEMA_LOCK(session, ret = __wt_schema_create(session, WT_METAFILE_URI, NULL));

    return (ret);
}

/*
 * __metadata_load_hot_backup --
 *     Load the contents of any hot backup file.
 */
static int
__metadata_load_hot_backup(WT_SESSION_IMPL *session)
{
    WT_DECL_ITEM(key);
    WT_DECL_ITEM(value);
    WT_DECL_RET;
    WT_FSTREAM *fs;
    bool exist;

    /* Look for a hot backup file: if we find it, load it. */
    WT_RET(__wt_fs_exist(session, WT_METADATA_BACKUP, &exist));
    if (!exist)
        return (0);
    WT_RET(__wt_fopen(session, WT_METADATA_BACKUP, 0, WT_STREAM_READ, &fs));

    /* Read line pairs and load them into the metadata file. */
    WT_ERR(__wt_scr_alloc(session, 512, &key));
    WT_ERR(__wt_scr_alloc(session, 512, &value));
    for (;;) {
        WT_ERR(__wt_getline(session, fs, key));
        if (key->size == 0)
            break;
        WT_ERR(__wt_getline(session, fs, value));
        if (value->size == 0)
            WT_ERR_PANIC(session, EINVAL, "%s: zero-length value", WT_METADATA_BACKUP);
        WT_ERR(__wt_metadata_update(session, key->data, value->data));
    }

    F_SET(S2C(session), WT_CONN_WAS_BACKUP);

err:
    WT_TRET(__wt_fclose(session, &fs));
    __wt_scr_free(session, &key);
    __wt_scr_free(session, &value);
    return (ret);
}

/*
 * __metadata_load_bulk --
 *     Create any bulk-loaded file stubs.
 */
static int
__metadata_load_bulk(WT_SESSION_IMPL *session)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    uint32_t allocsize;
    const char *filecfg[] = {WT_CONFIG_BASE(session, file_meta), NULL, NULL};
    const char *key, *value;
    bool exist;

    /*
     * If a file was being bulk-loaded during the hot backup, it will appear in the metadata file,
     * but the file won't exist. Create on demand.
     */
    WT_RET(__wt_metadata_cursor(session, &cursor));
    while ((ret = cursor->next(cursor)) == 0) {
        WT_ERR(cursor->get_key(cursor, &key));
        if (!WT_PREFIX_SKIP(key, "file:"))
            continue;

        /* If the file exists, it's all good. */
        WT_ERR(__wt_fs_exist(session, key, &exist));
        if (exist)
            continue;

        /*
         * If the file doesn't exist, assume it's a bulk-loaded file; retrieve the allocation size
         * and re-create the file.
         */
        WT_ERR(cursor->get_value(cursor, &value));
        filecfg[1] = value;
        WT_ERR(__wt_direct_io_size_check(session, filecfg, "allocation_size", &allocsize));
        WT_ERR(__wt_block_manager_create(session, key, allocsize));
    }
    WT_ERR_NOTFOUND_OK(ret, false);

err:
    WT_TRET(__wt_metadata_cursor_release(session, &cursor));
    /*
     * We want to explicitly close, not just release the metadata cursor here. We know we are in
     * initialization and this open cursor holds a lock on the metadata and we may need to verify
     * the metadata.
     */
    WT_TRET(__wt_metadata_cursor_close(session));
    return (ret);
}

/*
 * __wt_turtle_validate_version --
 *     Retrieve version numbers from the turtle file and validate them against our WiredTiger
 *     version.
 */
int
__wt_turtle_validate_version(WT_SESSION_IMPL *session)
{
    WT_DECL_RET;
    uint32_t major, minor, patch;
    char *version_string;

    WT_WITH_TURTLE_LOCK(
      session, ret = __wt_turtle_read(session, WT_METADATA_VERSION, &version_string));

    if (ret != 0)
        WT_ERR_MSG(session, ret, "Unable to read version string from turtle file");

    if ((ret = sscanf(version_string, "major=%u,minor=%u,patch=%u", &major, &minor, &patch)) != 3)
        WT_ERR_MSG(session, ret, "Unable to parse turtle file version string");

    ret = 0;

    if (major < WT_MIN_STARTUP_VERSION_MAJOR ||
      (major == WT_MIN_STARTUP_VERSION_MAJOR && minor < WT_MIN_STARTUP_VERSION_MINOR))
        WT_ERR_MSG(session, WT_ERROR, "WiredTiger version incompatible with current binary");

    S2C(session)->recovery_major = major;
    S2C(session)->recovery_minor = minor;
    S2C(session)->recovery_patch = patch;

err:
    __wt_free(session, version_string);
    return (ret);
}

/*
 * __wt_turtle_exists --
 *     Return if the turtle file exists on startup.
 */
int
__wt_turtle_exists(WT_SESSION_IMPL *session, bool *existp)
{
    /*
     * The last thing we do in database initialization is rename a turtle
     * file into place, and there's never a database home after that point
     * without a turtle file. On startup we check if the turtle file exists
     * to decide if we're creating the database or re-opening an existing
     * database.
     *	Unfortunately, we re-write the turtle file at checkpoint end,
     * first creating the "set" file and then renaming it into place.
     * Renames on Windows aren't guaranteed to be atomic, a power failure
     * could leave us with only the set file. The turtle file is the file
     * we regularly rename when WiredTiger is running, so if we're going to
     * get caught, the turtle file is where it will happen. If we have a set
     * file and no turtle file, rename the set file into place. We don't
     * know what went wrong for sure, so this can theoretically make it
     * worse, but there aren't alternatives other than human intervention.
     */
    WT_RET(__wt_fs_exist(session, WT_METADATA_TURTLE, existp));
    if (*existp)
        return (0);

    WT_RET(__wt_fs_exist(session, WT_METADATA_TURTLE_SET, existp));
    if (!*existp)
        return (0);

    WT_RET(__wt_fs_rename(session, WT_METADATA_TURTLE_SET, WT_METADATA_TURTLE, true));
    WT_RET(__wt_msg(session, "%s not found, %s renamed to %s", WT_METADATA_TURTLE,
      WT_METADATA_TURTLE_SET, WT_METADATA_TURTLE));
    *existp = true;
    return (0);
}

/*
 * __wt_turtle_init --
 *     Check the turtle file and create if necessary.
 */
int
__wt_turtle_init(WT_SESSION_IMPL *session)
{
    WT_DECL_RET;
    char *metaconf, *unused_value;
    bool exist_backup, exist_incr, exist_isrc, exist_turtle;
    bool load, load_turtle, validate_turtle;

    load = load_turtle = validate_turtle = false;

    /*
     * Discard any turtle setup file left-over from previous runs. This doesn't matter for
     * correctness, it's just cleaning up random files.
     */

    if ((ret = __wt_remove_if_exists(session, WT_METADATA_TURTLE_SET, false)) != 0) {
        /* If we're a readonly database, we can skip discarding the leftover file. */
        if (ret == EACCES)
            ret = 0;
        WT_RET(ret);
    }

    /*
     * If we found a corrupted turtle file, then delete it and create a new. We could die after
     * creating the turtle file and before creating the metadata file, or worse, the metadata file
     * might be in some random state. Make sure that doesn't happen: if we don't find the turtle
     * file, first create the metadata file, load any hot backup, and then create the turtle file.
     * No matter what happens, if metadata file creation doesn't fully complete, we won't have a
     * turtle file and we will repeat the process until we succeed.
     *
     * Incremental backups can occur only if recovery is run and it becomes live. So, if there is a
     * turtle file and an incremental backup file, that is an error. Otherwise, if there's already a
     * turtle file, we're done.
     */
    WT_RET(__wt_fs_exist(session, WT_LOGINCR_BACKUP, &exist_incr));
    WT_RET(__wt_fs_exist(session, WT_LOGINCR_SRC, &exist_isrc));
    WT_RET(__wt_fs_exist(session, WT_METADATA_BACKUP, &exist_backup));
    WT_RET(__wt_fs_exist(session, WT_METADATA_TURTLE, &exist_turtle));

    if (exist_turtle) {
        /*
         * Failure to read means a bad turtle file. Remove it and create a new turtle file.
         */
        if (F_ISSET(S2C(session), WT_CONN_SALVAGE)) {
            WT_WITH_TURTLE_LOCK(
              session, ret = __wt_turtle_read(session, WT_METAFILE_URI, &unused_value));
            __wt_free(session, unused_value);
        }

        if (ret != 0) {
            WT_RET(__wt_remove_if_exists(session, WT_METADATA_TURTLE, false));
            load_turtle = true;
        } else
            /*
             * Set a flag to specify that we should validate whether we can start up on the turtle
             * file version seen. Return an error if we can't. Only check if we either didn't run
             * salvage or if salvage didn't fail to read it.
             */
            validate_turtle = true;

        /*
         * We need to detect the difference between a source database that may have crashed with an
         * incremental backup file and a destination database that incorrectly ran recovery.
         */
        if (exist_incr && !exist_isrc)
            WT_RET_MSG(session, EINVAL, "Incremental backup after running recovery is not allowed");
        /*
         * If we have a backup file and metadata and turtle files, we want to recreate the metadata
         * from the backup.
         */
        if (exist_backup) {
            WT_RET(__wt_msg(session, "Both %s and %s exist; recreating metadata from backup",
              WT_METADATA_TURTLE, WT_METADATA_BACKUP));
            WT_RET(__wt_remove_if_exists(session, WT_METAFILE, false));
            WT_RET(__wt_remove_if_exists(session, WT_METADATA_TURTLE, false));
            load = true;
        } else if (validate_turtle)
            WT_RET(__wt_turtle_validate_version(session));
    } else
        load = true;
    if (load) {
        if (exist_incr)
            F_SET(S2C(session), WT_CONN_WAS_BACKUP);

        /* Create the metadata file. */
        WT_RET(__metadata_init(session));

        /* Load any hot-backup information. */
        WT_RET(__metadata_load_hot_backup(session));

        /* Create any bulk-loaded file stubs. */
        WT_RET(__metadata_load_bulk(session));
    }

    if (load || load_turtle) {
        /* Create the turtle file. */
        WT_RET(__metadata_config(session, &metaconf));
        WT_WITH_TURTLE_LOCK(session, ret = __wt_turtle_update(session, WT_METAFILE_URI, metaconf));
        __wt_free(session, metaconf);
        WT_RET(ret);
    }

    /* Remove the backup files, we'll never read them again. */
    return (__wt_backup_file_remove(session));
}

/*
 * __wt_turtle_read --
 *     Read the turtle file.
 */
int
__wt_turtle_read(WT_SESSION_IMPL *session, const char *key, char **valuep)
{
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    WT_FSTREAM *fs;
    bool exist;

    *valuep = NULL;

    /* Require single-threading. */
    WT_ASSERT(session, FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_TURTLE));

    /*
     * Open the turtle file; there's one case where we won't find the turtle file, yet still
     * succeed. We create the metadata file before creating the turtle file, and that means
     * returning the default configuration string for the metadata file.
     */
    WT_RET(__wt_fs_exist(session, WT_METADATA_TURTLE, &exist));
    if (!exist)
        return (
          strcmp(key, WT_METAFILE_URI) == 0 ? __metadata_config(session, valuep) : WT_NOTFOUND);
    WT_RET(__wt_fopen(session, WT_METADATA_TURTLE, 0, WT_STREAM_READ, &fs));

    WT_ERR(__wt_scr_alloc(session, 512, &buf));

    /* Search for the key. */
    do {
        WT_ERR(__wt_getline(session, fs, buf));
        if (buf->size == 0)
            WT_ERR(WT_NOTFOUND);
    } while (strcmp(key, buf->data) != 0);

    /* Key matched: read the subsequent line for the value. */
    WT_ERR(__wt_getline(session, fs, buf));
    if (buf->size == 0)
        WT_ERR(WT_NOTFOUND);

    /* Copy the value for the caller. */
    WT_ERR(__wt_strdup(session, buf->data, valuep));

err:
    WT_TRET(__wt_fclose(session, &fs));
    __wt_scr_free(session, &buf);

    if (ret != 0)
        __wt_free(session, *valuep);

    /*
     * A file error or a missing key/value pair in the turtle file means something has gone horribly
     * wrong, except for the compatibility setting which is optional. Failure to read the turtle
     * file when salvaging means it can't be used for salvage.
     */
    if (ret == 0 || strcmp(key, WT_METADATA_COMPAT) == 0 || F_ISSET(S2C(session), WT_CONN_SALVAGE))
        return (ret);
    F_SET(S2C(session), WT_CONN_DATA_CORRUPTION);
    WT_RET_PANIC(session, WT_TRY_SALVAGE, "%s: fatal turtle file read error", WT_METADATA_TURTLE);
}

/*
 * __wt_turtle_update --
 *     Update the turtle file.
 */
int
__wt_turtle_update(WT_SESSION_IMPL *session, const char *key, const char *value)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_FSTREAM *fs;
    int vmajor, vminor, vpatch;
    const char *version;

    fs = NULL;
    conn = S2C(session);

    /* Require single-threading. */
    WT_ASSERT(session, FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_TURTLE));

    /*
     * Create the turtle setup file: we currently re-write it from scratch every time.
     */
    WT_RET(__wt_fopen(session, WT_METADATA_TURTLE_SET, WT_FS_OPEN_CREATE | WT_FS_OPEN_EXCLUSIVE,
      WT_STREAM_WRITE, &fs));

    /*
     * If a compatibility setting has been explicitly set, save it out to the turtle file.
     */
    if (F_ISSET(conn, WT_CONN_COMPATIBILITY))
        WT_ERR(__wt_fprintf(session, fs,
          "%s\n"
          "major=%d,minor=%d\n",
          WT_METADATA_COMPAT, conn->compat_major, conn->compat_minor));

    version = wiredtiger_version(&vmajor, &vminor, &vpatch);
    WT_ERR(__wt_fprintf(session, fs,
      "%s\n%s\n%s\n"
      "major=%d,minor=%d,patch=%d\n%s\n%s\n",
      WT_METADATA_VERSION_STR, version, WT_METADATA_VERSION, vmajor, vminor, vpatch, key, value));

    /* Flush the stream and rename the file into place. */
    ret = __wt_sync_and_rename(session, &fs, WT_METADATA_TURTLE_SET, WT_METADATA_TURTLE);

/* Close any file handle left open, remove any temporary file. */
err:
    WT_TRET(__wt_fclose(session, &fs));
    WT_TRET(__wt_remove_if_exists(session, WT_METADATA_TURTLE_SET, false));

    /*
     * An error updating the turtle file means something has gone horribly wrong -- we're done.
     */
    if (ret == 0)
        return (ret);
    F_SET(conn, WT_CONN_DATA_CORRUPTION);
    WT_RET_PANIC(session, ret, "%s: fatal turtle file update error", WT_METADATA_TURTLE);
}
