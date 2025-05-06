/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "test_util.h"
#include "wt_internal.h"

extern int __wt_optind;
extern char *__wt_optarg;

static const char *uri = "table:normalized_pos";
static const int NUM_KEYS = 100000;
static int verbose = 0;

/*
 * usage --
 *     Print a usage message.
 */
__attribute__((noreturn)) static void
usage(void)
{
    fprintf(stderr, "usage: %s [-h dir]\n", progname);
    exit(EXIT_FAILURE);
}

/*
 * create_btree --
 *     Setup a btree with one key per page. Soft positions work on the in-memory btree, so use an
 *     in-memory version of WiredTiger to keep things simple when reasoning about the shape of the
 *     Btree.
 */
static void
create_btree(WT_CONNECTION *conn)
{
    WT_CURSOR *cursor;
    WT_SESSION *session;
    /* 1KB string to match the 1KB pages. */
    char val_str[1000];
    /* With 100,000 keys and 1 key per page this should mean that each key maps to an
     * equivalent npos. e.g. key 50,000 should map to roughly npos 0.5, and key 12,300 to 0.123.
     * This isn't true possibly because some pages have 10 slots and others 91? */

    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->create(session, uri,
      "key_format=Q,value_format=S,memory_page_max=1KB,leaf_page_max=1KB,allocation_size=1KB"));
    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));

    memset(val_str, 'A', 1000);
    val_str[1000 - 1] = '\0';

    for (int i = 0; i < NUM_KEYS; i++) {
        cursor->set_key(cursor, i);
        cursor->set_value(cursor, val_str);
        testutil_check(cursor->insert(cursor));
    }

    testutil_check(cursor->close(cursor));
    testutil_check(session->close(session, ""));
}

/*
 * test_normalized_pos --
 *     Given a key in a tree compute the normalized position (npos) of its page. Then make sure the
 *     soft position restores the same page.
 *
 * NOTE!! This is a white box test. It uses functions and types not available in the WiredTiger API.
 */
