/*-
 * Public Domain 2014-2020 MongoDB, Inc.
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

    len = strlen(g.home) + strlen("BACKUP") + 2;
    path = dmalloc(len);
    testutil_check(__wt_snprintf(path, len, "%s/BACKUP", g.home));

    wts_open(path, false, &conn);

    testutil_checkfmt(conn->open_session(conn, NULL, NULL, &session), "%s", path);

    /*
     * Verify can return EBUSY if the handle isn't available. Don't yield and retry, in the case of
     * LSM, the handle may not be available for a long time.
     */
    ret = session->verify(session, g.uri, NULL);
    testutil_assertfmt(ret == 0 || ret == EBUSY, "WT_SESSION.verify: %s: %s", path, g.uri);

    testutil_checkfmt(conn->close(conn, NULL), "%s", path);

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
    size_t len, tmp_sz;
    ssize_t rdsize;
    uint64_t offset, type;
    u_int size;
    int ret, rfd, wfd1, wfd2;
    char buf[512], config[512], *first, *second, *tmp;
    bool first_pass;

    /*
     * We need to prepend the home directory name here because we are not using the WiredTiger
     * internal functions that would prepend it for us.
     */
    len = strlen(g.home) + strlen("BACKUP") + strlen(name) + 10;
    first = dmalloc(len);

    /*
     * Save another copy of the original file to make debugging recovery errors easier.
     */
    len = strlen(g.home) + strlen("BACKUP.copy") + strlen(name) + 10;
    second = dmalloc(len);
    testutil_check(__wt_snprintf(config, sizeof(config), "incremental=(file=%s)", name));

    /* Open the duplicate incremental backup cursor with the file name given. */
    tmp_sz = 0;
    tmp = NULL;
    first_pass = true;
    rfd = wfd1 = wfd2 = -1;
    testutil_check(session->open_cursor(session, NULL, bkup_c, config, &incr_cur));
    while ((ret = incr_cur->next(incr_cur)) == 0) {
        testutil_check(incr_cur->get_key(incr_cur, &offset, (uint64_t *)&size, &type));
        if (type == WT_BACKUP_RANGE) {
            /*
             * Since we are using system calls below instead of a WiredTiger function, we have to
             * prepend the home directory to the file names ourselves.
             */
            testutil_check(__wt_snprintf(first, len, "%s/BACKUP/%s", g.home, name));
            testutil_check(__wt_snprintf(second, len, "%s/BACKUP.copy/%s", g.home, name));
            if (tmp_sz < size) {
                tmp = drealloc(tmp, size);
                tmp_sz = size;
            }
            if (first_pass) {
                testutil_check(__wt_snprintf(buf, sizeof(buf), "%s/%s", g.home, name));
                error_sys_check(rfd = open(buf, O_RDONLY, 0));
                error_sys_check(wfd1 = open(first, O_WRONLY | O_CREAT, 0));
                error_sys_check(wfd2 = open(second, O_WRONLY | O_CREAT, 0));
                first_pass = false;
            }
            error_sys_check(lseek(rfd, (wt_off_t)offset, SEEK_SET));
            error_sys_check(rdsize = read(rfd, tmp, size));
            error_sys_check(lseek(wfd1, (wt_off_t)offset, SEEK_SET));
            error_sys_check(lseek(wfd2, (wt_off_t)offset, SEEK_SET));
            /* Use the read size since we may have read less than the granularity. */
            error_sys_check(write(wfd1, tmp, (size_t)rdsize));
            error_sys_check(write(wfd2, tmp, (size_t)rdsize));
        } else {
            /*
             * These operations are using a WiredTiger function so it will prepend the home
             * directory to the name for us.
             */
            testutil_check(__wt_snprintf(first, len, "BACKUP/%s", name));
            testutil_check(__wt_snprintf(second, len, "BACKUP.copy/%s", name));
            testutil_assert(type == WT_BACKUP_FILE);
            testutil_assert(rfd == -1);
            testutil_assert(first_pass == true);
            testutil_check(__wt_copy_and_sync(session, name, first));
            testutil_check(__wt_copy_and_sync(session, first, second));
        }
    }
    testutil_check(incr_cur->close(incr_cur));
    if (rfd != -1) {
        error_sys_check(close(rfd));
        error_sys_check(close(wfd1));
        error_sys_check(close(wfd2));
    }
    free(first);
    free(second);
    free(tmp);
}
/*
 * copy_file --
 *     Copy a single file into the backup directories.
 */
