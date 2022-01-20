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
 *
 * ex_backup_block.c
 * 	demonstrates how to use block-based incremental backup.
 */
#include <test_util.h>

static const char *const home = "WT_BLOCK";
static const char *const home_full = "WT_BLOCK_LOG_FULL";
static const char *const home_incr = "WT_BLOCK_LOG_INCR";
static const char *const logpath = "logpath";

#define WTLOG "WiredTigerLog"
#define WTLOGLEN strlen(WTLOG)

static const char *const full_out = "./backup_block_full";
static const char *const incr_out = "./backup_block_incr";

static const char *const uri = "table:main";
static const char *const uri2 = "table:extra";

typedef struct __filelist {
    const char *name;
    bool exist;
} FILELIST;

static FILELIST *last_flist = NULL;
static size_t filelist_count = 0;

#define FLIST_INIT 16

#define CONN_CONFIG "create,cache_size=100MB,log=(enabled=true,path=logpath,file_max=100K)"
#define MAX_ITERATIONS 5
#define MAX_KEYS 10000

static int
compare_backups(int i)
{
    int ret;
    char buf[1024], msg[32];

    /*
     * We run 'wt dump' on both the full backup directory and the incremental backup directory for
     * this iteration. Since running 'wt' runs recovery and makes both directories "live", we need a
     * new directory for each iteration.
     *
     * If i == 0, we're comparing against the main, original directory with the final incremental
     * directory.
     */
    if (i == 0)
        (void)snprintf(buf, sizeof(buf), "../../wt -R -h %s dump main > %s.%d", home, full_out, i);
    else
        (void)snprintf(
          buf, sizeof(buf), "../../wt -R -h %s.%d dump main > %s.%d", home_full, i, full_out, i);
    error_check(system(buf));
    /*
     * Now run dump on the incremental directory.
     */
    (void)snprintf(
      buf, sizeof(buf), "../../wt -R -h %s.%d dump main > %s.%d", home_incr, i, incr_out, i);
    error_check(system(buf));

    /*
     * Compare the files.
     */
    (void)snprintf(buf, sizeof(buf), "cmp %s.%d %s.%d", full_out, i, incr_out, i);
    ret = system(buf);
    if (i == 0)
        (void)snprintf(msg, sizeof(msg), "%s", "MAIN");
    else
        (void)snprintf(msg, sizeof(msg), "%d", i);
    printf("Iteration %s: Tables %s.%d and %s.%d %s\n", msg, full_out, i, incr_out, i,
      ret == 0 ? "identical" : "differ");
    if (ret != 0)
        exit(1);

    /*
     * If they compare successfully, clean up.
     */
    if (i != 0) {
        (void)snprintf(buf, sizeof(buf), "rm -rf %s.%d %s.%d %s.%d %s.%d", home_full, i, home_incr,
          i, full_out, i, incr_out, i);
        error_check(system(buf));
    }
    return (ret);
}

/*
 * Set up all the directories needed for the test. We have a full backup directory for each
 * iteration and an incremental backup for each iteration. That way we can compare the full and
 * incremental each time through.
 */
static void
setup_directories(void)
{
    int i;
    char buf[1024];

    for (i = 0; i < MAX_ITERATIONS; i++) {
        /*
         * For incremental backups we need 0-N. The 0 incremental directory will compare with the
         * original at the end.
         */
        (void)snprintf(buf, sizeof(buf), "rm -rf %s.%d && mkdir -p %s.%d/%s", home_incr, i,
          home_incr, i, logpath);
        error_check(system(buf));
        if (i == 0)
            continue;
        /*
         * For full backups we need 1-N.
         */
        (void)snprintf(buf, sizeof(buf), "rm -rf %s.%d && mkdir -p %s.%d/%s", home_full, i,
          home_full, i, logpath);
        error_check(system(buf));
    }
}

