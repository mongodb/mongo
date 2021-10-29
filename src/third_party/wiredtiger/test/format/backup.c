/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "format.h"

/*
 * check_copy --
 *     Confirm the backup worked.
 */
static void
check_copy(void)
{
    WT_CONNECTION *conn;
    WT_DECL_RET;
    WT_SESSION *session;
    size_t len;
    char *path;

    len = WT_MAX(strlen(BACKUP_INFO_FILE), strlen(BACKUP_INFO_FILE_TMP));
    len = WT_MAX(len, strlen("BACKUP"));
    len += strlen(g.home) + 2;
    path = dmalloc(len);
    /*
     * Remove any backup info files that exist. We're about the run recovery in the backup directory
     * so we cannot use the backup directory after that restart. We must remove the files before the
     * restart to avoid a window where they could exist and the backup directory has had recovery
     * run.
     */
    testutil_check(__wt_snprintf(path, len, "%s/%s", g.home, BACKUP_INFO_FILE_TMP));
    ret = unlink(path);
    /* It is fine if the file does not exist. */
    if (ret == 0 || errno == ENOENT)
        ret = 0;
    else
        testutil_die(errno, "unlink %s", path);

    testutil_check(__wt_snprintf(path, len, "%s/%s", g.home, BACKUP_INFO_FILE));
    ret = unlink(path);
    /* It is fine if the file does not exist. */
    if (ret == 0 || errno == ENOENT)
        ret = 0;
    else
        testutil_die(errno, "unlink %s", path);

    /* Now setup and open the path for real. */
    testutil_check(__wt_snprintf(path, len, "%s/BACKUP", g.home));
    wts_open(path, &conn, &session, true);

    /*
     * Verify can return EBUSY if the handle isn't available. Don't yield and retry, in the case of
     * LSM, the handle may not be available for a long time.
     */
    ret = session->verify(session, g.uri, NULL);
    testutil_assertfmt(ret == 0 || ret == EBUSY, "WT_SESSION.verify: %s: %s", path, g.uri);

    wts_close(&conn, &session);

    free(path);
}

/*
 * The set of active files in a backup. This is our "memory" of files that are used in each backup,
 * so we can remove any that are not mentioned in the next backup.
 */
typedef struct {
    char **names;
    uint32_t count;
} ACTIVE_FILES;

/*
 * active_files_init --
 *     Initialize (clear) the active file struct.
 */
static void
active_files_init(ACTIVE_FILES *active)
{
    WT_CLEAR(*active);
}

#if 0
/*
 * active_files_print --
 *     Print the set of active files for debugging.
 */
static void
active_files_print(ACTIVE_FILES *active, const char *msg)
{
    uint32_t i;

    if (active == NULL)
        return;
    fprintf(stderr, "Active files: %s, %d entries\n", msg, (int)active->count);
    for (i = 0; i < active->count; i++)
        fprintf(stderr, "  %s\n", active->names[i]);
}
#endif

/*
 * active_files_add --
 *     Add a new name to the active file list.
 */
static void
active_files_add(ACTIVE_FILES *active, const char *name)
{
    uint32_t pos;

    if (active == NULL)
        return;
    pos = active->count++;
    active->names = drealloc(active->names, sizeof(char *) * active->count);
    active->names[pos] = strdup(name);
}

/*
 * active_files_sort_function --
 *     Sort function for qsort.
 */
static int
active_files_sort_function(const void *left, const void *right)
{
    return (strcmp(*(const char **)left, *(const char **)right));
}

/*
 * active_files_sort --
 *     Sort the list of names in the active file list.
 */
static void
active_files_sort(ACTIVE_FILES *active)
{
    if (active == NULL)
        return;
    __wt_qsort(active->names, active->count, sizeof(char *), active_files_sort_function);
}

/*
 * active_files_remove_missing --
 *     Files in the previous list that are missing from the current list are removed.
 */
