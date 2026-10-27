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

#include "test_util.h"

#include <pthread.h>
#include <sys/stat.h>

/*
 * A log file handed out by a backup cursor has been sealed: no new records can be written to it.
 * Any records that were already allocated to it must have reached the file before a copy is taken,
 * otherwise the copy is short. This test writes with enough concurrency that slots are routinely
 * closed before all their joining threads have released (and written) them, takes a backup cursor,
 * copies the sealed file immediately, then flushes and compares the copy against the final file.
 */

static char home[1024]; /* Program working dir */
static const char *const uri = "table:main";

#define ENV_CONFIG                                                   \
    "create,log=(enabled,file_max=1MB,prealloc=false,remove=false)," \
    "transaction_sync=(enabled,method=none),statistics=(fast)"

#define LOG_PREFIX "WiredTigerLog."
#define SEALED_COPY "sealed.copy"

/*
 * Small enough that many records share a slot, so several threads can be filling one when it is
 * closed, which is the window where alloc_lsn has advanced over records that are not yet written.
 */
#define NWRITERS 16
#define V_SIZE (12 * 1024)
#define ITERATIONS 200
#define NKEYS 32

static WT_CONNECTION *conn;
static volatile bool running;

typedef struct {
    uint32_t id;
} writer_arg;

static void *writer_main(void *arg);

/*
 * writer_main --
 *     Write records until the main thread stops the writers.
 */
static void *
writer_main(void *arg)
{
    WT_CURSOR *cursor;
    WT_SESSION *session;
    writer_arg *wa = (writer_arg *)arg;
    char k[64], *v;
    uint32_t i;

    if ((v = malloc(V_SIZE)) == NULL)
        testutil_die(errno, "malloc");
    memset(v, (int)('a' + wa->id), V_SIZE);
    v[V_SIZE - 1] = '\0';

    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));

    for (i = 0; running; ++i) {
        testutil_snprintf(k, sizeof(k), "key%02" PRIu32, i % NKEYS);
        cursor->set_key(cursor, k);
        cursor->set_value(cursor, v);
        testutil_check(cursor->insert(cursor));
    }

    testutil_check(cursor->close(cursor));
    testutil_check(session->close(session, NULL));
    free(v);
    return (NULL);
}

/*
 * read_file --
 *     Read a whole file into memory.
 */
static void
read_file(const char *path, uint8_t **bufp, size_t *sizep)
{
    struct stat sb;
    FILE *fp;
    size_t len, n;
    uint8_t *buf;

    if ((fp = fopen(path, "r")) == NULL)
        testutil_die(errno, "fopen: %s", path);
    testutil_assert_errno(fstat(fileno(fp), &sb) == 0);
    *sizep = (size_t)sb.st_size;
    if ((buf = malloc(*sizep + 1)) == NULL)
        testutil_die(errno, "malloc");
    /*
     * The log file server truncates a sealed file to its final length once the backup cursor is
     * closed, which can happen while the file is being read. Read to end of file rather than
     * trusting the size from fstat: a short read only loses the pre-allocated zeroed tail.
     */
    for (len = 0; len < (size_t)sb.st_size; len += n)
        if ((n = fread(buf + len, 1, (size_t)sb.st_size - len, fp)) == 0)
            break;
    if (ferror(fp))
        testutil_die(errno, "fread: %s", path);
    (void)fclose(fp);
    *sizep = len;
    *bufp = buf;
}

/*
 * compare_copy --
 *     Compare the copy taken at seal time with the source file after the flush. They must be
 *     identical: the file was sealed, so the only possible difference is bytes that had not been
 *     written when the copy was taken.
 */
static bool
compare_copy(const char *src_path)
{
    size_t copy_len, i, src_len;
    uint8_t *copy, *src;
    bool differ;

    read_file(SEALED_COPY, &copy, &copy_len);
    read_file(src_path, &src, &src_len);

    /*
     * The copy may be longer than the source: log files are extended to their maximum size, and the
     * final source file is truncated to the last record when it is closed. Compare the source
     * extent, which is where the sealed records end.
     */
    differ = false;
    if (copy_len < src_len) {
        printf("sealed copy is %" PRIu64 " bytes, source is %" PRIu64 " bytes\n",
          (uint64_t)copy_len, (uint64_t)src_len);
        differ = true;
    }
    for (i = 0; i < copy_len && i < src_len; ++i)
        if (copy[i] != src[i]) {
            printf("first difference at offset %" PRIu64 ": copy %#x, source %#x\n", (uint64_t)i,
              copy[i], src[i]);
            differ = true;
            break;
        }

    free(copy);
    free(src);
    return (differ);
}