static void
add_work(WT_SESSION *session, int iter, int iterj)
{
    WT_CURSOR *cursor, *cursor2;
    int i;
    char k[64], v[64];

    error_check(session->open_cursor(session, uri, NULL, NULL, &cursor));
    /*
     * Only on even iterations add content to the extra table. This illustrates and shows that
     * sometimes only some tables will be updated.
     */
    cursor2 = NULL;
    if (iter % 2 == 0)
        error_check(session->open_cursor(session, uri2, NULL, NULL, &cursor2));
    /*
     * Perform some operations with individual auto-commit transactions.
     */
    for (i = 0; i < MAX_KEYS; i++) {
        (void)snprintf(k, sizeof(k), "key.%d.%d.%d", iter, iterj, i);
        (void)snprintf(v, sizeof(v), "value.%d.%d.%d", iter, iterj, i);
        cursor->set_key(cursor, k);
        cursor->set_value(cursor, v);
        error_check(cursor->insert(cursor));
        if (cursor2 != NULL) {
            cursor2->set_key(cursor2, k);
            cursor2->set_value(cursor2, v);
            error_check(cursor2->insert(cursor2));
        }
    }
    error_check(cursor->close(cursor));
    if (cursor2 != NULL)
        error_check(cursor2->close(cursor2));
}

static int
finalize_files(FILELIST *flistp, size_t count)
{
    size_t i;
    char buf[512];

    /*
     * Process files that were removed. Any file that is not marked in the previous list as existing
     * in this iteration should be removed. Free all previous filenames as we go along. Then free
     * the overall list.
     */
    for (i = 0; i < filelist_count; ++i) {
        if (last_flist[i].name == NULL)
            break;
        if (!last_flist[i].exist) {
            (void)snprintf(buf, sizeof(buf), "rm WT_BLOCK_LOG_*/%s%s",
              strncmp(last_flist[i].name, WTLOG, WTLOGLEN) == 0 ? "logpath/" : "",
              last_flist[i].name);
            error_check(system(buf));
        }
        free((void *)last_flist[i].name);
    }
    free(last_flist);

    /* Set up the current list as the new previous list. */
    last_flist = flistp;
    filelist_count = count;
    return (0);
}

/*
 * Process a file name. Build up a list of current file names. But also process the file names from
 * the previous iteration. Mark any name we see as existing so that the finalize function can remove
 * any that don't exist. We walk the list each time. This is slow.
 */
static int
process_file(FILELIST **flistp, size_t *countp, size_t *allocp, const char *filename)
{
    FILELIST *flist;
    size_t alloc, i, new, orig;

    /* Build up the current list, growing as needed. */
    i = *countp;
    alloc = *allocp;
    flist = *flistp;
    if (i == alloc) {
        orig = alloc * sizeof(FILELIST);
        new = orig * 2;
        flist = realloc(flist, new);
        testutil_assert(flist != NULL);
        memset(flist + alloc, 0, new - orig);
        *allocp = alloc * 2;
        *flistp = flist;
    }

    flist[i].name = strdup(filename);
    flist[i].exist = false;
    ++(*countp);

    /* Check against the previous list. */
    for (i = 0; i < filelist_count; ++i) {
        /* If name is NULL, we've reached the end of the list. */
        if (last_flist[i].name == NULL)
            break;
        if (strcmp(filename, last_flist[i].name) == 0) {
            last_flist[i].exist = true;
            break;
        }
    }
    return (0);
}