static void
active_files_remove_missing(ACTIVE_FILES *prev, ACTIVE_FILES *cur)
{
    uint32_t curpos, prevpos;
    int cmp;
    char filename[1024];

    if (prev == NULL)
        return;
#if 0
    active_files_print(prev, "computing removals: previous list of active files");
    active_files_print(cur, "computing removals: current list of active files");
#endif
    curpos = 0;

    /*
     * Walk through the two lists looking for non-matches.
     */
    for (prevpos = 0; prevpos < prev->count; prevpos++) {
again:
        if (curpos >= cur->count)
            cmp = -1; /* There are extra entries at the end of the prev list */
        else
            cmp = strcmp(prev->names[prevpos], cur->names[curpos]);

        if (cmp == 0)
            curpos++;
        else if (cmp < 0) {
            /*
             * There is something in the prev list not in the current list. Remove it, and continue
             * - don't advance the current list.
             */
            testutil_check(__wt_snprintf(
              filename, sizeof(filename), "%s/BACKUP/%s", g.home, prev->names[prevpos]));
#if 0
            fprintf(stderr, "Removing file from backup: %s\n", filename);
#endif
            error_sys_check(unlink(filename));
            testutil_check(__wt_snprintf(
              filename, sizeof(filename), "%s/BACKUP.copy/%s", g.home, prev->names[prevpos]));
            error_sys_check(unlink(filename));
        } else {
            /*
             * There is something in the current list not in the prev list. Walk past it in the
             * current list and try again.
             */
            curpos++;
            goto again;
        }
    }
}

/*
 * active_files_free --
 *     Free the list of active files.
 */
static void
active_files_free(ACTIVE_FILES *active)
{
    uint32_t i;

    if (active == NULL)
        return;
    for (i = 0; i < active->count; i++)
        free(active->names[i]);
    free(active->names);
    active_files_init(active);
}

/*
 * copy_blocks --
 *     Perform a single block-based incremental backup of the given file.
 */
static void
copy_blocks(WT_SESSION *session, WT_CURSOR *bkup_c, const char *name)
{
    WT_CURSOR *incr_cur;
    WT_DECL_RET;
    size_t len, tmp_sz;
    ssize_t rdsize;
    uint64_t offset, size, this_size, total, type;
    int rfd, wfd1, wfd2;
    char config[512], *tmp;
    bool first_pass;

    tmp_sz = 0;
    tmp = NULL;
    first_pass = true;
    rfd = wfd1 = wfd2 = -1;

    /* Open the duplicate incremental backup cursor with the file name given. */
    testutil_check(__wt_snprintf(config, sizeof(config), "incremental=(file=%s)", name));
    testutil_check(session->open_cursor(session, NULL, bkup_c, config, &incr_cur));
    while ((ret = incr_cur->next(incr_cur)) == 0) {
        testutil_check(incr_cur->get_key(incr_cur, &offset, &size, &type));
        if (type == WT_BACKUP_RANGE) {
            /*
             * Since we are using system calls below instead of a WiredTiger function, we have to
             * prepend the home directory to the file names ourselves.
             */
            if (first_pass) {
                len = strlen(g.home) + strlen(name) + 10;
                tmp = dmalloc(len);
                testutil_check(__wt_snprintf(tmp, len, "%s/%s", g.home, name));
                error_sys_check(rfd = open(tmp, O_RDONLY, 0644));
                free(tmp);
                tmp = NULL;

                len = strlen(g.home) + strlen("BACKUP") + strlen(name) + 10;
                tmp = dmalloc(len);
                testutil_check(__wt_snprintf(tmp, len, "%s/BACKUP/%s", g.home, name));
                error_sys_check(wfd1 = open(tmp, O_WRONLY | O_CREAT, 0644));
                free(tmp);
                tmp = NULL;

                len = strlen(g.home) + strlen("BACKUP.copy") + strlen(name) + 10;
                tmp = dmalloc(len);
                testutil_check(__wt_snprintf(tmp, len, "%s/BACKUP.copy/%s", g.home, name));
                error_sys_check(wfd2 = open(tmp, O_WRONLY | O_CREAT, 0644));
                free(tmp);
                tmp = NULL;

                first_pass = false;
            }
            this_size = WT_MIN(size, BACKUP_MAX_COPY);
            if (tmp_sz < this_size) {
                tmp = drealloc(tmp, this_size);
                tmp_sz = this_size;
            }
            /*
             * Don't use the system checker for lseek. The system check macro uses an int which is
             * often 4 bytes and checks for any negative value. The offset returned from lseek is 8
             * bytes and we can have a false positive error check.
             */
            if (lseek(rfd, (wt_off_t)offset, SEEK_SET) == -1)
                testutil_die(errno, "backup-read: lseek");
            if (lseek(wfd1, (wt_off_t)offset, SEEK_SET) == -1)
                testutil_die(errno, "backup-write1: lseek");
            if (lseek(wfd2, (wt_off_t)offset, SEEK_SET) == -1)
                testutil_die(errno, "backup-write2: lseek");
            total = 0;
            while (total < size) {
                /* Use the read size since we may have read less than the granularity. */
                error_sys_check(rdsize = read(rfd, tmp, this_size));
                /* If we get EOF, we're done. */
                if (rdsize == 0)
                    break;
                error_sys_check(write(wfd1, tmp, (size_t)rdsize));
                error_sys_check(write(wfd2, tmp, (size_t)rdsize));
                total += (uint64_t)rdsize;
                offset += (uint64_t)rdsize;
                this_size = WT_MIN(this_size, size - total);
            }
        } else {
            testutil_assert(type == WT_BACKUP_FILE);
            testutil_assert(first_pass == true);
            testutil_assert(rfd == -1);

            /*
             * These operations are using a WiredTiger function so it will prepend the home
             * directory to the name for us.
             */
            len = strlen("BACKUP") + strlen(name) + 10;
            tmp = dmalloc(len);
            testutil_check(__wt_snprintf(tmp, len, "BACKUP/%s", name));
            testutil_check(__wt_copy_and_sync(session, name, tmp));
            free(tmp);
            tmp = NULL;

            len = strlen("BACKUP.copy") + strlen(name) + 10;
            tmp = dmalloc(len);
            testutil_check(__wt_snprintf(tmp, len, "BACKUP.copy/%s", name));
            testutil_check(__wt_copy_and_sync(session, name, tmp));
            free(tmp);
            tmp = NULL;
        }
    }
    testutil_assert(ret == WT_NOTFOUND);
    testutil_check(incr_cur->close(incr_cur));
    if (rfd != -1) {
        error_sys_check(close(rfd));
        error_sys_check(close(wfd1));
        error_sys_check(close(wfd2));
    }
    free(tmp);
}

