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
#ifndef _WIN32
#include <dirent.h>
#endif
#include <math.h>
#include "test_util.h"

/*
 * This test is to calculate benchmarks for tiered storage:
 * - This test populates tables of different sizes, say 100K, 1MB, 10MB,
 * 100MB and checkpoints with/without flush call and calculates time taken for
 * populate and checkpoint.
 */

#define HOME_BUF_SIZE 512
#define MAX_RUN 5
#define MAX_TIERED_FILES 10
#define NUM_RECORDS 500
#define MAX_VALUE_SIZE 200

/* Constants and variables declaration. */
static const char conn_config[] =
  "create,cache_size=2GB,statistics=(all),statistics_log=(json,on_close,wait=1)";
static const char table_config[] = "leaf_page_max=64KB,key_format=i,value_format=u";
static unsigned char data_str[MAX_VALUE_SIZE] = "";

static TEST_OPTS *opts, _opts;
static bool read_data = true;
static bool flush;

static WT_RAND_STATE rnd;
/* Forward declarations. */

static double calculate_std_deviation(const double *);
static void compute_wt_file_size(const char *, const char *, uint64_t *);
static void compute_tiered_file_size(const char *, const char *, uint64_t *);
static void fill_random_data(void);
static void get_file_size(const char *, uint64_t *);
static void populate(WT_SESSION *, uint32_t, uint32_t);
static void recover_validate(const char *, uint32_t, uint64_t, uint32_t);
static void run_test_clean(const char *, uint32_t);
static void run_test(const char *, uint32_t, uint32_t);
#ifndef _WIN32
static void remove_local_cached_files(const char *);
#endif

static double avg_wtime_arr[MAX_RUN], avg_rtime_arr[MAX_RUN], avg_wthroughput_arr[MAX_RUN],
  avg_rthroughput_arr[MAX_RUN];
static uint64_t avg_filesize_array[MAX_RUN];

/*
 * main --
 *     Methods implementation.
 */