static void
take_full_backup(WT_SESSION *session, int i)
{
    FILELIST *flist;
    WT_CURSOR *cursor;
    size_t alloc, count;
    int j, ret;
    char buf[1024], f[256], h[256];
    const char *filename, *hdir;

    /*
     * First time through we take a full backup into the incremental directories. Otherwise only
     * into the appropriate full directory.
     */
    if (i != 0) {
        (void)snprintf(h, sizeof(h), "%s.%d", home_full, i);
        hdir = h;
    } else
        hdir = home_incr;
    if (i == 0) {
        (void)snprintf(
          buf, sizeof(buf), "incremental=(granularity=1M,enabled=true,this_id=\"ID%d\")", i);
        error_check(session->open_cursor(session, "backup:", NULL, buf, &cursor));
    } else
        error_check(session->open_cursor(session, "backup:", NULL, NULL, &cursor));

    count = 0;
    alloc = FLIST_INIT;
    flist = calloc(alloc, sizeof(FILELIST));
    testutil_assert(flist != NULL);
    while ((ret = cursor->next(cursor)) == 0) {
        error_check(cursor->get_key(cursor, &filename));
        error_check(process_file(&flist, &count, &alloc, filename));

        /*
         * If it is a log file, prepend the path for cp.
         */
        if (strncmp(filename, WTLOG, WTLOGLEN) == 0)
            (void)snprintf(f, sizeof(f), "%s/%s", logpath, filename);
        else
            (void)snprintf(f, sizeof(f), "%s", filename);

        if (i == 0)
            /*
             * Take a full backup into each incremental directory.
             */
            for (j = 0; j < MAX_ITERATIONS; j++) {
                (void)snprintf(h, sizeof(h), "%s.%d", home_incr, j);
                (void)snprintf(buf, sizeof(buf), "cp %s/%s %s/%s", home, f, h, f);
#if 0
                printf("FULL: Copy: %s\n", buf);
#endif
                error_check(system(buf));
            }
        else {
#if 0
            (void)snprintf(h, sizeof(h), "%s.%d", home_full, i);
#endif
            (void)snprintf(buf, sizeof(buf), "cp %s/%s %s/%s", home, f, hdir, f);
#if 0
            printf("FULL %d: Copy: %s\n", i, buf);
#endif
            error_check(system(buf));
        }
    }
    scan_end_check(ret == WT_NOTFOUND);
    error_check(cursor->close(cursor));
    error_check(finalize_files(flist, count));
}

