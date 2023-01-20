/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * Structure to hold state for metadata entry worker procedure. It is only used during restore after
 * partial backup.
 */
typedef struct {
    WT_BACKUPHASH *backuphash; /* queue of target URI entries */
    size_t max_len;            /* max key length */

    size_t slot;                 /* next slot */
    size_t allocated;            /* allocated for partial backup keys array */
    char **partial_backup_names; /* partial backup keys array */
} WT_METADATA_FILE_WALK_STATE;

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
    WT_ERR(__wt_buf_fmt(session, buf,
      "key_format=S,value_format=S,id=%d,version=(major=%" PRIu16 ",minor=%" PRIu16 ")",
      WT_METAFILE_ID, WT_BTREE_VERSION_MAX.major, WT_BTREE_VERSION_MAX.minor));
    cfg[1] = buf->data;
    ret = __wt_config_tiered_strip(session, cfg, (const char **)metaconfp);

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
 * __metadata_backup_target_uri_search --
 *     Search in the backup uri hash table if the given uri exists.
 */
static bool
__metadata_backup_target_uri_search(
  WT_SESSION_IMPL *session, WT_BACKUPHASH *backuphash, const char *uri)
{
    WT_BACKUP_TARGET *target_uri;
    WT_CONNECTION_IMPL *conn;
    uint64_t bucket, hash;
    bool found;

    conn = S2C(session);
    found = false;

    hash = __wt_hash_city64(uri, strlen(uri));
    bucket = hash & (conn->hash_size - 1);

    TAILQ_FOREACH (target_uri, &backuphash[bucket], hashq)
        if (strcmp(uri, target_uri->name) == 0) {
            found = true;
            break;
        }
    return (found);
}

/*
 * __wt_read_metadata_file --
 *     Open a text-based metadata file and iterate over the key value pairs calling the worker
 *     function for each of them.
 */
int
__wt_read_metadata_file(WT_SESSION_IMPL *session, const char *file,
  int (*meta_entry_worker_func)(WT_SESSION_IMPL *, WT_ITEM *, WT_ITEM *, void *), void *state,
  bool *file_exist)
{
    WT_DECL_ITEM(key);
    WT_DECL_ITEM(value);
    WT_DECL_RET;
    WT_FSTREAM *fs;

    /* Look for the given filename. If it exists, load it. */
    WT_RET(__wt_fs_exist(session, file, file_exist));
    if (!(*file_exist))
        return (0);
    WT_RET(__wt_fopen(session, file, 0, WT_STREAM_READ, &fs));

    /* Read line pairs and add them to the import list. */
    WT_ERR(__wt_scr_alloc(session, 1024, &key));
    WT_ERR(__wt_scr_alloc(session, 1024, &value));
    for (;;) {
        WT_ERR(__wt_getline(session, fs, key));
        if (key->size == 0)
            break;
        WT_ERR(__wt_getline(session, fs, value));
        if (value->size == 0)
            WT_ERR_PANIC(session, EINVAL, "%s: zero-length value", file);

        WT_ERR(meta_entry_worker_func(session, key, value, state));
    }

err:
    __wt_scr_free(session, &key);
    __wt_scr_free(session, &value);
    WT_TRET(__wt_fclose(session, &fs));

    return (ret);
}

/*
 * __metadata_entry_worker --
 *     Worker function for metadata file reader procedure. The function populates the partial backup
 *     names array and updates the metadata of the database with the new entries read from the file.
 */
