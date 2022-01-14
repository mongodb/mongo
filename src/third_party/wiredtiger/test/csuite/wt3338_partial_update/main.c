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
 * JIRA ticket reference: WT-3338 Test case description: Smoke-test the partial update construction.
 */

#define DEBUG 0

#define DATASIZE 1024
#define MAX_MODIFY_ENTRIES 37 /* Maximum modify vectors */

static WT_MODIFY entries[MAX_MODIFY_ENTRIES]; /* Entries vector */
static int nentries;                          /* Entries count */

/*
 * The replacement bytes array is 2x the maximum replacement string so we can offset into it by the
 * maximum replacement string and still take a maximum replacement string without going past the end
 * of the buffer.
 */
#define MAX_REPL_BYTES 17
static char modify_repl[MAX_REPL_BYTES * 2]; /* Replacement bytes */

static WT_RAND_STATE rnd; /* RNG state */

/*
 * show --
 *     Dump out a buffer.
 */
static void
show(WT_ITEM *buf, const char *tag)
{
    size_t i;
    const uint8_t *a;

    fprintf(stderr, "%s: %" WT_SIZET_FMT " bytes\n\t", tag, buf->size);
    for (a = buf->data, i = 0; i < buf->size; ++i, ++a)
        fprintf(stderr, " %c", isprint(*a) ? *a : '.');
    fprintf(stderr, "\n");
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
        modify_repl[i] = 'Z' - (i % 26);
}

/*
 * modify_build --
 *     Generate a set of modify vectors.
 */
static void
modify_build(void)
{
    int i;

    /* Mess up the entries. */
    memset(entries, 0xff, sizeof(entries));

    /*
     * Randomly select a number of byte changes, offsets and lengths. Allow a value of 0, the API
     * should accept it.
     */
    nentries = (int)(__wt_random(&rnd) % (MAX_MODIFY_ENTRIES + 1));
    for (i = 0; i < nentries; ++i) {
        entries[i].data.data = modify_repl + __wt_random(&rnd) % MAX_REPL_BYTES;
        entries[i].data.size = (size_t)(__wt_random(&rnd) % MAX_REPL_BYTES);
        entries[i].offset = (size_t)(__wt_random(&rnd) % DATASIZE);
        entries[i].size = (size_t)(__wt_random(&rnd) % MAX_REPL_BYTES);
    }
#if DEBUG
    for (i = 0; i < nentries; ++i)
        printf("%d: {%.*s} %" WT_SIZET_FMT " bytes replacing %" WT_SIZET_FMT
               " bytes @ %" WT_SIZET_FMT "\n",
          i, (int)entries[i].data.size, (char *)entries[i].data.data, entries[i].data.size,
          entries[i].size, entries[i].offset);
#endif
}

/*
 * compare --
 *     Compare two results.
 */
static void
compare(WT_ITEM *orig, WT_ITEM *local, WT_ITEM *library)
{
    size_t i, max;
    const uint8_t *p, *t;

    max = WT_MIN(local->size, library->size);
    if (local->size != library->size ||
      (local->size != 0 && memcmp(local->data, library->data, local->size) != 0)) {
        for (i = 0, p = local->data, t = library->data; i < max; ++i, ++p, ++t)
            if (*p != *t)
                break;
        fprintf(stderr, "results differ: ");
        if (max == 0)
            fprintf(stderr, "identical up to %" WT_SIZET_FMT " bytes\n", max);
        else
            fprintf(stderr, "first mismatch at offset %" WT_SIZET_FMT "\n", i);
        show(orig, "original");
        show(local, "local results");
        show(library, "library results");
        testutil_assert(false);
    }
}

