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

/*
 * This test is to calculate benchmarks for tiered storage:
 * - This test populates tables of different sizes, say 100K, 1MB, 10MB,
 * 100MB and checkpoints with/without flush call and calculates time taken for
 * populate and checkpoint.
 */

#define HOME_BUF_SIZE 512
#define MAX_RUN 10
#define MAX_TIERED_FILES 10
#define NUM_RECORDS 500

/* Constants and variables declaration. */
static const char conn_config[] =
  "create,cache_size=2GB,statistics=(all),statistics_log=(json,on_close,wait=1)";
static const char table_config_row[] = "leaf_page_max=64KB,key_format=i,value_format=S";
static char data_str[200] = "";

static TEST_OPTS *opts, _opts;

/* Forward declarations. */

static void compute_wt_file_size(const char *, const char *, int64_t *);
static void compute_tiered_file_size(const char *, const char *, int64_t *);
static void get_file_size(const char *, int64_t *);
static void run_test_clean(const char *, uint64_t, bool);
static void run_test(const char *, uint64_t, bool, int);
static void populate(WT_SESSION *, uint64_t);

static double avg_time_array[MAX_RUN];
static double avg_throughput_array[MAX_RUN];
static int64_t avg_filesize_array[MAX_RUN];

/*
 * main --
 *     Methods implementation.
 */
int
main(int argc, char *argv[])
{
    bool flush;
    int i;
    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    testutil_check(testutil_parse_opts(argc, argv, opts));

    flush = false;
    printf("The below benchmarks are average of %d runs\n", MAX_RUN);
    for (i = 0; i < 2; ++i) {

        printf("############################################\n");
        printf(
          "            Flush call %s\n", (opts->tiered_storage && flush) ? "enabled" : "disabled");
        printf("############################################\n");

        /*
         * Run test with 100K file size. Row store case.
         */
        run_test_clean("100KB", NUM_RECORDS, flush);

        /*
         * Run test with 1Mb file size. Row store case.
         */
        run_test_clean("1MB", NUM_RECORDS * 10, flush);

        /*
         * Run test with 10 Mb file size. Row store case.
         */
        run_test_clean("10MB", NUM_RECORDS * 100, flush);

        /*
         * Run test with 100 Mb file size. Row store case.
         */
        run_test_clean("100MB", NUM_RECORDS * 1000, flush);
        flush = true;
    }

    testutil_cleanup(opts);

    return (EXIT_SUCCESS);
}

/*
 * difftime_msecs --
 *     Return the time in msecs.
 */
static double
difftime_msecs(struct timeval t0, struct timeval t1)
{
    return (t1.tv_sec - t0.tv_sec) * (double)WT_THOUSAND +
      (t1.tv_usec - t0.tv_usec) / (double)WT_THOUSAND;
}

/*
 * difftime_sec --
 *     Return the time in seconds.
 */
static double
difftime_sec(struct timeval t0, struct timeval t1)
{
    return difftime_msecs(t0, t1) / (double)WT_THOUSAND;
}

/*
 * run_test_clean --
 *     This function runs the test for configured number of times to compute the average time taken.
 */
static void
run_test_clean(const char *suffix, uint64_t num_records, bool flush)
{
    char home_full[HOME_BUF_SIZE];
    double avg_time, avg_throughput;
    int64_t avg_file_size;
    int counter;

    avg_file_size = 0;
    avg_time = avg_throughput = 0;

    for (counter = 0; counter < MAX_RUN; ++counter) {
        testutil_check(__wt_snprintf(
          home_full, HOME_BUF_SIZE, "%s_%s_%d_%d", opts->home, suffix, flush, counter));
        run_test(home_full, num_records, flush, counter);

        /* Cleanup */
        if (!opts->preserve)
            testutil_clean_work_dir(home_full);
    }

    for (counter = 0; counter < MAX_RUN; ++counter) {
        avg_time += avg_time_array[counter];
        avg_throughput += avg_throughput_array[counter];
        avg_file_size += avg_filesize_array[counter];
    }

    printf("Bytes Written- %" PRIi64
           " (~%s), Time took- %.3f seconds, Throughput- %.3f MB/second\n",
      avg_file_size / MAX_RUN, suffix, avg_time / MAX_RUN, avg_throughput / MAX_RUN);
}