static int
__metadata_entry_worker(WT_SESSION_IMPL *session, WT_ITEM *key, WT_ITEM *value, void *state)
{
    WT_CONNECTION_IMPL *conn;
    WT_METADATA_FILE_WALK_STATE *meta_state;
    char *metadata_key, **p;

    metadata_key = NULL;
    p = NULL;
    conn = S2C(session);
    meta_state = (WT_METADATA_FILE_WALK_STATE *)state;

    /*
     * When performing partial backup restore, generate a list of tables that are not part of the
     * target uri list so that we can drop all entries later. To do this, parse through all the
     * table metadata entries and check if the metadata entry exists in the target uri hash table.
     * If the metadata entry doesn't exist in the hash table, append the table name to the partial
     * backup remove list.
     */
    metadata_key = (char *)key->data;
    if (F_ISSET(conn, WT_CONN_BACKUP_PARTIAL_RESTORE) && WT_PREFIX_MATCH(metadata_key, "table:")) {
        /* Assert that there should be no WiredTiger tables with a table format. */
        WT_ASSERT(session, __wt_name_check(session, (const char *)key->data, key->size, true) == 0);
        /*
         * The target uri will be the deciding factor if a specific metadata table entry needs to be
         * dropped. If the metadata table entry does not exist in the target uri hash table, append
         * the metadata key to the backup remove list.
         */
        if (__metadata_backup_target_uri_search(session, meta_state->backuphash, metadata_key) ==
          false) {
            if (key->size > meta_state->max_len)
                meta_state->max_len = key->size;

            WT_RET(__wt_realloc_def(session, &meta_state->allocated, meta_state->slot + 2,
              &meta_state->partial_backup_names));
            p = &meta_state->partial_backup_names[meta_state->slot];
            p[0] = p[1] = NULL;

            WT_RET(__wt_strndup(session, (char *)key->data, key->size,
              &meta_state->partial_backup_names[meta_state->slot]));
            meta_state->slot++;
        }
    }

    /*
     * In the case of partial backup restore, add the entry to the metadata even if the table entry
     * doesn't exist so that we can correctly drop all related entries via the schema code later.
     */
    WT_RET(__wt_metadata_update(session, key->data, value->data));

    return (0);
}

/*
 * __metadata_load_hot_backup --
 *     Load the contents of any hot backup file.
 */
static int
__metadata_load_hot_backup(WT_SESSION_IMPL *session, WT_BACKUPHASH *backuphash)
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_METADATA_FILE_WALK_STATE meta_state;
    size_t file_len, i;
    char *filename, *metadata_conf, *tablename;
    const char *drop_cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_drop), "remove_files=false", NULL};
    bool exist;

    file_len = 0;
    conn = S2C(session);
    filename = metadata_conf = tablename = NULL;
    exist = false;

    WT_CLEAR(meta_state);
    meta_state.backuphash = backuphash;

    /* Open the metadata backup file and iterate over the key value pairs. */
    WT_ERR(__wt_read_metadata_file(
      session, WT_METADATA_BACKUP, __metadata_entry_worker, &meta_state, &exist));
    if (!exist)
        goto err;

    F_SET(conn, WT_CONN_WAS_BACKUP);
    if (F_ISSET(conn, WT_CONN_BACKUP_PARTIAL_RESTORE) && meta_state.partial_backup_names != NULL) {
        WT_ERR(__wt_calloc_def(session, meta_state.slot + 1, &conn->partial_backup_remove_ids));
        file_len = strlen("file:") + meta_state.max_len + strlen(".wt") + 1;
        WT_ERR(__wt_calloc_def(session, file_len, &filename));
        /*
         * Parse through the partial backup list and attempt to clean up all metadata references
         * relating to the file. To do so, perform a schema drop operation on the table to cleanly
         * remove all linked references. At the same time generate a list of btree ids to be used in
         * recovery to truncate all the history store records.
         */
        for (i = 0; i < meta_state.slot; ++i) {
            tablename = meta_state.partial_backup_names[i];
            WT_PREFIX_SKIP_REQUIRED(session, tablename, "table:");
            WT_ERR(__wt_snprintf(filename, file_len, "file:%s.wt", tablename));
            WT_ERR(__wt_metadata_search(session, filename, &metadata_conf));
            WT_ERR(__wt_config_getones(session, metadata_conf, "id", &cval));
            conn->partial_backup_remove_ids[i] = (uint32_t)cval.val;

            WT_WITH_SCHEMA_LOCK(session,
              WT_WITH_TABLE_WRITE_LOCK(session,
                ret = __wt_schema_drop(session, meta_state.partial_backup_names[i], drop_cfg)));
            WT_ERR(ret);
            __wt_free(session, metadata_conf);
        }
    }

