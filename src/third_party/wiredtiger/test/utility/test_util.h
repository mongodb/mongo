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
#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include "wt_internal.h"

#ifdef _WIN32
#define DIR_DELIM '\\'
#define DIR_DELIM_STR "\\"
#define DIR_EXISTS_COMMAND "IF EXIST "
#define RM_COMMAND "rd /s /q "
#else
#define DIR_DELIM '/'
#define DIR_DELIM_STR "/"
#define RM_COMMAND "rm -rf "
#endif

#define DEFAULT_DIR "WT_TEST"
#define DEFAULT_TABLE_SCHEMA "key_format=i,value_format=S"
#define MKDIR_COMMAND "mkdir "

/* Subdirectory names, if we need to split the test directory into multiple subdirectories. */
#define RECORDS_DIR "records"
#define WT_HOME_DIR "WT_HOME"

/* Default file and subdirectory names to use for LazyFS in the tests. */
#define LAZYFS_BASE_DIR "base"
#define LAZYFS_CONFIG_FILE "lazyfs-config.toml"
#define LAZYFS_CONTROL_FILE "lazyfs-control.fifo"
#define LAZYFS_LOG_FILE "lazyfs.log"

#ifdef _WIN32
#include "windows_shim.h"
#endif

#define DIR_STORE_BUCKET_NAME "bucket"
#define S3_DEFAULT_BUCKET_NAME "s3testext;ap-southeast-2"

#define DIR_STORE "dir_store"
#define S3_STORE "s3_store"

#define TESTUTIL_ENV_CONFIG_TIERED                 \
    ",tiered_storage=(bucket=%s"                   \
    ",bucket_prefix=pfx-,local_retention=%" PRIu32 \
    ",name=%s"                                     \
    ",auth_token=%s)"
#define TESTUTIL_ENV_CONFIG_TIERED_EXT                                         \
    "\"%s/ext/storage_sources/%s/libwiredtiger_%s.so\"=("                      \
    "config=\"(delay_ms=%" PRIu64 ",error_ms=%" PRIu64 ",force_delay=%" PRIu64 \
    ",force_error=%" PRIu64 ",verbose=0)\")"
#define TESTUTIL_ENV_CONFIG_REC \
    ",log=(recover=on,remove=false),statistics=(all),statistics_log=(json,on_close,wait=1)"
#define TESTUTIL_ENV_CONFIG_COMPAT ",compatibility=(release=\"2.9\")"

/* Generic option parsing structure shared by all test cases. */
typedef struct {
    char *home;
    const char *argv0; /* Exec name */
    char usage[512];   /* Usage string for this parser */

    const char *progname;        /* Truncated program name */
    char *build_dir;             /* Build directory path */
    char *tiered_storage_source; /* Tiered storage source */

    enum {
        TABLE_NOT_SET = 0, /* Not explicitly set */
        TABLE_COL = 1,     /* Fixed-length column store */
        TABLE_FIX = 2,     /* Variable-length column store */
        TABLE_ROW = 3      /* Row-store */
    } table_type;

    FILE *progress_fp; /* Progress tracking file */
    char *progress_file_name;

    WT_RAND_STATE data_rnd;  /* PRNG state for data ops */
    WT_RAND_STATE extra_rnd; /* PRNG state for extra ops */
    uint64_t data_seed;      /* Random seed for data ops */
    uint64_t extra_seed;     /* Random seed for extra ops */

    uint64_t delay_ms;        /* Average length of delay when simulated */
    uint64_t error_ms;        /* Average length of delay when simulated */
    uint64_t force_delay;     /* Force a simulated network delay every N operations */
    uint64_t force_error;     /* Force a simulated network error every N operations */
    uint32_t local_retention; /* Local retention for tiered storage */

#define TESTUTIL_SEED_FORMAT "-PSD%" PRIu64 ",E%" PRIu64

    bool absolute_bucket_dir;  /* Use an absolute bucket path when it is a directory */
    bool compat;               /* Compatibility */
    bool do_data_ops;          /* Have schema ops use data */
    bool inmem;                /* In-memory */
    bool make_bucket_dir;      /* Create bucket when it is a directory */
    bool preserve;             /* Don't remove files on exit */
    bool tiered_begun;         /* Tiered storage ready */
    bool tiered_storage;       /* Configure tiered storage */
    bool verbose;              /* Run in verbose mode */
    uint64_t nrecords;         /* Number of records */
    uint64_t nops;             /* Number of operations */
    uint64_t nthreads;         /* Number of threads */
    uint64_t n_append_threads; /* Number of append threads */
    uint64_t n_read_threads;   /* Number of read threads */
    uint64_t n_write_threads;  /* Number of write threads */

    uint64_t tiered_flush_interval_us; /* Microseconds between flush_tier calls */
    uint64_t tiered_flush_next_us;     /* Next tiered flush in epoch microseconds */

    /*
     * Fields commonly shared within a test program. The test cleanup function will attempt to
     * automatically free and close non-null resources.
     */
    WT_CONNECTION *conn;
    WT_SESSION *session;
    volatile bool running; /* Whether to stop */
    char *uri;
    volatile uint64_t next_threadid;
    uint64_t unique_id;
    uint64_t max_inserted_id;

    /* Fields used internally by testutil library. */
    char **argv; /* Saved argument vector */
    int argc;    /* Saved argument count */
    const char *getopts_string;

} TEST_OPTS;

