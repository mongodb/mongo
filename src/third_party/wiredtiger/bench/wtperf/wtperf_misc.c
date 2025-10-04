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

#include "wtperf.h"

#define WT_BACKUP_COPY_SIZE (128 * 1024)

/* Common key snprintf function for both generating and deleting keys. */
static void
create_index_key(char *key_buf, size_t len, uint64_t index_val, uint64_t keyno)
{
    testutil_snprintf(key_buf, len, "%" PRIu64 ":%" PRIu64, index_val, keyno);
}

int
delete_index_key(WTPERF *wtperf, WT_CURSOR *index_cursor, char *key_buf, uint64_t keyno)
{
    CONFIG_OPTS *opts;
    uint64_t i, index_val;
    size_t len;
    int ret;

    opts = wtperf->opts;
    len = opts->key_sz + opts->value_sz_max;

    /* Delete any other index entries. */
    for (i = 1; i <= INDEX_MAX_MULTIPLIER; ++i) {
        index_val = i * INDEX_BASE;
        create_index_key(key_buf, len, index_val, keyno);
        index_cursor->set_key(index_cursor, key_buf);
        ret = index_cursor->remove(index_cursor);
        if (ret == 0 || ret == WT_NOTFOUND)
            continue;
        if (ret == WT_ROLLBACK)
            return (ret);
        lprintf(wtperf, ret, 1, "Delete earlier index key failed");
    }
    return (0);
}

/*
 * Set up an index key based on global values. The populate inserted index values of the form
 * INDEX_BASE:key. Then each workload thread is assigned an id and uses its id to modify the index
 * keys. This spreads out the keys for each key number but clusters the keys from each particular
 * thread.
 */
void
generate_index_key(WTPERF_THREAD *thread, bool populate, char *key_buf, uint64_t keyno)
{
    CONFIG_OPTS *opts;
    WTPERF *wtperf;
    uint64_t index_val, mult;
    size_t len;

    wtperf = thread->wtperf;
    opts = wtperf->opts;
    if (populate)
        mult = INDEX_POPULATE_MULT;
    else
        /* Multipliers go from 2 through the maximum.  */
        mult = __wt_random(&thread->rnd) % (INDEX_MAX_MULTIPLIER - INDEX_POPULATE_MULT) +
          INDEX_POPULATE_MULT + 1;

    len = opts->key_sz + opts->value_sz_max;
    index_val = mult * INDEX_BASE;
    create_index_key(key_buf, len, index_val, keyno);
}

/* Setup the logging output mechanism. */
int
setup_log_file(WTPERF *wtperf)
{
    CONFIG_OPTS *opts;
    size_t len;
    int ret;
    char *fname;

    opts = wtperf->opts;
    ret = 0;

    if (opts->verbose < 1)
        return (0);

    len = strlen(wtperf->monitor_dir) + strlen(opts->table_name) + strlen(".stat") + 2;
    fname = dmalloc(len);
    testutil_snprintf(fname, len, "%s/%s.stat", wtperf->monitor_dir, opts->table_name);
    if ((wtperf->logf = fopen(fname, "w")) == NULL) {
        ret = errno;
        fprintf(stderr, "%s: %s\n", fname, strerror(ret));
    }
    free(fname);
    if (wtperf->logf == NULL)
        return (ret);

    /* Use line buffering for the log file. */
    __wt_stream_set_line_buffer(wtperf->logf);
    return (0);
}

/*
 * Log printf - output a log message.
 */
void
lprintf(const WTPERF *wtperf, int err, uint32_t level, const char *fmt, ...)
{
    CONFIG_OPTS *opts;
    va_list ap;

    opts = wtperf->opts;

    if (err == 0 && level <= opts->verbose) {
        va_start(ap, fmt);
        vfprintf(wtperf->logf, fmt, ap);
        va_end(ap);
        fprintf(wtperf->logf, "\n");

        if (level < opts->verbose) {
            va_start(ap, fmt);
            vprintf(fmt, ap);
            va_end(ap);
            printf("\n");
        }
    }
    if (err == 0)
        return;

    /* We are dealing with an error. */
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, " Error: %s\n", wiredtiger_strerror(err));
    if (wtperf->logf != NULL) {
        va_start(ap, fmt);
        vfprintf(wtperf->logf, fmt, ap);
        va_end(ap);
        fprintf(wtperf->logf, " Error: %s\n", wiredtiger_strerror(err));
    }

    /* Never attempt to continue if we got a panic from WiredTiger. */
    if (err == WT_PANIC)
        abort();
}

/*
 * backup_read --
 *     Read in a file, used mainly to measure the impact of backup on a single machine. Backup is
 *     usually copied across different machines, therefore the write portion doesn't affect the
 *     machine backup is performing on.
 */
void
backup_read(WTPERF *wtperf, WT_SESSION *session)
{
    struct stat st;
    WT_CURSOR *backup_cursor;
    WT_DECL_RET;
    uint32_t buf_size, size, total;
    int rfd;
    ssize_t rdsize;
    char *buf;
    const char *filename;

    buf = NULL;

    /*
     * open_cursor can return EBUSY if concurrent with a metadata operation, retry in that case.
     */
    while ((ret = session->open_cursor(session, "backup:", NULL, NULL, &backup_cursor)) == EBUSY)
        __wt_yield();
    if (ret != 0)
        goto err;

    buf = dmalloc(WT_BACKUP_COPY_SIZE);
    while ((ret = backup_cursor->next(backup_cursor)) == 0) {
        testutil_check(backup_cursor->get_key(backup_cursor, &filename));

        rfd = -1;
        /* Open the file handle. */
        testutil_snprintf(buf, WT_BACKUP_COPY_SIZE, "%s/%s", wtperf->home, filename);
        error_sys_check(rfd = open(buf, O_RDONLY, 0644));
        if (rfd < 0)
            continue;

        /* Get the file's size. */
        testutil_check(stat(buf, &st));
        size = (uint32_t)st.st_size;

        total = 0;
        buf_size = WT_MIN(size, WT_BACKUP_COPY_SIZE);
        while (total < size) {
            /* Use the read size since we may have read less than the granularity. */
            error_sys_check(rdsize = read(rfd, buf, buf_size));

            /* If we get EOF, we're done. */
            if (rdsize == 0)
                break;
            total += (uint32_t)rdsize;
            buf_size = WT_MIN(buf_size, size - total);
        }
        testutil_check(close(rfd));
    }
    testutil_check(backup_cursor->close(backup_cursor));
err:
    free(buf);
}
