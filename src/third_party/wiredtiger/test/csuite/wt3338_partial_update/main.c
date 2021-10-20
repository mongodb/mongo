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

static WT_MODIFY entries[1000]; /* Entries vector */
static int nentries;            /* Entries count */

/*
 * The replacement bytes array is 2x the maximum replacement string so we can offset into it by the
 * maximum replacement string and still take a maximum replacement string without going past the end
 * of the buffer.
 */
#define MAX_REPL_BYTES 17
static char modify_repl[MAX_REPL_BYTES * 2]; /* Replacement bytes */

static WT_RAND_STATE rnd; /* RNG state */

#if DEBUG
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
    for (a = buf->data, i = 0; i < buf->size; ++i, ++a) {
        if (isprint(*a))
            fprintf(stderr, " %c", *a);
        else
            fprintf(stderr, " %#x", *a);
    }
    fprintf(stderr, "\n");
}
#endif

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
modify_build(void)
{
    int i;

    /* Mess up the entries. */
    memset(entries, 0xff, MAX_MODIFY_ENTRIES * sizeof(entries[0]));

    /*
     * Randomly select a number of byte changes, offsets and lengths. Allow a value of 0, the API
     * should accept it.
     */
    nentries = (int)(__wt_random(&rnd) % MAX_MODIFY_ENTRIES);
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
          i, (int)entries[i].data.size, entries[i].data.data, entries[i].data.size, entries[i].size,
          entries[i].offset);
#endif
}

/*
 * slow_apply_api --
 *     Apply a set of modification changes using a different algorithm.
 */
static void
slow_apply_api(WT_ITEM *orig)
{
    static WT_ITEM _tb;
    WT_ITEM *ta, *tb, *tmp, _tmp;
    size_t len, size;
    int i;

    ta = orig;
    tb = &_tb;

    /* Mess up anything not initialized in the buffers. */
    memset((uint8_t *)ta->mem + ta->size, 0xff, ta->memsize - ta->size);
    memset((uint8_t *)tb->mem, 0xff, tb->memsize);

    /*
     * Process the entries to figure out how large a buffer we need. This is a bit pessimistic
     * because we're ignoring replacement bytes, but it's a simpler calculation.
     */
    for (size = ta->size, i = 0; i < nentries; ++i) {
        if (entries[i].offset >= size)
            size = entries[i].offset;
        size += entries[i].data.size;
    }

    testutil_check(__wt_buf_grow(NULL, ta, size));
    testutil_check(__wt_buf_grow(NULL, tb, size));

#if DEBUG
    show(ta, "slow-apply start");
#endif
    /*
     * From the starting buffer, create a new buffer b based on changes in the entries array. We're
     * doing a brute force solution here to test the faster solution implemented in the library.
     */
    for (i = 0; i < nentries; ++i) {
        /* Take leading bytes from the original, plus any gap bytes. */
        if (entries[i].offset >= ta->size) {
            memcpy(tb->mem, ta->mem, ta->size);
            if (entries[i].offset > ta->size)
                memset((uint8_t *)tb->mem + ta->size, '\0', entries[i].offset - ta->size);
        } else if (entries[i].offset > 0)
            memcpy(tb->mem, ta->mem, entries[i].offset);
        tb->size = entries[i].offset;

        /* Take replacement bytes. */
        if (entries[i].data.size > 0) {
            memcpy((uint8_t *)tb->mem + tb->size, entries[i].data.data, entries[i].data.size);
            tb->size += entries[i].data.size;
        }

        /* Take trailing bytes from the original. */
        len = entries[i].offset + entries[i].size;
        if (ta->size > len) {
            memcpy((uint8_t *)tb->mem + tb->size, (uint8_t *)ta->mem + len, ta->size - len);
            tb->size += ta->size - len;
        }
        testutil_assert(tb->size <= size);

        /* Swap the buffers and do it again. */
        tmp = ta;
        ta = tb;
        tb = tmp;
    }
    ta->data = ta->mem;
    tb->data = tb->mem;

    /*
     * The final results may not be in the original buffer, in which case we swap them back around.
     */
    if (ta != orig) {
        _tmp = *ta;
        *ta = *tb;
        *tb = _tmp;
    }

#if DEBUG
    show(ta, "slow-apply finish");
#endif
}

