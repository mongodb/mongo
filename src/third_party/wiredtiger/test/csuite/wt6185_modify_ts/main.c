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

extern int __wt_optind;
extern char *__wt_optarg;

#define KEYNO 50
#define MAX_MODIFY_ENTRIES 5
#define MAX_OPS 25
#define RUNS 250
#define VALUE_SIZE 80

static WT_RAND_STATE rnd;

static struct { /* List of repeatable operations. */
    uint64_t ts;
    char *v;
} list[MAX_OPS];
static u_int lnext;

static char *tlist[MAX_OPS * 100]; /* List of traced operations. */
static u_int tnext;

static uint64_t ts; /* Current timestamp. */

static char keystr[100], modify_repl[256], tmp[4 * 1024];
static uint64_t keyrecno;

static bool use_columns = false;

/*
 * trace --
 *     Trace an operation.
 */
#define trace(fmt, ...)                                                    \
    do {                                                                   \
        testutil_assert(tnext < WT_ELEMENTS(tlist));                       \
        testutil_check(__wt_snprintf(tmp, sizeof(tmp), fmt, __VA_ARGS__)); \
        free(tlist[tnext]);                                                \
        tlist[tnext] = dstrdup(tmp);                                       \
        ++tnext;                                                           \
    } while (0)

static void usage(void) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));

/*
 * usage --
 *     Print usage message and exit.
 */
static void
usage(void)
{
    fprintf(stderr, "usage: %s [-ce] [-h home] [-S seed]\n", progname);
    exit(EXIT_FAILURE);
}

/*
 * cleanup --
 *     Discard allocated memory in case it's a sanitizer run.
 */
static void
cleanup(void)
{
    u_int i;

    for (i = 0; i < WT_ELEMENTS(list); ++i)
        free(list[i].v);
    for (i = 0; i < WT_ELEMENTS(tlist); ++i)
        free(tlist[i]);
}

/*
 * mmrand --
 *     Return a random value between a min/max pair, inclusive.
 */
static inline uint32_t
mmrand(u_int min, u_int max)
{
    uint32_t v;
    u_int range;

    /*
     * Test runs with small row counts can easily pass a max of 0 (for example, "g.rows / 20").
     * Avoid the problem.
     */
    if (max <= min)
        return (min);

    v = __wt_random(&rnd);
    range = (max - min) + 1;
    v %= range;
    v += min;
    return (v);
}

/*
 * change_key --
 *     Switch to a different key.
 */
static void
change_key(u_int n)
{
    if (use_columns)
        keyrecno = n + 1;
    else
        testutil_check(__wt_snprintf(keystr, sizeof(keystr), "%010u.key", n));
}

/*
 * set_key --
 *     Set the current key in the cursor.
 */
static void
set_key(WT_CURSOR *c)
{
    if (use_columns)
        c->set_key(c, keyrecno);
    else
        c->set_key(c, keystr);
}

/*
 * modify_repl_init --
 *     Initialize the replacement information.
 */
static void
modify_repl_init(void)
{
    size_t i;

    for (i = 0; i < sizeof(modify_repl); ++i)
        modify_repl[i] = "zyxwvutsrqponmlkjihgfedcba"[i % 26];
}

/*
 * modify_build --
 *     Generate a set of modify vectors.
 */
static void
modify_build(WT_MODIFY *entries, int *nentriesp, int tag)
{
    int i, nentries;

    /* Randomly select a number of byte changes, offsets and lengths. */
    nentries = (int)mmrand(1, MAX_MODIFY_ENTRIES);
    for (i = 0; i < nentries; ++i) {
        /*
         * Take between 0 and 10 bytes from a random spot in the modify data. Replace between 0 and
         * 10 bytes in a random spot in the value, but start at least 11 bytes into the buffer so we
         * skip leading key information.
         */
        entries[i].data.data = modify_repl + mmrand(1, sizeof(modify_repl) - 10);
        entries[i].data.size = (size_t)mmrand(0, 10);
        entries[i].offset = (size_t)mmrand(15, VALUE_SIZE);
        entries[i].size = (size_t)mmrand(0, 10);
        trace("modify %d: off=%" WT_SIZET_FMT ", size=%" WT_SIZET_FMT ", data=\"%.*s\"", tag,
          entries[i].offset, entries[i].size, (int)entries[i].data.size,
          (char *)entries[i].data.data);
    }

    *nentriesp = (int)nentries;
}