#define RESTORE_SKIP 1
#define RESTORE_SUCCESS 0
/*
 * restore_backup_info --
 *     If it exists, restore the backup information. Return 0 on success.
 */
static int
restore_backup_info(WT_SESSION *session, ACTIVE_FILES *active)
{
    FILE *fp;
    WT_CURSOR *cursor;
    WT_DECL_RET;
    size_t len;
    uint64_t id;
    uint32_t i;
    char buf[512], *path;

    testutil_assert(g.c_backup_incr_flag == INCREMENTAL_BLOCK);
    len = strlen(g.home) + strlen(BACKUP_INFO_FILE) + 2;
    path = dmalloc(len);
    testutil_check(__wt_snprintf(path, len, "%s/%s", g.home, BACKUP_INFO_FILE));
    errno = 0;
    ret = RESTORE_SUCCESS;
    if ((fp = fopen(path, "r")) == NULL && errno != ENOENT)
        testutil_die(errno, "restore_backup_info fopen: %s", path);
    free(path);
    if (errno == ENOENT)
        return (RESTORE_SKIP);
    ret = fscanf(fp, "%" SCNu64 "\n", &id);
    if (ret != 1)
        testutil_die(EINVAL, "restore_backup_info ID");
    /*
     * Try to open the backup cursor. We may get ENOENT if the source ID we wrote to the program
     * file was not yet checkpointed. Sometimes it will, sometimes it won't. If we don't find it
     * then return non-zero so that we skip incremental restart testing.
     *
     * NOTE: This call to open a backup cursor to check the source id uses a made up 'this_id' that
     * tries to generate one that cannot possibly be in use. This call can/should be changed if the
     * API ever allows us to open a cursor with a source id that does not require a this id.
     */
    testutil_check(__wt_snprintf(buf, sizeof(buf),
      "incremental=(enabled,src_id=%" PRIu64 ",this_id=%" PRIu64 ")", id, id / 2));
    while ((ret = session->open_cursor(session, "backup:", NULL, buf, &cursor)) == EBUSY)
        __wt_yield();
    if (ret != 0) {
        if (ret == ENOENT) {
            ret = RESTORE_SKIP;
            goto out;
        }
        testutil_die(ret, "session.open_cursor: backup");
    }
    testutil_check(cursor->close(cursor));

    active_files_init(active);
    ret = fscanf(fp, "%" SCNu32 "\n", &active->count);
    /* We could save just an ID if the file count was 0, so return if we find that case. */
    if (ret != 1) {
        ret = RESTORE_SKIP;
        goto out;
    }

    /* Set global id after error paths. */
    g.backup_id = id + 1;
    active->names = drealloc(active->names, sizeof(char *) * active->count);
    for (i = 0; i < active->count; ++i) {
        memset(buf, 0, sizeof(buf));
        ret = fscanf(fp, "%511s\n", buf);
        if (ret != 1) {
            ret = RESTORE_SKIP;
            goto out;
        }
        active->names[i] = strdup(buf);
    }
    ret = RESTORE_SUCCESS;
out:
    fclose_and_clear(&fp);
    return (ret);
}