/*
 * run_test --
 *     This function runs the actual test and checkpoints with/without flush call based on the
 *     parameter.
 */
static void
run_test(const char *home, uint64_t num_records, bool flush, int counter)
{
    struct timeval start, end;

    char buf[1024];
    double diff_sec;
    int64_t file_size;

    WT_CONNECTION *conn;
    WT_SESSION *session;

    testutil_make_work_dir(home);
    if (opts->tiered_storage) {
        testutil_check(__wt_snprintf(buf, sizeof(buf), "%s/bucket", home));
        testutil_make_work_dir(buf);
    }

    testutil_wiredtiger_open(opts, home, conn_config, NULL, &conn, false);
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Create and populate table. Checkpoint the data after that. */
    testutil_check(session->create(session, opts->uri, table_config_row));

    testutil_check(__wt_snprintf(buf, sizeof(buf), flush ? "flush_tier=(enabled,force=true)" : ""));

    gettimeofday(&start, 0);
    populate(session, num_records);

    testutil_check(session->checkpoint(session, buf));
    gettimeofday(&end, 0);

    diff_sec = difftime_sec(start, end);

    /* Sleep to guarantee the tables are created to read the size. */
    sleep(1);

    get_file_size(home, &file_size);
    testutil_assert(diff_sec > 0);

    avg_time_array[counter] = diff_sec;
    avg_throughput_array[counter] = ((file_size / diff_sec) / WT_MEGABYTE);
    avg_filesize_array[counter] = file_size;
}

/*
 * populate --
 *     Populate the table.
 */
static void
populate(WT_SESSION *session, uint64_t num_records)
{
    WT_CURSOR *cursor;
    WT_RAND_STATE rnd;
    uint64_t i, str_len;

    __wt_random_init_seed((WT_SESSION_IMPL *)session, &rnd);

    str_len = sizeof(data_str) / sizeof(data_str[0]);
    for (i = 0; i < str_len - 1; i++)
        data_str[i] = 'a' + (uint32_t)__wt_random(&rnd) % 26;

    data_str[str_len - 1] = '\0';

    testutil_check(session->open_cursor(session, opts->uri, NULL, NULL, &cursor));
    for (i = 0; i < num_records; i++) {
        cursor->set_key(cursor, i + 1);
        cursor->set_value(cursor, data_str);
        testutil_check(cursor->insert(cursor));
    }

    testutil_check(cursor->close(cursor));
    cursor = NULL;
}

/*
 * compute_tiered_file_size --
 *     Iterate over all the tiered files and compute file size..
 */
static void
compute_tiered_file_size(const char *home, const char *tablename, int64_t *file_size)
{
    char stat_path[512];
    int index;
    struct stat stats;

    *file_size = 0;
    for (index = 1; index < MAX_TIERED_FILES; ++index) {
        testutil_check(__wt_snprintf(
          stat_path, sizeof(stat_path), "%s/%s-%10.10d.wtobj", home, tablename, index));

        /* Return if the stat fails that means the file does not exist. */
        if (stat(stat_path, &stats) == 0)
            *file_size += stats.st_size;
        else
            return;
    }
}

/*
 * compute_wt_file_size --
 *     Compute wt file size.
 */
static void
compute_wt_file_size(const char *home, const char *tablename, int64_t *file_size)
{
    char stat_path[512];
    struct stat stats;

    *file_size = 0;
    testutil_check(__wt_snprintf(stat_path, sizeof(stat_path), "%s/%s.wt", home, tablename));
    if (stat(stat_path, &stats) == 0)
        *file_size = stats.st_size;
    else
        testutil_die(ENOENT, "%s does not exist", stat_path);
}

/*
 * get_file_size --
 *     Retrieve the file size of the table.
 */
static void
get_file_size(const char *home, int64_t *file_size)
{
    const char *tablename;

    tablename = strchr(opts->uri, ':');
    testutil_assert(tablename != NULL);
    tablename++;

    if (opts->tiered_storage)
        compute_tiered_file_size(home, tablename, file_size);
    else
        compute_wt_file_size(home, tablename, file_size);
}