/*
 * modify --
 *     Make two modifications to a record inside a single transaction.
 */
static void
modify(WT_SESSION *session, WT_CURSOR *c)
{
    WT_MODIFY entries[MAX_MODIFY_ENTRIES];
    int cnt, loop, nentries;
    const char *v;

    testutil_check(session->begin_transaction(session, "isolation=snapshot"));

    /* Set a read timestamp 90% of the time. */
    if (mmrand(1, 10) != 1) {
        testutil_check(__wt_snprintf(tmp, sizeof(tmp), "read_timestamp=%" PRIx64, ts));
        testutil_check(session->timestamp_transaction(session, tmp));
    }

    /* Up to 4 modify operations, 80% chance for each. */
    for (cnt = loop = 1; loop < 5; ++cnt, ++loop)
        if (mmrand(1, 10) <= 8) {
            modify_build(entries, &nentries, cnt);
            set_key(c);
            testutil_check(c->modify(c, entries, nentries));
        }

    /* Commit 90% of the time, else rollback. */
    if (mmrand(1, 10) != 1) {
        set_key(c);
        testutil_check(c->search(c));
        testutil_check(c->get_value(c, &v));
        free(list[lnext].v);
        list[lnext].v = dstrdup(v);

        trace("modify read-ts=%" PRIu64 ", commit-ts=%" PRIu64, ts, ts + 1);
        trace("returned {%s}", v);

        testutil_check(__wt_snprintf(tmp, sizeof(tmp), "commit_timestamp=%" PRIx64, ts + 1));
        testutil_check(session->timestamp_transaction(session, tmp));
        testutil_check(session->commit_transaction(session, NULL));

        list[lnext].ts = ts + 1; /* Reread at commit timestamp */
        ++lnext;
    } else
        testutil_check(session->rollback_transaction(session, NULL));

    ++ts;
}

/*
 * repeat --
 *     Reread all previously committed modifications.
 */
static void
repeat(WT_SESSION *session, WT_CURSOR *c)
{
    u_int i;
    const char *v;

    for (i = 0; i < lnext; ++i) {
        testutil_check(session->begin_transaction(session, "isolation=snapshot"));
        testutil_check(__wt_snprintf(tmp, sizeof(tmp), "read_timestamp=%" PRIx64, list[i].ts));
        testutil_check(session->timestamp_transaction(session, tmp));

        set_key(c);
        testutil_check(c->search(c));
        testutil_check(c->get_value(c, &v));

        trace("repeat ts=%" PRIu64, list[i].ts);
        trace("expected {%s}", list[i].v);
        trace("   found {%s}", v);

        testutil_assert(strcmp(v, list[i].v) == 0);

        testutil_check(session->rollback_transaction(session, NULL));
    }
}

/*
 * evict --
 *     Force eviction of the underlying page.
 */
static void
evict(WT_CURSOR *c)
{
    trace("%s", "eviction");

    set_key(c);
    testutil_check(c->search(c));
    F_SET(c, WT_CURSTD_DEBUG_RESET_EVICT);
    testutil_check(c->reset(c));
    F_CLR(c, WT_CURSTD_DEBUG_RESET_EVICT);
}

/*
 * trace_die --
 *     Dump the trace.
 */
static void
trace_die(void)
{
    u_int i;

    fprintf(stderr, "\n");
    for (i = 0; i < tnext; ++i)
        fprintf(stderr, "%s\n", tlist[i]);
}

#define SET_VALUE(key, value)                                                           \
    do {                                                                                \
        char *__p;                                                                      \
        memset(value, '.', sizeof(value));                                              \
        value[sizeof(value) - 1] = '\0';                                                \
        testutil_check(__wt_snprintf(value, sizeof(value), "%010u.value", (u_int)key)); \
        for (__p = value; *__p != '\0'; ++__p)                                          \
            ;                                                                           \
        *__p = '.';                                                                     \
    } while (0)