static void
take_incr_backup(WT_SESSION *session, int i)
{
    FILELIST *flist;
    WT_CURSOR *backup_cur, *incr_cur;
    uint64_t offset, size, type;
    size_t alloc, count, rdsize, tmp_sz;
    int j, ret, rfd, wfd;
    char buf[1024], h[256], *tmp;
    const char *filename, *idstr;
    bool first;

    tmp = NULL;
    tmp_sz = 0;
    /*! [Query existing IDs] */
    error_check(session->open_cursor(session, "backup:query_id", NULL, NULL, &backup_cur));
    while ((ret = backup_cur->next(backup_cur)) == 0) {
        error_check(backup_cur->get_key(backup_cur, &idstr));
        printf("Existing incremental ID string: %s\n", idstr);
    }
    error_check(backup_cur->close(backup_cur));
    /*! [Query existing IDs] */

    /* Open the backup data source for incremental backup. */
    (void)snprintf(buf, sizeof(buf), "incremental=(src_id=\"ID%d\",this_id=\"ID%d\"%s)", i - 1, i,
      i % 2 == 0 ? "" : ",consolidate=true");
    error_check(session->open_cursor(session, "backup:", NULL, buf, &backup_cur));
    rfd = wfd = -1;
    count = 0;
    alloc = FLIST_INIT;
    flist = calloc(alloc, sizeof(FILELIST));
    testutil_assert(flist != NULL);
    /* For each file listed, open a duplicate backup cursor and copy the blocks. */
    while ((ret = backup_cur->next(backup_cur)) == 0) {
        error_check(backup_cur->get_key(backup_cur, &filename));
        error_check(process_file(&flist, &count, &alloc, filename));
        (void)snprintf(h, sizeof(h), "%s.0", home_incr);
        if (strncmp(filename, WTLOG, WTLOGLEN) == 0)
            (void)snprintf(buf, sizeof(buf), "cp %s/%s/%s %s/%s/%s", home, logpath, filename, h,
              logpath, filename);
        else
            (void)snprintf(buf, sizeof(buf), "cp %s/%s %s/%s", home, filename, h, filename);
#if 0
        printf("Copying backup: %s\n", buf);
#endif
        error_check(system(buf));
        first = true;

        (void)snprintf(buf, sizeof(buf), "incremental=(file=%s)", filename);
        error_check(session->open_cursor(session, NULL, backup_cur, buf, &incr_cur));
#if 0
        printf("Taking incremental %d: File %s\n", i, filename);
#endif
        while ((ret = incr_cur->next(incr_cur)) == 0) {
            error_check(incr_cur->get_key(incr_cur, &offset, &size, &type));
            scan_end_check(type == WT_BACKUP_FILE || type == WT_BACKUP_RANGE);
#if 0
            printf("Incremental %s: KEY: Off %" PRIu64 " Size: %" PRIu64 " %s\n", filename, offset,
              size, type == WT_BACKUP_FILE ? "WT_BACKUP_FILE" : "WT_BACKUP_RANGE");
#endif
            if (type == WT_BACKUP_RANGE) {
                /*
                 * We should never get a range key after a whole file so the read file descriptor
                 * should be valid. If the read descriptor is valid, so is the write one.
                 */
                if (tmp_sz < size) {
                    tmp = realloc(tmp, size);
                    testutil_assert(tmp != NULL);
                    tmp_sz = size;
                }
                if (first) {
                    (void)snprintf(buf, sizeof(buf), "%s/%s", home, filename);
                    error_sys_check(rfd = open(buf, O_RDONLY, 0));
                    (void)snprintf(h, sizeof(h), "%s.%d", home_incr, i);
                    (void)snprintf(buf, sizeof(buf), "%s/%s", h, filename);
                    error_sys_check(wfd = open(buf, O_WRONLY | O_CREAT, 0));
                    first = false;
                }

                /*
                 * Don't use the system checker for lseek. The system check macro uses an int which
                 * is often 4 bytes and checks for any negative value. The offset returned from
                 * lseek is 8 bytes and we can have a false positive error check.
                 */
                if (lseek(rfd, (wt_off_t)offset, SEEK_SET) == -1)
                    testutil_die(errno, "lseek: read");
                error_sys_check(rdsize = (size_t)read(rfd, tmp, (size_t)size));
                if (lseek(wfd, (wt_off_t)offset, SEEK_SET) == -1)
                    testutil_die(errno, "lseek: write");
                /* Use the read size since we may have read less than the granularity. */
                error_sys_check(write(wfd, tmp, rdsize));
            } else {
                /* Whole file, so close both files and just copy the whole thing. */
                testutil_assert(first == true);
                rfd = wfd = -1;
                if (strncmp(filename, WTLOG, WTLOGLEN) == 0)
                    (void)snprintf(buf, sizeof(buf), "cp %s/%s/%s %s/%s/%s", home, logpath,
                      filename, h, logpath, filename);
                else
                    (void)snprintf(buf, sizeof(buf), "cp %s/%s %s/%s", home, filename, h, filename);
#if 0
                printf("Incremental: Whole file copy: %s\n", buf);
#endif
                error_check(system(buf));
            }
        }
        scan_end_check(ret == WT_NOTFOUND);
        /* Done processing this file. Close incremental cursor. */
        error_check(incr_cur->close(incr_cur));

        /* Close file descriptors if they're open. */
        if (rfd != -1) {
            error_check(close(rfd));
            error_check(close(wfd));
        }
        /*
         * For each file, we want to copy the file into each of the later incremental directories so
         * that they start out at the same for the next incremental round. We then check each
         * incremental directory along the way.
         */
        for (j = i; j < MAX_ITERATIONS; j++) {
            (void)snprintf(h, sizeof(h), "%s.%d", home_incr, j);
            if (strncmp(filename, WTLOG, WTLOGLEN) == 0)
                (void)snprintf(buf, sizeof(buf), "cp %s/%s/%s %s/%s/%s", home, logpath, filename, h,
                  logpath, filename);
            else
                (void)snprintf(buf, sizeof(buf), "cp %s/%s %s/%s", home, filename, h, filename);
            error_check(system(buf));
        }
    }
    scan_end_check(ret == WT_NOTFOUND);

    /* Done processing all files. Close backup cursor. */
    error_check(backup_cur->close(backup_cur));
    error_check(finalize_files(flist, count));
    free(tmp);
}