extern int __wt_optind;
extern char *__wt_optarg;

static void usage(void) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));

/*
 * usage --
 *     Print usage and exit.
 */
static void
usage(void)
{
    fprintf(stderr, "usage: %s [-h dir] [-p]\n", progname);
    exit(EXIT_FAILURE);
}

/*
 * main --
 *     Run the backup seal test.
 */
int
main(int argc, char *argv[])
{
    WT_CURSOR *backup_cursor;
    WT_SESSION *session;
    pthread_t threads[NWRITERS];
    writer_arg args[NWRITERS];
    const char *file;
    int ch, ret;
    const char *working_dir;
    char sealed[64];
    uint32_t i, iter, sealed_num;
    bool preserve;

    preserve = false;
    (void)testutil_set_progname(argv);

    working_dir = "WT_TEST.wt18969_log_backup_seal";
    while ((ch = __wt_getopt(progname, argc, argv, "h:p")) != EOF)
        switch (ch) {
        case 'h':
            working_dir = __wt_optarg;
            break;
        case 'p':
            preserve = true;
            break;
        default:
            usage();
        }
    argc -= __wt_optind;
    if (argc != 0)
        usage();

    testutil_work_dir_from_path(home, sizeof(home), working_dir);
    testutil_recreate_dir(home);
    if (chdir(home) != 0)
        testutil_die(errno, "chdir: %s", home);

    testutil_check(wiredtiger_open(NULL, NULL, ENV_CONFIG, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->create(session, uri, "key_format=S,value_format=S"));

    running = true;
    for (i = 0; i < NWRITERS; ++i) {
        args[i].id = i;
        testutil_assert(pthread_create(&threads[i], NULL, writer_main, &args[i]) == 0);
    }

    for (iter = 0; iter < ITERATIONS; ++iter) {
        /*
         * Open a backup cursor. This seals the current log file and forces writes into a new one.
         */
        testutil_check(session->open_cursor(session, "backup:", NULL, NULL, &backup_cursor));

        /*
         * Find the highest-numbered journal in the list: that is the file that was active before
         * the seal.
         */
        sealed_num = 0;
        sealed[0] = '\0';
        while ((ret = backup_cursor->next(backup_cursor)) == 0) {
            uint32_t num;

            testutil_check(backup_cursor->get_key(backup_cursor, &file));
            if (strncmp(file, LOG_PREFIX, strlen(LOG_PREFIX)) != 0)
                continue;
            testutil_assert(sscanf(file, LOG_PREFIX "%" SCNu32, &num) == 1);
            if (num >= sealed_num) {
                sealed_num = num;
                testutil_snprintf(sealed, sizeof(sealed), "%s", file);
            }
        }
        testutil_assert(ret == WT_NOTFOUND);
        testutil_assert(sealed_num != 0);

        /*
         * Copy the sealed file immediately, the way a backup tooling copies the journal files it is
         * handed.
         */
        (void)unlink(SEALED_COPY);
        testutil_copy(sealed, SEALED_COPY);
        testutil_check(backup_cursor->close(backup_cursor));

        /*
         * Wait for any log records that were in flight at the seal to reach the file, then compare.
         */
        testutil_check(session->log_flush(session, NULL));
        if (compare_copy(sealed)) {
            printf("REPRODUCED on iteration %" PRIu32 ": %s was copied before it was complete\n",
              iter, sealed);
            break;
        }
    }

    running = false;
    for (i = 0; i < NWRITERS; ++i)
        testutil_assert(pthread_join(threads[i], NULL) == 0);

    testutil_check(conn->close(conn, NULL));

    if (iter == ITERATIONS)
        printf("no incomplete sealed copy detected in %d iterations\n", ITERATIONS);

    if (!preserve) {
        /* At this point $PATH is inside `home`, which we intend to delete. cd to the parent dir. */
        if (chdir("../") != 0)
            testutil_die(errno, "root chdir: %s", home);
        testutil_remove(home);
    }

    return (iter == ITERATIONS ? EXIT_SUCCESS : EXIT_FAILURE);
}
