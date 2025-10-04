/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *  All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"
#include "live_restore_private.h"

/*
 * __wti_live_restore_migration_complete --
 *     Return if live restore is past the background migration stage.
 */
bool
__wti_live_restore_migration_complete(WT_SESSION_IMPL *session)
{
    WTI_LIVE_RESTORE_FS *lr_fs = (WTI_LIVE_RESTORE_FS *)S2C(session)->file_system;
    WTI_LIVE_RESTORE_STATE state = __wti_live_restore_get_state(session, lr_fs);

    return (state == WTI_LIVE_RESTORE_STATE_CLEAN_UP || state == WTI_LIVE_RESTORE_STATE_COMPLETE);
}

/*
 * __wt_live_restore_migration_in_progress --
 *     Return if live restore is in progress stage.
 */
bool
__wt_live_restore_migration_in_progress(WT_SESSION_IMPL *session)
{
    /* If live restore is not enabled then background migration is by definition not in progress. */
    if (!F_ISSET(S2C(session), WT_CONN_LIVE_RESTORE_FS))
        return (false);
    WTI_LIVE_RESTORE_FS *lr_fs = (WTI_LIVE_RESTORE_FS *)S2C(session)->file_system;
    WTI_LIVE_RESTORE_STATE state = __wti_live_restore_get_state(session, lr_fs);

    return (state == WTI_LIVE_RESTORE_STATE_BACKGROUND_MIGRATION);
}

/*
 * __live_restore_state_to_string --
 *     Convert a live restore state to its string representation.
 */
static const char *
__live_restore_state_to_string(WTI_LIVE_RESTORE_STATE state)
{
    switch (state) {
    case WTI_LIVE_RESTORE_STATE_NONE:
        return ("WTI_LIVE_RESTORE_STATE_NONE");
    case WTI_LIVE_RESTORE_STATE_BACKGROUND_MIGRATION:
        return ("WTI_LIVE_RESTORE_STATE_BACKGROUND_MIGRATION");
    case WTI_LIVE_RESTORE_STATE_CLEAN_UP:
        return ("WTI_LIVE_RESTORE_STATE_CLEAN_UP");
    case WTI_LIVE_RESTORE_STATE_COMPLETE:
        return ("WTI_LIVE_RESTORE_STATE_COMPLETE");
    }

    return (0);
}

/*
 * __live_restore_state_from_string --
 *     Convert a string to its live restore state.
 */
static int
__live_restore_state_from_string(
  WT_SESSION_IMPL *session, char *state_str, WTI_LIVE_RESTORE_STATE *statep)
{
    size_t str_len = __wt_strnlen(state_str, WT_LIVE_RESTORE_STATE_STRING_MAX);

    if (WT_STRING_LIT_MATCH("WTI_LIVE_RESTORE_STATE_NONE", state_str, str_len))
        *statep = WTI_LIVE_RESTORE_STATE_NONE;
    else if (WT_STRING_LIT_MATCH("WTI_LIVE_RESTORE_STATE_BACKGROUND_MIGRATION", state_str, str_len))
        *statep = WTI_LIVE_RESTORE_STATE_BACKGROUND_MIGRATION;
    else if (WT_STRING_LIT_MATCH("WTI_LIVE_RESTORE_STATE_CLEAN_UP", state_str, str_len))
        *statep = WTI_LIVE_RESTORE_STATE_CLEAN_UP;
    else if (WT_STRING_LIT_MATCH("WTI_LIVE_RESTORE_STATE_COMPLETE", state_str, str_len))
        *statep = WTI_LIVE_RESTORE_STATE_COMPLETE;
    else
        WT_RET_MSG(session, EINVAL, "Invalid state string: '%s' ", state_str);

    return (0);
}

/*
 * __live_restore_get_state_from_file --
 *     Read the live restore state from the turtle file. If it doesn't exist we return NONE. In live
 *     restore mode the caller must already hold the live restore state lock. This function takes a
 *     non-live restore file system as it can be called in non-live restore modes.
 */