int
main(int argc, char *argv[])
{
    int i;
    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    testutil_check(testutil_parse_opts(argc, argv, opts));

    flush = false;
    printf("The below benchmarks are average of %d runs\n", MAX_RUN);

    for (i = 0; i < 2; ++i) {

        printf(
          "########################################################################################"
          "\n");
        printf("                        Checkpoint is done with flush_tier %s\n",
          (opts->tiered_storage && flush) ? "enabled" : "disabled");
        printf(
          "########################################################################################"
          "\n");

        /*
         * Run test with 100K file size.
         */
        run_test_clean("100KB", NUM_RECORDS);

        /*
         * Run test with 1Mb file size.
         */
        run_test_clean("1MB", NUM_RECORDS * 10);

        /*
         * Run test with 10 Mb file size.
         */
        run_test_clean("10MB", NUM_RECORDS * 100);

        /*
         * Run test with 50 Mb file size.
         */
        run_test_clean("50MB", NUM_RECORDS * 500);

        /*
         * Run test with 100 Mb file size.
         */
        run_test_clean("100MB", NUM_RECORDS * WT_THOUSAND);
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
run_test_clean(const char *suffix, uint32_t num_records)
{
    char home_full[HOME_BUF_SIZE];
    double avg_wtime, avg_rtime, avg_wthroughput, avg_rthroughput;
    uint64_t avg_file_size;
    uint32_t counter;

    avg_file_size = 0;
    avg_wtime = avg_rtime = avg_rthroughput = avg_wthroughput = 0;

    for (counter = 0; counter < MAX_RUN; ++counter) {
        testutil_check(__wt_snprintf(
          home_full, HOME_BUF_SIZE, "%s_%s_%d_%" PRIu32, opts->home, suffix, flush, counter));
        run_test(home_full, num_records, counter);
    }

    /* Cleanup */
    if (!opts->preserve) {
        testutil_check(__wt_snprintf(home_full, HOME_BUF_SIZE, "%s_%s*", opts->home, suffix));
        testutil_clean_work_dir(home_full);
    }

    /* Compute the average */
    for (counter = 0; counter < MAX_RUN; ++counter) {
        avg_wtime += avg_wtime_arr[counter];
        avg_rtime += avg_rtime_arr[counter];
        avg_wthroughput += avg_wthroughput_arr[counter];
        avg_rthroughput += avg_rthroughput_arr[counter];
        avg_file_size += avg_filesize_array[counter];
    }

    printf("Bytes transferred: %" PRIu64
           " (~%s), W_Time: %.3f secs (SD %.3f), W_Tput: %.3f MB/sec (SD %.3f), R_Time: %.3f "
           "secs (SD %.3f), "
           "R_Tput: %.3f MB/sec (SD %.3f)\n",
      avg_file_size / MAX_RUN, suffix, avg_wtime / MAX_RUN, calculate_std_deviation(avg_wtime_arr),
      avg_wthroughput / MAX_RUN, calculate_std_deviation(avg_wthroughput_arr), avg_rtime / MAX_RUN,
      calculate_std_deviation(avg_rtime_arr), avg_rthroughput / MAX_RUN,
      calculate_std_deviation(avg_rthroughput_arr));
}

/*
 * fill_random_data --
 *     Fill random data.
 */
static void
fill_random_data(void)
{
    uint64_t i, str_len;

    str_len = sizeof(data_str) / sizeof(data_str[0]);
    for (i = 0; i < str_len - 1; i++)
        data_str[i] = '\x01' + (uint8_t)__wt_random(&rnd) % 255;

    data_str[str_len - 1] = '\0';
}

#ifndef _WIN32
/*
 * remove_local_cached_files --
 *     Remove local cached files and cached folders.
 */
static void
remove_local_cached_files(const char *home)
{
    struct dirent *dir_entry;
    DIR *dir;

    char *tablename;
    char delete_file[512], file_prefix[1024], rm_cmd[512];
    int highest, index, nmatches, objnum, status;

    highest = nmatches = objnum = 0;
    tablename = opts->uri;

    if (!WT_PREFIX_SKIP(tablename, "table:"))
        testutil_die(EINVAL, "unexpected uri: %s", opts->uri);

    /*
     * This code is to remove all the .wtobj files except the object file with highest object number
     * because that is the writable object.
     */
    dir = opendir(home);
    testutil_assert(dir != NULL);

    while ((dir_entry = readdir(dir)) != NULL) {
        if (strcmp(dir_entry->d_name, ".") == 0 || strcmp(dir_entry->d_name, "..") == 0)
            continue;

        if (!WT_PREFIX_MATCH(dir_entry->d_name, tablename))
            continue;

        ++nmatches;

        sscanf(dir_entry->d_name, "%*[^0-9]%d", &objnum);
        highest = WT_MAX(highest, objnum);
    }

    closedir(dir);

    testutil_check(__wt_snprintf(file_prefix, sizeof(file_prefix), "%s-000", tablename));
    if (highest > 1 && nmatches > 1) {
        for (index = 1; index < highest; index++) {
            testutil_check(__wt_snprintf(
              delete_file, sizeof(delete_file), "rm -f %s/%s*0%d.wtobj", home, file_prefix, index));
            status = system(delete_file);

            if (status != 0)
                testutil_die(status, "system: %s", delete_file);
        }
    }

    testutil_check(__wt_snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf %s/cache-*", home));
    status = system(rm_cmd);
    if (status < 0)
        testutil_die(status, "system: %s", rm_cmd);
}
#endif

/*
 * recover_validate --
 *     Open wiredtiger and validate the data.
 */
static void
recover_validate(const char *home, uint32_t num_records, uint64_t file_size, uint32_t counter)
{
    struct timeval start, end;

    char buf[1024];
    double diff_sec;
    size_t val_1_size, val_2_size;
    uint64_t key, i, v;

    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_ITEM item;
    WT_SESSION *session;

    /* Copy the data to a separate folder for debugging purpose. */
    testutil_copy_data(home);

    key = 0;
    buf[0] = '\0';

#ifndef _WIN32
    /*
     * Remove cached files and cached buckets.
     */
    if (opts->tiered_storage)
        remove_local_cached_files(home);
#endif

    /*
     * Open the connection which forces recovery to be run.
     */
    testutil_wiredtiger_open(opts, home, buf, NULL, &conn, true, true);
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Seed the random number generator */
    v = (uint32_t)getpid() + num_records + (2 * counter);
    __wt_random_init_custom_seed(&rnd, v);

    testutil_check(session->open_cursor(session, opts->uri, NULL, NULL, &cursor));
    val_1_size = MAX_VALUE_SIZE;

    gettimeofday(&start, 0);

    for (i = 0; i < num_records; i++) {
        fill_random_data();

        testutil_check(cursor->next(cursor));
        testutil_check(cursor->get_key(cursor, &key));
        testutil_assert(key == i + 1);
        testutil_check(cursor->get_value(cursor, &item));
        val_2_size = item.size;
        testutil_assert(val_1_size == val_2_size);
        testutil_assert(memcmp(data_str, item.data, item.size) == 0);
    }
    testutil_assert(cursor->next(cursor) == WT_NOTFOUND);
    gettimeofday(&end, 0);

    testutil_check(session->close(session, NULL));
    testutil_check(conn->close(conn, NULL));

    diff_sec = difftime_sec(start, end);
    avg_rtime_arr[counter] = diff_sec;
    avg_rthroughput_arr[counter] = ((file_size / diff_sec) / WT_MEGABYTE);
}

/*
 * run_test --
 *     This function runs the actual test and checkpoints with/without flush call based on the
 *     parameter.
 */
static void
run_test(const char *home, uint32_t num_records, uint32_t counter)
{
    struct timeval start, end;
    char buf[1024];
    double diff_sec;
    uint64_t file_size;

    WT_CONNECTION *conn;
    WT_SESSION *session;

    testutil_make_work_dir(home);
    if (opts->tiered_storage && testutil_is_dir_store(opts)) {
        testutil_check(__wt_snprintf(buf, sizeof(buf), "%s/%s", home, DIR_STORE_BUCKET_NAME));
        testutil_make_work_dir(buf);
    }

    testutil_wiredtiger_open(opts, home, conn_config, NULL, &conn, false, true);
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Create and populate table. Checkpoint the data after that. */
    testutil_check(session->create(session, opts->uri, table_config));

    testutil_check(__wt_snprintf(buf, sizeof(buf), flush ? "flush_tier=(enabled,force=true)" : ""));

    gettimeofday(&start, 0);

    populate(session, num_records, counter);
    testutil_check(session->checkpoint(session, buf));

    gettimeofday(&end, 0);
    diff_sec = difftime_sec(start, end);

    testutil_check(session->close(session, NULL));
    testutil_check(conn->close(conn, NULL));

    /* Sleep to guarantee the tables are created to read the size. */
    sleep(4);

    get_file_size(home, &file_size);
    testutil_assert(diff_sec > 0);

    avg_wtime_arr[counter] = diff_sec;
    avg_wthroughput_arr[counter] = ((file_size / diff_sec) / WT_MEGABYTE);
    avg_filesize_array[counter] = file_size;

    if (read_data)
        recover_validate(home, num_records, file_size, counter);
}

/*
 * populate --
 *     Populate the table.
 */
static void
populate(WT_SESSION *session, uint32_t num_records, uint32_t counter)
{
    WT_CURSOR *cursor;
    WT_ITEM item;
    uint64_t v, i;

    /* Seed the random number generator */
    v = (uint32_t)getpid() + num_records + (2 * counter);
    __wt_random_init_custom_seed(&rnd, v);

    testutil_check(session->open_cursor(session, opts->uri, NULL, NULL, &cursor));
    for (i = 0; i < num_records; i++) {
        fill_random_data();
        cursor->set_key(cursor, i + 1);
        item.data = data_str;
        item.size = sizeof(data_str);
        cursor->set_value(cursor, &item);
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
compute_tiered_file_size(const char *home, const char *tablename, uint64_t *file_size)
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
            *file_size += (uint64_t)stats.st_size;
    }
}

/*
 * compute_wt_file_size --
 *     Compute wt file size.
 */
static void
compute_wt_file_size(const char *home, const char *tablename, uint64_t *file_size)
{
    char stat_path[512];
    struct stat stats;

    *file_size = 0;
    testutil_check(__wt_snprintf(stat_path, sizeof(stat_path), "%s/%s.wt", home, tablename));
    if (stat(stat_path, &stats) == 0)
        *file_size = (uint64_t)stats.st_size;
    else
        testutil_die(ENOENT, "%s does not exist", stat_path);
}

/*
 * get_file_size --
 *     Retrieve the file size of the table.
 */
static void
get_file_size(const char *home, uint64_t *file_size)
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

/*
 * calculate_std_deviation --
 *     Calculate and return the standard deviation of the argument array.
 */
static double
calculate_std_deviation(const double *arr)
{
    double sum, mean, std_dev;
    int i;

    sum = mean = std_dev = 0.0;
    for (i = 0; i < MAX_RUN; ++i) {
        sum += arr[i];
    }
    mean = sum / MAX_RUN;
    for (i = 0; i < MAX_RUN; ++i) {
        std_dev += pow(arr[i] - mean, 2);
    }
    return sqrt(std_dev / MAX_RUN);
}