/*
 * main --
 *     TODO: Add a comment describing this function.
 */
int
main(int argc, char *argv[])
{
    WT_CONNECTION *conn;
    WT_CURSOR *c;
    WT_SESSION *session;
    u_int i, j;
    int ch;
    char path[1024], table_config[128], value[VALUE_SIZE];
    const char *home, *v;
    bool no_checkpoint, no_eviction, preserve;

    (void)testutil_set_progname(argv);
    custom_die = trace_die;

    __wt_random_init_seed(NULL, &rnd);
    modify_repl_init();

    no_checkpoint = no_eviction = preserve = false;
    home = "WT_TEST.wt6185_modify_ts";
    while ((ch = __wt_getopt(progname, argc, argv, "Cceh:S:")) != EOF)
        switch (ch) {
        case 'C':
            /* Variable-length columns only (for now anyway) */
            use_columns = true;
            break;
        case 'c':
            no_checkpoint = true;
            break;
        case 'e':
            no_eviction = true;
            break;
        case 'h':
            home = __wt_optarg;
            break;
        case 'p':
            preserve = true;
            break;
        case 'S':
            rnd.v = strtoul(__wt_optarg, NULL, 10);
            break;
        default:
            usage();
        }
    argc -= __wt_optind;
    if (argc != 0)
        usage();

    testutil_work_dir_from_path(path, sizeof(path), home);
    testutil_make_work_dir(path);

    testutil_check(__wt_snprintf(
      table_config, sizeof(table_config), "key_format=%s,value_format=S", use_columns ? "r" : "S"));

    /* Load 100 records. */
    testutil_check(wiredtiger_open(
      path, NULL, "create,statistics=(all),statistics_log=(json,on_close,wait=1)", &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->create(session, "file:xxx", table_config));
    testutil_check(session->open_cursor(session, "file:xxx", NULL, NULL, &c));
    for (i = 0; i <= 100; ++i) {
        change_key(i);
        set_key(c);
        SET_VALUE(i, value);
        c->set_value(c, value);
        testutil_check(c->insert(c));
    }

    /* Flush, reopen and verify a record. */
    testutil_check(conn->close(conn, NULL));
    testutil_check(
      wiredtiger_open(path, NULL, "statistics=(all),statistics_log=(json,on_close,wait=1)", &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->create(session, "file:xxx", NULL));
    testutil_check(session->open_cursor(session, "file:xxx", NULL, NULL, &c));
    change_key(KEYNO);
    set_key(c);
    testutil_check(c->search(c));
    testutil_check(c->get_value(c, &v));
    SET_VALUE(KEYNO, value);
    testutil_assert(strcmp(v, value) == 0);

    testutil_check(conn->set_timestamp(conn, "oldest_timestamp=1"));

    /*
     * Loop doing N operations per loop. Each operation consists of modify operations and re-reading
     * all previous committed transactions, then optional page evictions and checkpoints.
     */
    for (i = 0, ts = 1; i < RUNS; ++i) {
        lnext = tnext = 0;
        trace("run %u, seed %" PRIu64, i, rnd.v);

        for (j = mmrand(10, MAX_OPS); j > 0; --j) {
            modify(session, c);
            repeat(session, c);

            /* 20% chance we evict the page. */
            if (!no_eviction && mmrand(1, 10) > 8)
                evict(c);

            /* 80% chance we checkpoint. */
            if (!no_checkpoint && mmrand(1, 10) > 8) {
                trace("%s", "checkpoint");
                testutil_check(session->checkpoint(session, NULL));
            }
        }
        testutil_assert(write(STDOUT_FILENO, ".", 1) == 1);
    }
    testutil_assert(write(STDOUT_FILENO, "\n", 1) == 1);

    testutil_check(conn->close(conn, NULL));

    cleanup();

    if (!preserve)
        testutil_clean_work_dir(home);
    return (EXIT_SUCCESS);
}