/*
 * save_backup_info --
 *     Save backup information to a text file format can use to restore on a reopen.
 */
static void
save_backup_info(ACTIVE_FILES *active, uint64_t id)
{
    FILE *fp;
    size_t len;
    uint32_t i;
    char *from_path, *to_path;

    if (g.c_backup_incr_flag != INCREMENTAL_BLOCK)
        return;
    len = strlen(g.home) + strlen(BACKUP_INFO_FILE_TMP) + 2;
    from_path = dmalloc(len);
    testutil_check(__wt_snprintf(from_path, len, "%s/%s", g.home, BACKUP_INFO_FILE_TMP));
    if ((fp = fopen(from_path, "w")) == NULL)
        testutil_die(errno, "save_backup_info fopen: %s", from_path);
    fprintf(fp, "%" PRIu64 "\n", id);
    if (active->count > 0) {
        fprintf(fp, "%" PRIu32 "\n", active->count);
        for (i = 0; i < active->count; ++i)
            fprintf(fp, "%s\n", active->names[i]);
    }
    fclose_and_clear(&fp);
    len = strlen(g.home) + strlen(BACKUP_INFO_FILE) + 2;
    to_path = dmalloc(len);
    testutil_check(__wt_snprintf(to_path, len, "%s/%s", g.home, BACKUP_INFO_FILE));
    error_sys_check(rename(from_path, to_path));
    free(from_path);
    free(to_path);
}

/*
 * backup --
 *     Periodically do a backup and verify it.
 */