static int
__live_restore_get_state_from_file(
  WT_SESSION_IMPL *session, WT_FILE_SYSTEM *fs, WTI_LIVE_RESTORE_STATE *statep)
{
    WT_DECL_RET;

    /*
     * In live restore mode check we hold the state lock. For non-live restore file systems we don't
     * need this lock as we'll never modify the state.
     */
    WT_CONNECTION_IMPL *conn = S2C(session);
    if (F_ISSET(conn, WT_CONN_LIVE_RESTORE_FS)) {
        WTI_LIVE_RESTORE_FS *lr_fs = (WTI_LIVE_RESTORE_FS *)conn->file_system;
        WT_ASSERT_ALWAYS(session, __wt_spin_owned(session, &lr_fs->state_lock),
          "Live restore state lock not held!");
    }

    bool turtle_in_dest = false;
    WTI_LIVE_RESTORE_STATE state_from_file = WTI_LIVE_RESTORE_STATE_NONE;
    char *lr_metadata = NULL;

    WT_DECL_ITEM(dest_turt_path);
    WT_RET(__wt_scr_alloc(session, 0, &dest_turt_path));
    WT_ERR(__wt_filename_construct(
      session, conn->home, WT_METADATA_TURTLE, UINTMAX_MAX, UINT32_MAX, dest_turt_path));

    WT_ERR(
      fs->fs_exist(fs, (WT_SESSION *)session, (char *)(dest_turt_path)->data, &turtle_in_dest));

    /*
     * If the turtle file isn't present in the destination we've already default initialized the
     * state to NONE.
     */
    if (turtle_in_dest) {
        WT_ERR_NOTFOUND_OK(
          __wt_metadata_search(session, WT_METADATA_LIVE_RESTORE, &lr_metadata), true);
        if (ret == WT_NOTFOUND) {
            state_from_file = WTI_LIVE_RESTORE_STATE_NONE;
            ret = 0;
        } else {
            /*
             * The scan constant needs to be hardcoded, assert if someone changes the underlying
             * constant.
             */
            WT_ASSERT(session, WT_LIVE_RESTORE_STATE_STRING_MAX == 128);
            char lr_metadata_string[WT_LIVE_RESTORE_STATE_STRING_MAX];
            if ((sscanf(lr_metadata, "state=%127s", lr_metadata_string)) != 1)
                WT_ASSERT_ALWAYS(
                  session, false, "failed to parse live restore metadata from the turtle file!");

            WT_ERR(__live_restore_state_from_string(session, lr_metadata_string, &state_from_file));
        }
    }

    *statep = state_from_file;

err:
    __wt_free(session, lr_metadata);
    __wt_scr_free(session, &dest_turt_path);

    return (ret);
}

/*
 * __live_restore_report_state_to_application --
 *     WiredTiger reports a simplified live restore state to the application which lets it know it
 *     can restart on completion of live restore.
 */
static void
__live_restore_report_state_to_application(WT_SESSION_IMPL *session, WTI_LIVE_RESTORE_STATE state)
{
    switch (state) {
    case WTI_LIVE_RESTORE_STATE_NONE:
        WT_STAT_CONN_SET(session, live_restore_state, WT_LIVE_RESTORE_INIT);
        break;
    case WTI_LIVE_RESTORE_STATE_BACKGROUND_MIGRATION:
    case WTI_LIVE_RESTORE_STATE_CLEAN_UP:
        WT_STAT_CONN_SET(session, live_restore_state, WT_LIVE_RESTORE_IN_PROGRESS);
        break;
    case WTI_LIVE_RESTORE_STATE_COMPLETE:
        WT_STAT_CONN_SET(session, live_restore_state, WT_LIVE_RESTORE_COMPLETE);
        break;
    }
}

/*
 * __live_restore_set_state_int --
 *     Internal function for setting the live restore state. The caller must hold the state lock.
 */
static int
__live_restore_set_state_int(
  WT_SESSION_IMPL *session, WTI_LIVE_RESTORE_FS *lr_fs, WTI_LIVE_RESTORE_STATE new_state)
{
    WT_ASSERT_SPINLOCK_OWNED(session, &lr_fs->state_lock);
    /*
     * Validity checking. There is a defined transition of states and we should never skip or repeat
     * a state.
     */
    switch (new_state) {
    case WTI_LIVE_RESTORE_STATE_NONE:
        /*  We should never transition to NONE. This is a placeholder when state is not set. */
        WT_ASSERT_ALWAYS(session, false, "Attempting to set Live Restore state to NONE!");
        break;
    case WTI_LIVE_RESTORE_STATE_BACKGROUND_MIGRATION:
        WT_ASSERT(session, lr_fs->state == WTI_LIVE_RESTORE_STATE_NONE);
        break;
    case WTI_LIVE_RESTORE_STATE_CLEAN_UP:
        WT_ASSERT(session, lr_fs->state == WTI_LIVE_RESTORE_STATE_BACKGROUND_MIGRATION);
        break;
    case WTI_LIVE_RESTORE_STATE_COMPLETE:
        WT_ASSERT(session, lr_fs->state == WTI_LIVE_RESTORE_STATE_CLEAN_UP);
        break;
    }

    lr_fs->state = new_state;
    /* Bumping the turtle file will automatically write the latest live restore state. */
    WT_RET(__wt_live_restore_turtle_rewrite(session));
    __live_restore_report_state_to_application(session, new_state);
    return (0);
}