int
main(int argc, char *argv[])
{
    struct stat sb;
    WT_CONNECTION *wt_conn;
    WT_CURSOR *backup_cur;
    WT_SESSION *session;
    int i, j, ret;
    char cmd_buf[256], *idstr;

    (void)argc; /* Unused variable */
    (void)testutil_set_progname(argv);

    (void)snprintf(cmd_buf, sizeof(cmd_buf), "rm -rf %s && mkdir -p %s/%s", home, home, logpath);
    error_check(system(cmd_buf));
    error_check(wiredtiger_open(home, NULL, CONN_CONFIG, &wt_conn));

    setup_directories();
    error_check(wt_conn->open_session(wt_conn, NULL, NULL, &session));
    error_check(session->create(session, uri, "key_format=S,value_format=S"));
    error_check(session->create(session, uri2, "key_format=S,value_format=S"));
    printf("Adding initial data\n");
    add_work(session, 0, 0);

    printf("Taking initial backup\n");
    take_full_backup(session, 0);

    error_check(session->checkpoint(session, NULL));

    for (i = 1; i < MAX_ITERATIONS; i++) {
        printf("Iteration %d: adding data\n", i);
        /* For each iteration we may add work and checkpoint multiple times. */
        for (j = 0; j < i; j++) {
            add_work(session, i, j);
            error_check(session->checkpoint(session, NULL));
        }

        /*
         * The full backup here is only needed for testing and comparison purposes. A normal
         * incremental backup procedure would not include this.
         */
        printf("Iteration %d: taking full backup\n", i);
        take_full_backup(session, i);
        /*
         * Taking the incremental backup also calls truncate to remove the log files, if the copies
         * were successful. See that function for details on that call.
         */
        printf("Iteration %d: taking incremental backup\n", i);
        take_incr_backup(session, i);

        printf("Iteration %d: dumping and comparing data\n", i);
        error_check(compare_backups(i));
    }

    printf("Close and reopen the connection\n");
    /*
     * Close and reopen the connection to illustrate the durability of id information.
     */
    error_check(wt_conn->close(wt_conn, NULL));
    error_check(wiredtiger_open(home, NULL, CONN_CONFIG, &wt_conn));
    error_check(wt_conn->open_session(wt_conn, NULL, NULL, &session));

    printf("Verify query after reopen\n");
    error_check(session->open_cursor(session, "backup:query_id", NULL, NULL, &backup_cur));
    while ((ret = backup_cur->next(backup_cur)) == 0) {
        error_check(backup_cur->get_key(backup_cur, &idstr));
        printf("Existing incremental ID string: %s\n", idstr);
    }
    error_check(backup_cur->close(backup_cur));

    /*
     * We should have an entry for i-1 and i-2. Use the older one.
     */
    (void)snprintf(
      cmd_buf, sizeof(cmd_buf), "incremental=(src_id=\"ID%d\",this_id=\"ID%d\")", i - 2, i);
    error_check(session->open_cursor(session, "backup:", NULL, cmd_buf, &backup_cur));
    error_check(backup_cur->close(backup_cur));

    /*
     * After we're done, release resources. Test the force stop setting.
     */
    (void)snprintf(cmd_buf, sizeof(cmd_buf), "incremental=(force_stop=true)");
    error_check(session->open_cursor(session, "backup:", NULL, cmd_buf, &backup_cur));
    error_check(backup_cur->close(backup_cur));

    /*
     * Close the connection. We're done and want to run the final comparison between the incremental
     * and original.
     */
    error_check(wt_conn->close(wt_conn, NULL));

    printf("Final comparison: dumping and comparing data\n");
    error_check(compare_backups(0));
    for (i = 0; i < (int)filelist_count; ++i) {
        if (last_flist[i].name == NULL)
            break;
        free((void *)last_flist[i].name);
    }
    free(last_flist);

    /*
     * Reopen the connection to verify that the forced stop should remove incremental information.
     */
    error_check(wiredtiger_open(home, NULL, CONN_CONFIG, &wt_conn));
    error_check(wt_conn->open_session(wt_conn, NULL, NULL, &session));
    /*
     * We should not have any information.
     */
    (void)snprintf(
      cmd_buf, sizeof(cmd_buf), "incremental=(src_id=\"ID%d\",this_id=\"ID%d\")", i - 2, i);
    testutil_assert(session->open_cursor(session, "backup:", NULL, cmd_buf, &backup_cur) == ENOENT);
    error_check(wt_conn->close(wt_conn, NULL));

    (void)snprintf(cmd_buf, sizeof(cmd_buf), "%s/WiredTiger.backup.block", home);
    ret = stat(cmd_buf, &sb);
    testutil_assert(ret == -1 && errno == ENOENT);

    return (EXIT_SUCCESS);
}