/*
 * modify_run --
 *     Run some tests:
 *
 * 1. Create an initial value, a copy and a fake cursor to use with the WiredTiger routines.
 *     Generate a set of modify vectors and apply them to the item stored in the cursor using the
 *     modify apply API. Also apply the same modify vector to one of the copies using a helper
 *     routine written to test the modify API. The final value generated with the modify API and the
 *     helper routine should match.
 *
 * 2. Use the initial value and the modified value generated above as inputs into the
 *     calculate-modify API to generate a set of modify vectors. Apply this generated vector to the
 *     initial value using the modify apply API to obtain a final value. The final value generated
 *     should match the modified value that was used as input to the calculate-modify API.
 */
static void
modify_run(TEST_OPTS *opts)
{
    WT_CURSOR *cursor, _cursor;
    WT_DECL_RET;
    WT_ITEM *localA, _localA, *localB, _localB;
    WT_ITEM modtmp;
    WT_SESSION_IMPL *session;
    size_t len;
    int i, j;
    u_char *p;
    bool verbose;

    session = (WT_SESSION_IMPL *)opts->session;
    verbose = opts->verbose;

    /* Initialize the RNG. */
    __wt_random_init_seed(session, &rnd);

    /* Set up replacement information. */
    modify_repl_init();

    localA = &_localA;
    memset(&_localA, 0, sizeof(_localA));
    localB = &_localB;
    memset(&_localB, 0, sizeof(_localB));
    cursor = &_cursor;
    memset(&_cursor, 0, sizeof(_cursor));
    cursor->session = (WT_SESSION *)session;
    cursor->value_format = "u";
    memset(&modtmp, 0, sizeof(modtmp));

#define NRUNS 10000
    for (i = 0; i < NRUNS; ++i) {
        /* Create an initial value. */
        len = (size_t)(__wt_random(&rnd) % MAX_REPL_BYTES);
        testutil_check(__wt_buf_set(session, localA, modify_repl, len));

        for (j = 0; j < 1000; ++j) {
            /* Make lower case so modifications are easy to see. */
            for (p = localA->mem; WT_PTRDIFF(p, localA->mem) < localA->size; p++)
                *p = __wt_tolower(*p);

            /* Copy the current value into the second item. */
            testutil_check(__wt_buf_set(session, localB, localA->data, localA->size));

            /*
             * Create a random set of modify vectors, run the underlying library modification
             * function, then compare the result against our implementation of modify.
             */
            modify_build();
            testutil_check(__wt_buf_set(session, &cursor->value, localA->data, localA->size));
            testutil_check(__wt_modify_apply_api(cursor, entries, nentries));
            testutil_modify_apply(localA, &modtmp, entries, nentries);
            compare(localB, localA, &cursor->value);

            /*
             * Call the WiredTiger function to build a modification vector for the change, and
             * repeat the test using the WiredTiger modification vector, then compare results
             * against our implementation of modify.
             */
            nentries = WT_ELEMENTS(entries);
            ret = wiredtiger_calc_modify(opts->session, localB, localA,
              WT_MAX(localB->size, localA->size) + 100, entries, &nentries);
            if (ret == WT_NOTFOUND)
                continue;
            testutil_check(ret);
            testutil_check(__wt_buf_set(session, &cursor->value, localB->data, localB->size));
            testutil_check(__wt_modify_apply_api(cursor, entries, nentries));
            compare(localB, localA, &cursor->value);
        }
        if (verbose) {
            printf("%d (%d%%)\r", i, (i * 100) / NRUNS);
            fflush(stdout);
        }
    }
    if (verbose)
        printf("%d (100%%)\n", i);

    __wt_buf_free(session, localA);
    __wt_buf_free(session, localB);
    __wt_buf_free(session, &cursor->value);
    __wt_buf_free(session, &modtmp);
}

/*
 * main --
 *     TODO: Add a comment describing this function.
 */
int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    testutil_check(testutil_parse_opts(argc, argv, opts));
    testutil_make_work_dir(opts->home);
    testutil_check(wiredtiger_open(opts->home, NULL, "create", &opts->conn));
    testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &opts->session));

    /* Run the test. */
    modify_run(opts);

    testutil_cleanup(opts);
    return (EXIT_SUCCESS);
}