/*
 * __wti_live_restore_set_state --
 *     Update the live restore state in memory and persist it to the turtle file.
 */
int
__wti_live_restore_set_state(
  WT_SESSION_IMPL *session, WTI_LIVE_RESTORE_FS *lr_fs, WTI_LIVE_RESTORE_STATE new_state)
{
    WT_DECL_RET;
    WTI_WITH_LIVE_RESTORE_STATE_LOCK(
      session, lr_fs, ret = __live_restore_set_state_int(session, lr_fs, new_state));
    return (ret);
}

/*
 * __live_restore_init_state_int --
 *     Internal function for initializing the live restore state, expects the state lock to be held.
 */
static int
__live_restore_init_state_int(WT_SESSION_IMPL *session, WTI_LIVE_RESTORE_FS *lr_fs)
{
    WT_ASSERT_SPINLOCK_OWNED(session, &lr_fs->state_lock);
    WT_ASSERT_ALWAYS(session, lr_fs->state == WTI_LIVE_RESTORE_STATE_NONE,
      "Attempting to initialize already initialized state!");

    WTI_LIVE_RESTORE_STATE state;
    WT_RET(__live_restore_get_state_from_file(session, lr_fs->os_file_system, &state));

    if (state != WTI_LIVE_RESTORE_STATE_NONE)
        lr_fs->state = state;
    else
        /*
         * Only set the in memory state, don't write it to the turtle file. Creating the turtle file
         * here means WiredTiger will expect to find a metadata file and panic when it can't find
         * it. Since background migration is the first legal live restore state we'll always come
         * back to this state on a restart if we didn't persist state in the turtle file.
         */
        lr_fs->state = WTI_LIVE_RESTORE_STATE_BACKGROUND_MIGRATION;
    return (0);
}

/*
 * __wti_live_restore_init_state --
 *     Initialize the live restore state. Read the state from file if it exists, otherwise we start
 *     in the log copy state and need to create the file on disk.
 */
int
__wti_live_restore_init_state(WT_SESSION_IMPL *session, WTI_LIVE_RESTORE_FS *lr_fs)
{
    WT_DECL_RET;
    WTI_WITH_LIVE_RESTORE_STATE_LOCK(
      session, lr_fs, ret = __live_restore_init_state_int(session, lr_fs));
    return (ret);
}

/*
 * __wt_live_restore_get_state_string --
 *     Get the live restore state in string form.
 */
int
__wt_live_restore_get_state_string(WT_SESSION_IMPL *session, WT_ITEM *lr_state_str)
{
    WT_CONNECTION_IMPL *conn = S2C(session);
    WT_ASSERT_ALWAYS(session, F_ISSET(conn, WT_CONN_LIVE_RESTORE_FS),
      "Can't fetch state string when live restore is not enabled!");

    WTI_LIVE_RESTORE_FS *lr_fs = (WTI_LIVE_RESTORE_FS *)conn->file_system;
    WTI_LIVE_RESTORE_STATE state = __wti_live_restore_get_state(session, lr_fs);

    WT_RET(__wt_buf_fmt(session, lr_state_str, "%s", __live_restore_state_to_string(state)));
    return (0);
}

/*
 * __wt_live_restore_turtle_update --
 *     Intercept updates to the turtle file so we can take the state lock first. The state lock must
 *     be held for the entire process and taken before we take the turtle lock.
 */
int
__wt_live_restore_turtle_update(
  WT_SESSION_IMPL *session, const char *key, const char *value, bool take_turtle_lock)
{
    WT_DECL_RET;

    WTI_LIVE_RESTORE_FS *lr_fs = (WTI_LIVE_RESTORE_FS *)S2C(session)->file_system;
    /* clang-format off */
    WTI_WITH_LIVE_RESTORE_STATE_LOCK(session, lr_fs,
        if (take_turtle_lock)
            WT_WITH_TURTLE_LOCK(session, ret = __wt_turtle_update(session, key, value));
        else
            ret = __wt_turtle_update(session, key, value);
    );
    /* clang-format on */
    return (ret);
}