err:
    __wt_free(session, metadata_conf);
    __wt_free(session, filename);

    /*
     * Free the partial backup names list. The backup id list is used in recovery to truncate the
     * history store entries that do not exist as part of the database anymore.
     */
    if (meta_state.partial_backup_names != NULL) {
        for (i = 0; i < meta_state.slot; ++i)
            __wt_free(session, meta_state.partial_backup_names[i]);
        __wt_free(session, meta_state.partial_backup_names);
    }

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
    WT_VERSION version;
    char *version_string;

    version = WT_NO_VERSION;

    WT_WITH_TURTLE_LOCK(
      session, ret = __wt_turtle_read(session, WT_METADATA_VERSION, &version_string));

    if (ret != 0)
        WT_ERR_MSG(session, ret, "Unable to read version string from turtle file");

    if ((ret = sscanf(version_string, "major=%" SCNu16 ",minor=%" SCNu16 ",patch=%" SCNu16,
           &version.major, &version.minor, &version.patch)) != 3)
        WT_ERR_MSG(session, ret, "Unable to parse turtle file version string");

    ret = 0;

    if (__wt_version_lt(version, WT_MIN_STARTUP_VERSION))
        WT_ERR_MSG(session, WT_ERROR, "WiredTiger version incompatible with current binary");

    S2C(session)->recovery_version = version;

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
    __wt_verbose_notice(session, WT_VERB_METADATA, "%s not found, %s renamed to %s",
      WT_METADATA_TURTLE, WT_METADATA_TURTLE_SET, WT_METADATA_TURTLE);
    *existp = true;
    return (0);
}

/*
 * __metadata_add_backup_target_uri --
 *     Add the target uri to the backup uri hash table.
 */
static int
__metadata_add_backup_target_uri(
  WT_SESSION_IMPL *session, WT_BACKUPHASH *backuphash, const char *name, size_t len)
{
    WT_BACKUP_TARGET *new_target_uri;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    uint64_t bucket, hash;

    conn = S2C(session);

    WT_ERR(__wt_calloc_one(session, &new_target_uri));
    WT_ERR(__wt_strndup(session, name, len, &new_target_uri->name));

    hash = __wt_hash_city64(name, len);
    bucket = hash & (conn->hash_size - 1);
    new_target_uri->name_hash = hash;
    /* Insert target uri entry into hashtable. */
    TAILQ_INSERT_HEAD(&backuphash[bucket], new_target_uri, hashq);

    return (0);
err:
    if (new_target_uri != NULL)
        __wt_free(session, new_target_uri->name);
    __wt_free(session, new_target_uri);

    return (ret);
}

/*
 * __metadata_load_target_uri_list --
 *     Load the list of target uris and construct a hashtable from it.
 */
static int
__metadata_load_target_uri_list(
  WT_SESSION_IMPL *session, bool exist_backup, const char *cfg[], WT_BACKUPHASH *backuphash)
{
    WT_CONFIG backup_config;
    WT_CONFIG_ITEM cval, k, v;
    WT_DECL_RET;

    WT_TRET(__wt_config_gets(session, cfg, "backup_restore_target", &cval));
    if (cval.len != 0) {
        if (!exist_backup)
            WT_RET_MSG(session, EINVAL,
              "restoring a partial backup requires the WiredTiger metadata backup file.");
        F_SET(S2C(session), WT_CONN_BACKUP_PARTIAL_RESTORE);

        /*
         * Check that the configuration string only has table schema formats in the target list and
         * construct the target hash table.
         */
        __wt_config_subinit(session, &backup_config, &cval);
        while ((ret = __wt_config_next(&backup_config, &k, &v)) == 0) {
            if (!WT_PREFIX_MATCH(k.str, "table:"))
                WT_RET_MSG(session, EINVAL,
                  "partial backup restore only supports objects of type \"table\" formats in the "
                  "target uri list, found %.*s instead.",
                  (int)k.len, k.str);
            WT_RET(__metadata_add_backup_target_uri(session, backuphash, (char *)k.str, k.len));
        }
        WT_RET_NOTFOUND_OK(ret);
    }
    return (0);
}

/*
 * __wt_turtle_init --
 *     Check the turtle file and create if necessary.
 */