/*
 * compare --
 *     Compare two results.
 */
static void
compare(WT_ITEM *local, WT_ITEM *library)
{
#if DEBUG
    if (local->size != library->size || memcmp(local->data, library->data, local->size) != 0) {
        fprintf(stderr, "results differ\n");
        show(local, "local results");
        show(library, "library results");
    }
#endif
    testutil_assert(
      local->size == library->size && memcmp(local->data, library->data, local->size) == 0);
}

/*
 * modify_run
 *	Run some tests:
 *	1. Create an initial value, a copy and a fake cursor to use with the
 *	WiredTiger routines. Generate a set of modify vectors and apply them to
 *	the item stored in the cursor using the modify apply API. Also apply the
 *	same modify vector to one of the copies using a helper routine written
 *	to test the modify API. The final value generated with the modify API
 *	and the helper routine should match.
 *
 *	2. Use the initial value and the modified value generated above as
 *	inputs into the calculate-modify API to generate a set of modify
 *	vectors. Apply this generated vector to the initial value using the
 *	modify apply API to obtain a final value. The final value generated
 *	should match the modified value that was used as input to the
 *	calculate-modify API.
 */
static void
modify_run(bool verbose)
{
    WT_CURSOR *cursor, _cursor;
    WT_DECL_RET;
    WT_ITEM *localA, _localA, *localB, _localB;
    size_t len;
    int i, j;

    /* Initialize the RNG. */
    __wt_random_init_seed(NULL, &rnd);

    /* Set up replacement information. */
    modify_repl_init();

    /* We need three WT_ITEMs, one of them part of a fake cursor. */
    localA = &_localA;
    memset(&_localA, 0, sizeof(_localA));
    localB = &_localB;
    memset(&_localB, 0, sizeof(_localB));
    cursor = &_cursor;
    memset(&_cursor, 0, sizeof(_cursor));
    cursor->value_format = "u";

#define NRUNS 10000
    for (i = 0; i < NRUNS; ++i) {
        /* Create an initial value. */
        len = (size_t)(__wt_random(&rnd) % MAX_REPL_BYTES);
        testutil_check(__wt_buf_set(NULL, localA, modify_repl, len));

        for (j = 0; j < 1000; ++j) {
            /* Copy the current value into the second item. */
            testutil_check(__wt_buf_set(NULL, localB, localA->data, localA->size));

            /*
             * Create a random set of modify vectors, run the underlying library modification
             * function, then compare the result against our implementation of modify.
             */
            modify_build();
            testutil_check(__wt_buf_set(NULL, &cursor->value, localA->data, localA->size));
            testutil_check(__wt_modify_apply_api(NULL, cursor, entries, nentries));
            slow_apply_api(localA);
            compare(localA, &cursor->value);

            /*
             * Call the WiredTiger function to build a modification vector for the change, and
             * repeat the test using the WiredTiger modification vector, then compare results
             * against our implementation of modify.
             */
            nentries = WT_ELEMENTS(entries);
            ret = wiredtiger_calc_modify(
              NULL, localB, localA, WT_MAX(localB->size, localA->size) + 100, entries, &nentries);
            if (ret == WT_NOTFOUND)
                continue;
            testutil_check(ret);
            testutil_check(__wt_buf_set(NULL, &cursor->value, localB->data, localB->size));
            testutil_check(__wt_modify_apply_api(NULL, cursor, entries, nentries));
            compare(localA, &cursor->value);
        }
        if (verbose) {
            printf("%d (%d%%)\r", i, (i * 100) / NRUNS);
            fflush(stdout);
        }
    }
    if (verbose)
        printf("%d (100%%)\n", i);

    __wt_buf_free(NULL, localA);
    __wt_buf_free(NULL, localB);
    __wt_buf_free(NULL, &cursor->value);
}

int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    testutil_check(testutil_parse_opts(argc, argv, opts));
    testutil_make_work_dir(opts->home);
    testutil_check(wiredtiger_open(opts->home, NULL, "create", &opts->conn));

    /* Run the test. */
    modify_run(opts->verbose);

    testutil_cleanup(opts);
    return (EXIT_SUCCESS);
}