/*
 * __wt_live_restore_turtle_rewrite --
 *     Intercept calls to rewrite the turtle file so we can take the state lock first. The state
 *     lock must be held for the entire process and taken before we take the turtle lock.
 */
int
__wt_live_restore_turtle_rewrite(WT_SESSION_IMPL *session)
{
    WT_DECL_RET;
    WTI_LIVE_RESTORE_FS *lr_fs = (WTI_LIVE_RESTORE_FS *)S2C(session)->file_system;
    WTI_WITH_LIVE_RESTORE_STATE_LOCK(
      session, lr_fs, WT_WITH_TURTLE_LOCK(session, ret = __wt_metadata_turtle_rewrite(session)));
    return (ret);
}

/*
 * __wt_live_restore_turtle_read --
 *     Intercept calls to read the turtle file so we can take the state lock first. The state lock
 *     must be held for the entire process and taken before we take the turtle lock.
 */
int
__wt_live_restore_turtle_read(WT_SESSION_IMPL *session, const char *key, char **valuep)
{
    WT_DECL_RET;
    WTI_LIVE_RESTORE_FS *lr_fs = (WTI_LIVE_RESTORE_FS *)S2C(session)->file_system;
    WTI_WITH_LIVE_RESTORE_STATE_LOCK(
      session, lr_fs, WT_WITH_TURTLE_LOCK(session, ret = __wt_turtle_read(session, key, valuep)));
    return (ret);
}

/*
 * __wti_live_restore_get_state --
 *     Get the live restore state. Take the state lock if it isn't already held.
 */
WTI_LIVE_RESTORE_STATE
__wti_live_restore_get_state(WT_SESSION_IMPL *session, WTI_LIVE_RESTORE_FS *lr_fs)
{
    WTI_LIVE_RESTORE_STATE state = WTI_LIVE_RESTORE_STATE_NONE;
    WTI_WITH_LIVE_RESTORE_STATE_LOCK(session, lr_fs, state = lr_fs->state);
    /* We initialize state on startup. This shouldn't be possible. */
    WT_ASSERT_ALWAYS(session, state != WTI_LIVE_RESTORE_STATE_NONE, "State not initialized!");
    return (state);
}

/*
 * __wti_live_restore_validate_directories --
 *     Validate the source and destination directories are in the correct state on startup.
 */