int
__wt_turtle_init(WT_SESSION_IMPL *session, bool verify_meta, const char *cfg[])
{
    WT_BACKUPHASH *backuphash;
    WT_BACKUP_TARGET *target_uri;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    uint64_t i;
    char *metaconf, *unused_value;
    bool exist_backup, exist_incr, exist_isrc, exist_turtle;
    bool load, load_turtle, validate_turtle;

    conn = S2C(session);
    load = load_turtle = validate_turtle = false;
    /* Initialize target uri hashtable. */
    WT_ERR(__wt_calloc_def(session, conn->hash_size, &backuphash));
    for (i = 0; i < conn->hash_size; ++i)
        TAILQ_INIT(&backuphash[i]);

    /*
     * Discard any turtle setup file left-over from previous runs. This doesn't matter for
     * correctness, it's just cleaning up random files.
     */

    if ((ret = __wt_remove_if_exists(session, WT_METADATA_TURTLE_SET, false)) != 0) {
        /* If we're a readonly database, we can skip discarding the leftover file. */
        if (ret == EACCES)
            ret = 0;
        WT_ERR(ret);
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
    WT_ERR(__wt_fs_exist(session, WT_LOGINCR_BACKUP, &exist_incr));
    WT_ERR(__wt_fs_exist(session, WT_LOGINCR_SRC, &exist_isrc));
    WT_ERR(__wt_fs_exist(session, WT_METADATA_BACKUP, &exist_backup));
    WT_ERR(__wt_fs_exist(session, WT_METADATA_TURTLE, &exist_turtle));

    if (exist_turtle) {
        /*
         * Failure to read means a bad turtle file. Remove it and create a new turtle file.
         */
        if (F_ISSET(conn, WT_CONN_SALVAGE)) {
            WT_WITH_TURTLE_LOCK(
              session, ret = __wt_turtle_read(session, WT_METAFILE_URI, &unused_value));
            __wt_free(session, unused_value);
        }

        if (ret != 0) {
            WT_ERR(__wt_remove_if_exists(session, WT_METADATA_TURTLE, false));
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
            WT_ERR_MSG(session, EINVAL, "Incremental backup after running recovery is not allowed");
        /*
         * If we have a backup file and metadata and turtle files, we want to recreate the metadata
         * from the backup.
         */
        if (exist_backup) {
            __wt_verbose_notice(session, WT_VERB_METADATA,
              "Both %s and %s exist; recreating metadata from backup", WT_METADATA_TURTLE,
              WT_METADATA_BACKUP);
            WT_ERR(__wt_remove_if_exists(session, WT_METAFILE, false));
            WT_ERR(__wt_remove_if_exists(session, WT_METADATA_TURTLE, false));
            load = true;
        } else if (validate_turtle)
            WT_ERR(__wt_turtle_validate_version(session));
    } else
        load = true;
    if (load) {
        if (exist_incr)
            F_SET(conn, WT_CONN_WAS_BACKUP);

        /*
         * Verifying the metadata is incompatible with restarting from a backup because the verify
         * call will rewrite the metadata's checkpoint and could lead to skipping recovery. Test
         * here before creating the metadata file and reading in the backup file.
         */
        if (verify_meta && exist_backup)
            WT_ERR_MSG(
              session, EINVAL, "restoring a backup is incompatible with metadata verification");
        /* If partial backup target is non-empty, construct the target backup uri list. */
        WT_ERR(__metadata_load_target_uri_list(session, exist_backup, cfg, backuphash));

        /* Create the metadata file. */
        WT_ERR(__metadata_init(session));

        /* Load any hot-backup information. */
        WT_ERR(__metadata_load_hot_backup(session, backuphash));

        /* Create any bulk-loaded file stubs. */
        WT_ERR(__metadata_load_bulk(session));
    }

    if (load || load_turtle) {
        /* Create the turtle file. */
        WT_ERR(__metadata_config(session, &metaconf));
        WT_WITH_TURTLE_LOCK(session, ret = __wt_turtle_update(session, WT_METAFILE_URI, metaconf));
        __wt_free(session, metaconf);
        WT_ERR(ret);
    }

err:
    for (i = 0; i < conn->hash_size; ++i)
        while (!TAILQ_EMPTY(&backuphash[i])) {
            target_uri = TAILQ_FIRST(&backuphash[i]);
            /* Remove target uri entry from the hashtable. */
            TAILQ_REMOVE(&backuphash[i], target_uri, hashq);
            __wt_free(session, target_uri->name);
            __wt_free(session, target_uri);
        }
    __wt_free(session, backuphash);

    /*
     * We used to remove the backup file here. But we cannot do that until the metadata is fully
     * synced to disk after recovery.
     */
    return (ret);
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
          "major=%" PRIu16 ",minor=%" PRIu16 "\n",
          WT_METADATA_COMPAT, conn->compat_version.major, conn->compat_version.minor));

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