/*
 * A structure for the data specific to a single thread of those used by the group of threads
 * defined below.
 */
typedef struct {
    TEST_OPTS *testopts;
    int threadnum;
    int thread_counter;
} TEST_PER_THREAD_OPTS;

/*
 * A data structure for everything that we need to keep track of when using LazyFS.
 */
typedef struct {
    char base[PATH_MAX];       /* The base home directory under LazyFS, if using it */
    char config[PATH_MAX];     /* The LazyFS config file */
    char control[PATH_MAX];    /* The LazyFS FIFO file for controlling it */
    char mountpoint[PATH_MAX]; /* The mount home directory under LazyFS, if using it */
    char logfile[PATH_MAX];    /* The LazyFS log file */
    pid_t pid;                 /* The PID of the LazyFS process */
} WT_LAZY_FS;

/*
 * testutil_assert --
 *     Complain and quit if something isn't true, with no error value.
 */
#define testutil_assert(a)                                                   \
    do {                                                                     \
        if (!(a))                                                            \
            testutil_die(0, "%s/%d: %s", __PRETTY_FUNCTION__, __LINE__, #a); \
    } while (0)

/*
 * testutil_assert_errno --
 *     Complain and quit if something isn't true, with errno.
 */
#define testutil_assert_errno(a)                                                 \
    do {                                                                         \
        if (!(a))                                                                \
            testutil_die(errno, "%s/%d: %s", __PRETTY_FUNCTION__, __LINE__, #a); \
    } while (0)

/*
 * testutil_assertfmt --
 *     Complain and quit if something isn't true, with no error value, with formatted output.
 */
#define testutil_assertfmt(a, fmt, ...)                                                         \
    do {                                                                                        \
        if (!(a))                                                                               \
            testutil_die(0, "%s/%d: %s: " fmt, __PRETTY_FUNCTION__, __LINE__, #a, __VA_ARGS__); \
    } while (0)

/*
 * testutil_check --
 *     Complain and quit if a function call fails.
 */
#define testutil_check(call)                                                      \
    do {                                                                          \
        int __r;                                                                  \
        if ((__r = (call)) != 0)                                                  \
            testutil_die(__r, "%s/%d: %s", __PRETTY_FUNCTION__, __LINE__, #call); \
    } while (0)

/*
 * testutil_check_error_ok --
 *     Complain and quit if a function call fails, with specified error ok.
 */
#define testutil_check_error_ok(call, e)                                          \
    do {                                                                          \
        int __r;                                                                  \
        if ((__r = (call)) != 0 && (__r != (e)))                                  \
            testutil_die(__r, "%s/%d: %s", __PRETTY_FUNCTION__, __LINE__, #call); \
    } while (0)

/*
 * testutil_checkfmt --
 *     Complain and quit if a function call fails, with formatted output.
 */
#define testutil_checkfmt(call, fmt, ...)                                                 \
    do {                                                                                  \
        int __r;                                                                          \
        if ((__r = (call)) != 0)                                                          \
            testutil_die(                                                                 \
              __r, "%s/%d: %s: " fmt, __PRETTY_FUNCTION__, __LINE__, #call, __VA_ARGS__); \
    } while (0)

/*
 * WT_OP_CHECKPOINT_WAIT --
 *	If an operation returns EBUSY checkpoint and retry.
 */
#define WT_OP_CHECKPOINT_WAIT(session, op)                      \
    do {                                                        \
        int __ret;                                              \
        while ((__ret = (op)) == EBUSY)                         \
            testutil_check(session->checkpoint(session, NULL)); \
        testutil_check(__ret);                                  \
    } while (0)

/*
 * testutil_drop --
 *     Drop a table
 */
#define testutil_drop(session, uri, config)                            \
    do {                                                               \
        int __ret;                                                     \
        while ((__ret = session->drop(session, uri, config)) == EBUSY) \
            testutil_check(session->checkpoint(session, NULL));        \
        testutil_check(__ret);                                         \
    } while (0)

/*
 * testutil_verify --
 *     Verify a table
 */
#define testutil_verify(session, uri, config)                            \
    do {                                                                 \
        int __ret;                                                       \
        while ((__ret = session->verify(session, uri, config)) == EBUSY) \
            testutil_check(session->checkpoint(session, NULL));          \
        testutil_check(__ret);                                           \
    } while (0)

/*
 * error_sys_check --
 *     Complain and quit if a function call fails. A special name because it appears in the
 *     documentation. Allow any non-negative values.
 *
 * DO NOT USE THIS MACRO IN TEST CODE, IT IS ONLY FOR DOCUMENTATION.
 */
#define error_sys_check(call)                                                     \
    do {                                                                          \
        int __r;                                                                  \
        if ((__r = (int)(call)) < 0 && __r != ENOTSUP)                            \
            testutil_die(__r, "%s/%d: %s", __PRETTY_FUNCTION__, __LINE__, #call); \
    } while (0)

/*
 * error_check --
 *     Complain and quit if a function call fails. A special name because it appears in the
 *     documentation. Ignore ENOTSUP to allow library calls which might not be included in any
 *     particular build.
 *
 * DO NOT USE THIS MACRO IN TEST CODE, IT IS ONLY FOR DOCUMENTATION.
 */
#define error_check(call)                                                         \
    do {                                                                          \
        int __r;                                                                  \
        if ((__r = (call)) != 0 && __r != ENOTSUP)                                \
            testutil_die(__r, "%s/%d: %s", __PRETTY_FUNCTION__, __LINE__, #call); \
    } while (0)

/*
 * scan_end_check --
 *     Complain and quit if something isn't true. The same as testutil_assert, with a different name
 *     because it appears in the documentation.
 */
#define scan_end_check(a) testutil_assert(a)

#ifdef _WIN32
__declspec(noreturn)
#endif
  void testutil_die(int, const char *, ...) WT_GCC_FUNC_ATTRIBUTE((cold))
    WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));

/*
 * u64_to_string --
 *     Convert a uint64_t to a text string. Algorithm from Andrei Alexandrescu's talk: "Three
 *     Optimization Tips for C++"
 */
static inline void
u64_to_string(uint64_t n, char **pp)
{
    static const char hundred_lookup[201] =
      "0001020304050607080910111213141516171819"
      "2021222324252627282930313233343536373839"
      "4041424344454647484950515253545556575859"
      "6061626364656667686970717273747576777879"
      "8081828384858687888990919293949596979899";
    u_int i;
    char *p;

    /*
     * The argument pointer references the last element of a buffer (which must be large enough to
     * hold any possible value).
     *
     * Nul-terminate the buffer.
     */
    for (p = *pp, *p-- = '\0'; n >= 100; n /= 100) {
        i = (n % 100) * 2;
        *p-- = hundred_lookup[i + 1];
        *p-- = hundred_lookup[i];
    }

    /* Handle the last two digits. */
    i = (u_int)n * 2;
    *p = hundred_lookup[i + 1];
    if (n >= 10)
        *--p = hundred_lookup[i];

    /* Return a pointer to the first byte of the text string. */
    *pp = p;
}

/*
 * u64_to_string_zf --
 *     Convert a uint64_t to a text string, zero-filling the buffer.
 */
static inline void
u64_to_string_zf(uint64_t n, char *buf, size_t len)
{
    char *p;

    p = buf + (len - 1);
    u64_to_string(n, &p);

    while (p > buf)
        *--p = '0';
}

/*
 * testutil_timestamp_parse --
 *     Parse a timestamp to an integral value.
 */
static inline uint64_t
testutil_timestamp_parse(const char *str)
{
    uint64_t ts;
    char *p;

    ts = __wt_strtouq(str, &p, 16);
    testutil_assert((size_t)(p - str) <= WT_TS_HEX_STRING_SIZE);
    return (ts);
}

/*
 * maximum_stable_ts --
 *     Return the largest usable stable timestamp from a list of n committed timestamps.
 */
static inline wt_timestamp_t
maximum_stable_ts(wt_timestamp_t *commit_timestamps, uint32_t n)
{
    wt_timestamp_t commit_ts, ts;
    uint32_t i;

    for (ts = WT_TS_MAX, i = 0; i < n; i++) {
        commit_ts = commit_timestamps[i];
        if (commit_ts == WT_TS_NONE)
            return (WT_TS_NONE);
        if (commit_ts < ts)
            ts = commit_ts;
    }

    /* Return one less than the earliest in-use timestamp. */
    return (ts == WT_TS_MAX ? WT_TS_NONE : ts - 1);
}

/* Allow tests to add their own death handling. */
extern void (*custom_die)(void);

void *dcalloc(size_t, size_t);
void *dmalloc(size_t);
void *drealloc(void *, size_t);
void *dstrdup(const void *);
void *dstrndup(const char *, size_t);
const char *example_setup(int, char *const *);

/*
 * The functions below can generate errors that we wish to ignore. We have handler functions
 * available for them here, to avoid making tests crash prematurely.
 */
int handle_op_error(WT_EVENT_HANDLER *, WT_SESSION *, int, const char *);
int handle_op_message(WT_EVENT_HANDLER *, WT_SESSION *, const char *);
bool is_mounted(const char *);
void lazyfs_command(const char *, const char *);
void lazyfs_clear_cache(const char *);
void lazyfs_create_config(const char *, const char *, const char *);
void lazyfs_display_cache_usage(const char *);
void lazyfs_init(void);
bool lazyfs_is_implicitly_enabled(void);
pid_t lazyfs_mount(const char *, const char *, const char *);
void lazyfs_unmount(const char *, pid_t);
void op_bulk(void *);
void op_bulk_unique(void *);
void op_create(void *);
void op_create_unique(void *);
void op_cursor(void *);
void op_drop(void *);
bool testutil_is_flag_set(const char *);
bool testutil_is_dir_store(TEST_OPTS *);
void testutil_build_dir(TEST_OPTS *, char *, int);
void testutil_clean_test_artifacts(const char *);
void testutil_clean_work_dir(const char *);
void testutil_cleanup(TEST_OPTS *);
void testutil_copy_data(const char *);
void testutil_copy_data_opt(const char *, const char *);
void testutil_copy_file(WT_SESSION *, const char *);
void testutil_copy_if_exists(WT_SESSION *, const char *);
void testutil_create_backup_directory(const char *);
void testutil_deduce_build_dir(TEST_OPTS *opts);
int testutil_general_event_handler(
  WT_EVENT_HANDLER *, WT_CONNECTION *, WT_SESSION *, WT_EVENT_TYPE, void *);
void testutil_lazyfs_cleanup(WT_LAZY_FS *);
void testutil_lazyfs_clear_cache(WT_LAZY_FS *);
void testutil_lazyfs_setup(WT_LAZY_FS *, const char *);
void testutil_make_work_dir(const char *);
void testutil_modify_apply(WT_ITEM *, WT_ITEM *, WT_MODIFY *, int, uint8_t);
uint64_t testutil_pareto(uint64_t, uint64_t, u_int);
void testutil_parse_begin_opt(int, char *const *, const char *, TEST_OPTS *);
void testutil_parse_end_opt(TEST_OPTS *);
int testutil_parse_single_opt(TEST_OPTS *, int);
int testutil_parse_opts(int, char *const *, TEST_OPTS *);
void testutil_print_command_line(int argc, char *const *argv);
void testutil_progress(TEST_OPTS *, const char *);
void testutil_random_init(WT_RAND_STATE *, uint64_t *, uint32_t);
void testutil_random_from_random(WT_RAND_STATE *, WT_RAND_STATE *);
void testutil_random_from_seed(WT_RAND_STATE *, uint64_t);
#ifndef _WIN32
void testutil_sleep_wait(uint32_t, pid_t);
#endif
void testutil_system(const char *fmt, ...) WT_GCC_FUNC_ATTRIBUTE((format(printf, 1, 2)));
void testutil_wiredtiger_open(
  TEST_OPTS *, const char *, const char *, WT_EVENT_HANDLER *, WT_CONNECTION **, bool, bool);
void testutil_tiered_begin(TEST_OPTS *);
void testutil_tiered_flush_complete(TEST_OPTS *, WT_SESSION *, void *);
void testutil_tiered_sleep(TEST_OPTS *, WT_SESSION *, uint64_t, bool *);
void testutil_tiered_storage_configuration(TEST_OPTS *, char *, size_t, char *, size_t);
uint64_t testutil_time_us(WT_SESSION *);
void testutil_verify_src_backup(WT_CONNECTION *, const char *, const char *, char *);
void testutil_work_dir_from_path(char *, size_t, const char *);
WT_THREAD_RET thread_append(void *);

extern const char *progname;
const char *testutil_set_progname(char *const *);

#endif