int
__wti_live_restore_validate_directories(WT_SESSION_IMPL *session, WTI_LIVE_RESTORE_FS *lr_fs)
{
    WT_DECL_RET;

    char **dirlist_source = NULL, **dirlist_dest = NULL;
    uint32_t num_source_files = 0, num_dest_files = 0;
    WTI_LIVE_RESTORE_STATE state_from_file;
    bool contain_backup_file = false;

    /*
     * First check that the source doesn't contain any live restore stop files, but does contain a
     * backup file.
     */
    WT_ERR(lr_fs->os_file_system->fs_directory_list(lr_fs->os_file_system, (WT_SESSION *)session,
      lr_fs->source.home, "", &dirlist_source, &num_source_files));

    if (num_source_files == 0)
        WT_ERR_MSG(session, EINVAL, "Source directory is empty. Nothing to restore!");

    for (uint32_t i = 0; i < num_source_files; ++i) {
        if (WT_SUFFIX_MATCH(dirlist_source[i], WTI_LIVE_RESTORE_STOP_FILE_SUFFIX))
            WT_ERR_MSG(session, EINVAL,
              "Source directory contains live restore stop file: %s. This implies it is a "
              "destination directory that hasn't finished restoration",
              dirlist_source[i]);

        if (WT_SUFFIX_MATCH(dirlist_source[i], WT_METADATA_BACKUP))
            contain_backup_file = true;
    }

    /*
     * We rely on the backup process to clean the metadata file in the source and remove instances
     * of nbits=-1. If we don't live restore could see this nbits=-1, think it applies to the file
     * in the destination, and never copy across the file causing data loss.
     */
    if (!contain_backup_file)
        WT_ERR_MSG(session, EINVAL, "Source directory is not a valid backup directory");

    /* Now check the destination folder */

    /* Read state directly from the turtle file in the destination. */
    WT_ERR(__live_restore_get_state_from_file(session, lr_fs->os_file_system, &state_from_file));

    WT_ERR(lr_fs->os_file_system->fs_directory_list(lr_fs->os_file_system, (WT_SESSION *)session,
      lr_fs->destination.home, "", &dirlist_dest, &num_dest_files));

    switch (state_from_file) {
    case WTI_LIVE_RESTORE_STATE_NONE:
        /*
         * Ideally we'd prevent live restore from starting when there are any files already present
         * in the destination, but we can't control for everything that the user might put into the
         * folder. Instead only check for WiredTiger files.
         */
        for (uint32_t i = 0; i < num_dest_files; ++i) {
            if (WT_PREFIX_MATCH(dirlist_dest[i], WT_WIREDTIGER) ||
              WT_SUFFIX_MATCH(dirlist_dest[i], ".wt"))
                /*
                 * This error is thrown for two reasons:
                 *
                 * 1) A live restore is attempted on a destination that already contains data. In
                 * this scenario we prevent unintentionally corrupting whatever data is already
                 * present in the destination.
                 *
                 * 2) When live restore starts there is a brief period where the live restore state
                 * is set in memory but not yet persisted to the turtle file. During this period
                 * WiredTiger files such as WiredTiger.lock are created in the destination, so if we
                 * crash and restart we'll see the live restore state is NONE, detect these files,
                 * and assume we're trying to overwrite a valid destination. This crash window is
                 * very short and difficult to recover from programmatically. Instead we expect the
                 * user to delete these orphan files and restart the live restore. The crash happens
                 * very early in startup so there's no chance that the user has written data that
                 * could be lost.
                 */
                WT_ERR_MSG(session, EINVAL,
                  "Attempting to begin a live restore on a directory that already contains "
                  "WiredTiger files '%s'! It's possible this file will be overwritten.",
                  dirlist_dest[i]);
        }
        break;
    case WTI_LIVE_RESTORE_STATE_BACKGROUND_MIGRATION:
    case WTI_LIVE_RESTORE_STATE_CLEAN_UP:
        /* There's no invalid state to check in these cases. */
        break;
    case WTI_LIVE_RESTORE_STATE_COMPLETE:
        for (uint32_t i = 0; i < num_dest_files; ++i) {
            if (WT_SUFFIX_MATCH(dirlist_dest[i], WTI_LIVE_RESTORE_STOP_FILE_SUFFIX))
                WT_ERR_MSG(session, EINVAL,
                  "Live restore is complete but live restore stop file '%s' still exists!",
                  dirlist_dest[i]);
        }
        break;
    }

err:
    WT_TRET(lr_fs->os_file_system->fs_directory_list_free(
      lr_fs->os_file_system, (WT_SESSION *)session, dirlist_source, num_source_files));

    WT_TRET(lr_fs->os_file_system->fs_directory_list_free(
      lr_fs->os_file_system, (WT_SESSION *)session, dirlist_dest, num_dest_files));

    return (ret);
}

/*
 * __wt_live_restore_validate_non_lr_system --
 *     When starting in non-live restore mode make sure we're not in the middle of a live restore.
 */
int
__wt_live_restore_validate_non_lr_system(WT_SESSION_IMPL *session)
{
    /* The turtle file should indicate we're not in the middle of a live restore. */
    WTI_LIVE_RESTORE_STATE state;
    WT_RET(__live_restore_get_state_from_file(session, S2C(session)->file_system, &state));
    if (state != WTI_LIVE_RESTORE_STATE_NONE && state != WTI_LIVE_RESTORE_STATE_COMPLETE)
        WT_RET_MSG(session, EINVAL,
          "Cannot start in non-live restore mode while a live restore is in progress!");

    return (0);
}

/*
 * __wt_live_restore_init_stats --
 *     Initialize the live restore stats.
 */
void
__wt_live_restore_init_stats(WT_SESSION_IMPL *session)
{
    if (F_ISSET(S2C(session), WT_CONN_LIVE_RESTORE_FS)) {
        /*
         * The live restore external state is known on initialization, but at that time the stat
         * server hasn't begun so we can't actually set the state. This must be called after the
         * stat server starts.
         */
        WTI_LIVE_RESTORE_FS *lr_fs = ((WTI_LIVE_RESTORE_FS *)S2C(session)->file_system);
        WTI_LIVE_RESTORE_STATE state = __wti_live_restore_get_state(session, lr_fs);
        __live_restore_report_state_to_application(session, state);
    }
}