static void
copy_file(WT_SESSION *session, const char *name)
{
    size_t len;
    char *first, *second;

    len = strlen("BACKUP") + strlen(name) + 10;
    first = dmalloc(len);
    testutil_check(__wt_snprintf(first, len, "BACKUP/%s", name));
    testutil_check(__wt_copy_and_sync(session, name, first));

    /*
     * Save another copy of the original file to make debugging recovery errors easier.
     */
    len = strlen("BACKUP.copy") + strlen(name) + 10;
    second = dmalloc(len);
    testutil_check(__wt_snprintf(second, len, "BACKUP.copy/%s", name));
    testutil_check(__wt_copy_and_sync(session, first, second));

    free(first);
    free(second);
}

/*
 * Backup directory initialize command, remove and re-create the primary backup directory, plus a
 * copy we maintain for recovery testing.
 */
#define HOME_BACKUP_INIT_CMD "rm -rf %s/BACKUP %s/BACKUP.copy && mkdir %s/BACKUP %s/BACKUP.copy"

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
    size_t len;
    u_int incremental, period;
    uint64_t src_id;
    const char *config, *key;
    char cfg[512], *cmd;
    bool full, incr_full;

    (void)(arg);

    conn = g.wts_conn;

    /* Guarantee backup ID uniqueness, we might be reopening an existing database. */
    __wt_seconds(NULL, &g.backup_id);

    /* Open a session. */
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /*
     * Perform a full backup at somewhere under 10 seconds (that way there's at least one), then at
     * larger intervals, optionally do incremental backups between full backups.
     */
    incr_full = true;
    incremental = 0;
    active_files_init(&active[0]);
    active_files_init(&active[1]);
    active_now = active_prev = NULL;
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
             * identifier for this one.
             */
            if (incr_full) {
                active_files_free(&active[0]);
                active_files_free(&active[1]);
                active_now = &active[g.backup_id % 2];
                active_prev = NULL;
                testutil_check(__wt_snprintf(
                  cfg, sizeof(cfg), "incremental=(enabled,this_id=ID%" PRIu64 ")", g.backup_id++));
                full = true;
                incr_full = false;
            } else {
                if (active_prev == &active[0])
                    active_now = &active[1];
                else
                    active_now = &active[0];
                src_id = g.backup_id - 1;
                testutil_check(__wt_snprintf(cfg, sizeof(cfg),
                  "incremental=(enabled,src_id=ID%" PRIu64 ",this_id=ID%" PRIu64 ")", src_id,
                  g.backup_id++));
                /* Restart a full incremental every once in a while. */
                full = false;
                incr_full = mmrand(NULL, 1, 8) == 1;
            }
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
            len = strlen(g.home) * 4 + strlen(HOME_BACKUP_INIT_CMD) + 1;
            cmd = dmalloc(len);
            testutil_check(
              __wt_snprintf(cmd, len, HOME_BACKUP_INIT_CMD, g.home, g.home, g.home, g.home));
            testutil_checkfmt(system(cmd), "%s", "backup directory creation failed");
            free(cmd);
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
                    copy_file(session, key);
                else
                    copy_blocks(session, backup_cursor, key);

            } else
                copy_file(session, key);
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
        active_prev = active_now;

        /*
         * If automatic log archival isn't configured, optionally do incremental backups after each
         * full backup. If we're not doing any more incrementals, verify the backup (we can't verify
         * intermediate states, once we perform recovery on the backup database, we can't do any
         * more incremental backups).
         */
        if (full)
            incremental = g.c_logging_archive ? 1 : mmrand(NULL, 1, 8);
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