static void
test_normalized_pos(WT_CONNECTION *conn, bool in_mem,
  int (*page_from_npos_fn)(
    WT_SESSION_IMPL *session, WT_REF **refp, double npos, uint32_t read_flags, uint32_t walk_flags))
{
    WT_CURSOR *cursor;
    WT_DATA_HANDLE *dhandle;
    WT_REF *page_ref, *page_ref2;
    WT_SESSION *session;
    WT_SESSION_IMPL *wt_session;
    double npos, prev_npos;
    size_t path_str_offset;
    int count, count1, count2;
    char path_str[2][1024];

    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));

    wt_session = (WT_SESSION_IMPL *)session;
    dhandle = ((WT_CURSOR_BTREE *)cursor)->dhandle;

    /*
     * Traverse the whole dataset to stabilize the tree and make sure that we don't cause page
     * splits by looking into pages.
     */
    for (int key = 0; key < NUM_KEYS; key++) {
        cursor->set_key(cursor, key);
        testutil_check(cursor->search(cursor));
    }

    /*
     * Traverse the whole dataset without looking into the page content
     */
    if (verbose)
        printf("  forward\n");
    prev_npos = npos = 0.;
    page_ref2 = NULL;
    count1 = 0;
    do {
        ++count1;
        WT_WITH_DHANDLE(wt_session, dhandle, page_from_npos_fn(wt_session, &page_ref, npos, 0, 0));
        if (verbose > 1)
            printf("npos = %f, page_ref = %p\n", npos, (void *)page_ref);
        if (page_ref == NULL)
            break;

        if (in_mem)
            testutil_assertfmt(page_ref != page_ref2,
              "Got the same page twice: %p, npos = %lf, prev_npos = %lf", (void *)page_ref, npos,
              prev_npos);

        prev_npos = npos;
        page_ref2 = page_ref;
        npos = __wt_page_npos(wt_session, page_ref, 1. + 1e-5, NULL, NULL, 0);
        testutil_assertfmt(
          npos > prev_npos, "next npos(%lf) must be greater than prev_npos(%lf)", npos, prev_npos);
        WT_WITH_DHANDLE(
          wt_session, dhandle, testutil_check(__wt_page_release(wt_session, page_ref, 0)));
    } while (npos < 1.0);
    if (verbose)
        printf("  ... %d\n", count1);
    if (in_mem)
        testutil_assertfmt(count1 == NUM_KEYS,
          "should have traversed %d pages, but only traversed %d", NUM_KEYS, count1);
    /* For on-disk database, there's no guarantee that it's one key per page */

    /*
     * And the other way around.
     */
    if (verbose)
        printf("  backwards\n");
    prev_npos = npos = 1.;
    page_ref2 = NULL;
    count2 = 0;
    do {
        ++count2;
        WT_WITH_DHANDLE(wt_session, dhandle,
          page_from_npos_fn(wt_session, &page_ref, npos, WT_READ_PREV, WT_READ_PREV));
        if (verbose > 1)
            printf("npos = %f, page_ref = %p\n", npos, (void *)page_ref);
        if (page_ref == NULL)
            break;

        if (in_mem)
            testutil_assertfmt(page_ref != page_ref2,
              "Got the same page twice: %p, npos = %lf, prev_npos = %lf", (void *)page_ref, npos,
              prev_npos);

        prev_npos = npos;
        page_ref2 = page_ref;
        npos = __wt_page_npos(wt_session, page_ref, -1e-5, NULL, NULL, 0);
        testutil_assertfmt(
          npos < prev_npos, "next npos(%lf) must be smaller than prev_npos(%lf)", npos, prev_npos);
        WT_WITH_DHANDLE(
          wt_session, dhandle, testutil_check(__wt_page_release(wt_session, page_ref, 0)));
    } while (npos > 0.0);
    if (verbose)
        printf("  ... %d\n", count2);
    if (in_mem)
        testutil_assertfmt(count2 == NUM_KEYS,
          "should have traversed %d pages, but only traversed %d", NUM_KEYS, count2);
    /* For on-disk database, there's no guarantee that it's one key per page */

    if (in_mem || page_from_npos_fn == __wt_page_from_npos_for_read)
        testutil_assertfmt(count1 == count2,
          "Number of pages traversed forward (%d) and backward (%d) don't match", count1, count2);

    /*
     * Traverse the whole dataset, checking npos.
     */
    if (verbose)
        printf("  keys\n");
    prev_npos = 0.;
    path_str[0][0] = path_str[1][0] = 0;
    count = 0;
    for (int key = 0; key < NUM_KEYS; key++, count++) {
        cursor->set_key(cursor, key);
        testutil_check(cursor->search(cursor));

        path_str_offset = 0;
        page_ref = ((WT_CURSOR_BTREE *)cursor)->ref;

        /* Compute the soft position (npos) of the page */
        npos = __wt_page_npos(
          wt_session, page_ref, WT_NPOS_MID, path_str[count & 1], &path_str_offset, 1024);
        if (verbose > 1)
            printf("key %d: npos = %f, path_str = %s\n", key, npos, path_str[count & 1]);

        /* We're walking through all pages in order. Each page should have a larger or equal npos */
        testutil_assertfmt(npos >= prev_npos,
          "Page containing key %" PRIu64 " %s has npos (%f) smaller than the page of key %" PRIu64
          ", (%f) %s",
          key, path_str[count & 1], npos, key - 1, prev_npos, path_str[(count & 1) ^ 1]);
        prev_npos = npos;

        /* Now find which page npos restores to. We haven't modified the Btree so it should be the
         * exact same page */
        WT_WITH_DHANDLE(wt_session, dhandle,
          testutil_check(page_from_npos_fn(wt_session, &page_ref2, npos, 0, 0)));

        if (in_mem)
            testutil_assertfmt(page_ref == page_ref2,
              "page mismatch for key %llu!\n  Expected %p, got %p\n  npos = %f", key,
              (void *)page_ref, (void *)page_ref2, npos);

        /* __wt_page_from_npos sets a hazard pointer on the found page. Release it. */
        WT_WITH_DHANDLE(
          wt_session, dhandle, testutil_check(__wt_page_release(wt_session, page_ref2, 0)));
    }
    if (verbose)
        printf("  ... %d\n", count);

    testutil_check(cursor->close(cursor));
    testutil_check(session->close(session, ""));
}

/*
 * run --
 *     Run the test.
 *
 * Create a btree with one key per page. Soft positions work on the in-memory btree, so use an
 *     in-memory version of WiredTiger to keep things simple when reasoning about the shape of the
 *     btree.
 *
 * Then, test that a computed npos returns to the same page it was derived from. This assumes no
 *     change of the underlying btree during the test.
 */
static void
run(const char *working_dir, bool in_mem)
{
    WT_CONNECTION *conn;
    char home[1024];

    testutil_work_dir_from_path(home, sizeof(home), working_dir);
    testutil_recreate_dir(home);

    testutil_check(wiredtiger_open(home, NULL,
      in_mem ? "create,in_memory=true,cache_size=1GB" : "create,cache_size=1MB", &conn));

    create_btree(conn);

    if (verbose)
        printf(" evict\n");

    test_normalized_pos(conn, in_mem, __wt_page_from_npos_for_eviction);

    if (verbose)
        printf(" read\n");

    test_normalized_pos(conn, in_mem, __wt_page_from_npos_for_read);

    testutil_check(conn->close(conn, ""));
    testutil_remove(home);
}

/*
 * main --
 *     Test correctness of normalized position.
 */
int
main(int argc, char *argv[])
{
    int ch;
    const char *working_dir;

    working_dir = "WT_TEST.normalized_pos";

    while ((ch = __wt_getopt(progname, argc, argv, "h:v")) != EOF)
        switch (ch) {
        case 'h':
            working_dir = __wt_optarg;
            break;
        case 'v':
            ++verbose;
            break;
        default:
            usage();
        }

    argc -= __wt_optind;
    if (argc != 0)
        usage();

    if (verbose)
        printf("mem\n");
    run(working_dir, true);
    if (verbose)
        printf("disk\n");
    run(working_dir, false);
    return 0;
}