WT_THREAD_RET
backup(void *arg)
{
    ACTIVE_FILES active[2], *active_now, *active_prev;
    WT_CONNECTION *conn;
    WT_CURSOR *backup_cursor;
    WT_DECL_RET;
    WT_SESSION *session;
    u_int incremental, period;
    uint64_t src_id, this_id;
    const char *config, *key;
    char cfg[512];
    bool full, incr_full;

    (void)(arg);

    conn = g.wts_conn;
    /* Open a session. */
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    __wt_seconds(NULL, &g.backup_id);
    active_files_init(&active[0]);
    active_files_init(&active[1]);
    active_now = active_prev = NULL;
    incr_full = true;
    incremental = 0;
    /*
     * If we're reopening an existing database and doing incremental backup we reset the initialized
     * variables based on whatever they were at the end of the previous run. We want to make sure
     * that we can take an incremental backup and use the older id as a source identifier. We force
     * that only if the restore function was successful in restoring the backup information.
     */
    if (g.reopen && g.c_backup_incr_flag == INCREMENTAL_BLOCK &&
      restore_backup_info(session, &active[0]) == RESTORE_SUCCESS) {
        incr_full = false;
        full = false;
        incremental = 1;
        active_prev = &active[0];
    }

    /*
     * Perform a full backup at somewhere under 10 seconds (that way there's at least one), then at
     * larger intervals, optionally do incremental backups between full backups.
     */
    this_id = 0;
    for (period = mmrand(NULL, 1, 10);; period = mmrand(NULL, 20, 45)) {
        /* Sleep for short periods so we don't make the run wait. */
        while (period > 0 && !g.workers_finished) {
            --period;
            __wt_sleep(1, 0);
        }

        /*
         * We can't drop named checkpoints while there's a backup in progress, serialize backups
         * with named checkpoints. Wait for the checkpoint to complete, otherwise backups might be
         * starved out.
         */
        lock_writelock(session, &g.backup_lock);
        if (g.workers_finished) {
            lock_writeunlock(session, &g.backup_lock);
            break;
        }

        if (g.c_backup_incr_flag == INCREMENTAL_BLOCK) {
            /*
             * If we're doing a full backup as the start of the incremental backup, only send in an
             * identifier for this one. Also set the block granularity.
             */
            if (incr_full) {
                active_files_free(&active[0]);
                active_files_free(&active[1]);
                active_now = &active[g.backup_id % 2];
                active_prev = NULL;
                testutil_check(__wt_snprintf(cfg, sizeof(cfg),
                  "incremental=(enabled,granularity=%" PRIu32 "K,this_id=%" PRIu64 ")",
                  g.c_backup_incr_granularity, g.backup_id));
                full = true;
                incr_full = false;
            } else {
                if (active_prev == &active[0])
                    active_now = &active[1];
                else
                    active_now = &active[0];
                src_id = g.backup_id - 1;
                /* Use consolidation too. */
                testutil_check(__wt_snprintf(cfg, sizeof(cfg),
                  "incremental=(enabled,consolidate=true,src_id=%" PRIu64 ",this_id=%" PRIu64 ")",
                  src_id, g.backup_id));
                /* Restart a full incremental every once in a while. */
                full = false;
                incr_full = mmrand(NULL, 1, 8) == 1;
            }
            this_id = g.backup_id++;
            config = cfg;
            /* Free up the old active file list we're going to overwrite. */
            active_files_free(active_now);
        } else if (g.c_logging && g.c_backup_incr_flag == INCREMENTAL_LOG) {
            if (incr_full) {
                config = NULL;
                full = true;
                incr_full = false;
            } else {
                testutil_check(__wt_snprintf(cfg, sizeof(cfg), "target=(\"log:\")"));
                config = cfg;
                full = false;
                /* Restart a full incremental every once in a while. */
                incr_full = mmrand(NULL, 1, 8) == 1;
            }
        } else {
            config = NULL;
            full = true;
        }

        /* If we're taking a full backup, create the backup directories. */
        if (full || incremental == 0) {
            testutil_create_backup_directory(g.home);
        }

        /*
         * open_cursor can return EBUSY if concurrent with a metadata operation, retry in that case.
         */
        while (
          (ret = session->open_cursor(session, "backup:", NULL, config, &backup_cursor)) == EBUSY)
            __wt_yield();
        if (ret != 0)
            testutil_die(ret, "session.open_cursor: backup");

        while ((ret = backup_cursor->next(backup_cursor)) == 0) {
            testutil_check(backup_cursor->get_key(backup_cursor, &key));
            if (g.c_backup_incr_flag == INCREMENTAL_BLOCK) {
                if (full)
                    testutil_copy_file(session, key);
                else
                    copy_blocks(session, backup_cursor, key);

            } else
                testutil_copy_file(session, key);
            active_files_add(active_now, key);
        }
        if (ret != WT_NOTFOUND)
            testutil_die(ret, "backup-cursor");

        /* After a log-based incremental backup, truncate the log files. */
        if (g.c_backup_incr_flag == INCREMENTAL_LOG)
            testutil_check(session->truncate(session, "log:", backup_cursor, NULL, NULL));

        testutil_check(backup_cursor->close(backup_cursor));
        lock_writeunlock(session, &g.backup_lock);
        active_files_sort(active_now);
        active_files_remove_missing(active_prev, active_now);
        /* Save the backup information to a file so we can restart on a reopen. */
        save_backup_info(active_now, this_id);
        active_prev = active_now;

        /*
         * If automatic log archival isn't configured, optionally do incremental backups after each
         * full backup. If we're not doing any more incrementals, verify the backup (we can't verify
         * intermediate states, once we perform recovery on the backup database, we can't do any
         * more incremental backups).
         */
        if (full) {
            incremental = 1;
            if (g.c_backup_incr_flag == INCREMENTAL_LOG)
                incremental = g.c_logging_archive ? 1 : mmrand(NULL, 1, 8);
            else if (g.c_backup_incr_flag == INCREMENTAL_BLOCK)
                incremental = mmrand(NULL, 1, 8);
        }
        if (--incremental == 0) {
            check_copy();
            /* We ran recovery in the backup directory, so next time it must be a full backup. */
            incr_full = full = true;
        }
    }

    if (incremental != 0)
        check_copy();

    active_files_free(&active[0]);
    active_files_free(&active[1]);
    testutil_check(session->close(session, NULL));

    return (WT_THREAD_RET_VALUE);
}
