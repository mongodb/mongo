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

#ifdef _WIN32
#include "windows_shim.h"
#endif

/* Generic option parsing structure shared by all test cases. */
typedef struct {
    char *home;
    const char *argv0;    /* Exec name */
    const char *progname; /* Truncated program name */
    char *build_dir;      /* Build directory path */

    enum {
        TABLE_COL = 1, /* Fixed-length column store */
        TABLE_FIX = 2, /* Variable-length column store */
        TABLE_ROW = 3  /* Row-store */
    } table_type;

    FILE *progress_fp; /* Progress tracking file */
    char *progress_file_name;

    bool preserve;             /* Don't remove files on exit */
    bool verbose;              /* Run in verbose mode */
    bool do_data_ops;          /* Have schema ops use data */
    uint64_t nrecords;         /* Number of records */
    uint64_t nops;             /* Number of operations */
    uint64_t nthreads;         /* Number of threads */
    uint64_t n_append_threads; /* Number of append threads */
    uint64_t n_read_threads;   /* Number of read threads */
    uint64_t n_write_threads;  /* Number of write threads */

    /*
     * Fields commonly shared within a test program. The test cleanup function will attempt to
     * automatically free and close non-null resources.
     */
    WT_CONNECTION *conn;
    WT_SESSION *session;
    bool running;
    char *uri;
    volatile uint64_t next_threadid;
    uint64_t unique_id;
    uint64_t max_inserted_id;
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

/* Allow tests to add their own death handling. */
extern void (*custom_die)(void);

#ifdef _WIN32
__declspec(noreturn)
#endif
  void testutil_die(int, const char *, ...) WT_GCC_FUNC_ATTRIBUTE((cold))
    WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));

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
void op_bulk(void *);
void op_bulk_unique(void *);
void op_create(void *);
void op_create_unique(void *);
void op_cursor(void *);
void op_drop(void *);
bool testutil_is_flag_set(const char *);
void testutil_build_dir(TEST_OPTS *, char *, int);
void testutil_clean_test_artifacts(const char *);
void testutil_clean_work_dir(const char *);
void testutil_cleanup(TEST_OPTS *);
void testutil_copy_data(const char *);
void testutil_copy_file(WT_SESSION *, const char *);
void testutil_create_backup_directory(const char *);
void testutil_make_work_dir(const char *);
void testutil_modify_apply(WT_ITEM *, WT_ITEM *, WT_MODIFY *, int);
int testutil_parse_opts(int, char *const *, TEST_OPTS *);
void testutil_print_command_line(int argc, char *const *argv);
void testutil_progress(TEST_OPTS *, const char *);
#ifndef _WIN32
void testutil_sleep_wait(uint32_t, pid_t);
#endif
void testutil_timestamp_parse(const char *, uint64_t *);
void testutil_work_dir_from_path(char *, size_t, const char *);
WT_THREAD_RET thread_append(void *);

extern const char *progname;
const char *testutil_set_progname(char *const *);

#endif
