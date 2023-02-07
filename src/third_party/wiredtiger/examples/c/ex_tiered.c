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
 *
 * ex_tiered.c
 *	This is an example demonstrating how to create and connect to a
 *	database running tiered storage.
 */
#include <test_util.h>

/*
 * The number of entries we insert at a time.
 */
#define N_ENTRIES 10

/*
 * Open the uri and starting at the first indicated key, insert count entries.
 */
static void
add_data(WT_SESSION *session, const char *uri, int first_key, int count)
{
    WT_CURSOR *cursor;
    int i;

    error_check(session->open_cursor(session, uri, NULL, NULL, &cursor));

    /* Insert a set of key/data pairs. */
    for (i = first_key; i < first_key + count; i++) {
        cursor->set_key(cursor, i);
        cursor->set_value(cursor, "value");
        error_check(cursor->insert(cursor));
    }

    error_check(cursor->close(cursor));
}

/*
 * Show all entries found in the uri.
 */
static void
show_data(WT_SESSION *session, const char *uri, int nentries, const char *comment)
{
    WT_CURSOR *cursor;
    int key, ret;
    const char *value;

    printf("%s: after %d %s\n", uri, nentries, comment);

    error_check(session->open_cursor(session, uri, NULL, NULL, &cursor));

    while ((ret = cursor->next(cursor)) == 0) {
        cursor->get_key(cursor, &key);
        cursor->get_value(cursor, &value);
        printf(" %d: %s\n", key, value);
    }

    /* We expect "not found" to end the iteration.  Anything else is an error. */
    if (ret == WT_NOTFOUND)
        ret = 0;

    error_check(ret);

    error_check(cursor->close(cursor));
}

/*
 * FIXME-WT-10567 At the moment, this example will not run on Windows, because it requires an
 * extension module to be loaded that is not yet built or tested on that platform.
 */
static bool
platform_supported(void)
{
#ifdef _WIN32
    return (false);
#else
    return (true);
#endif
}

/*
 * A storage source is a driver that controls access to an underlying object storage (e.g. a
 * specific cloud provider). The dir_store storage source stores objects in a directory named by the
 * bucket name, relative to the WiredTiger home directory.
 */
#define BUCKET_NAME "bucket"
#define BUILD_DIR "../../../"
#define STORAGE_SOURCE "dir_store"

#define LOCAL_URI "table:local_table"
#define TIERED_URI "table:tiered_table"

int
main(int argc, char *argv[])
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    const char *home;
    char buf[1024], config[1024];

    if (!platform_supported()) {
        fprintf(stderr, "**** Warning: %s is not supported on this platform\n", argv[0]);
        return (EXIT_SUCCESS);
    }

    home = example_setup(argc, argv);

    /*
     * Set up configuration for tiered storage. When tiered storage is configured, all tables
     * created will be tiered by default (that is, parts will be stored in object storage), though
     * tiered storage can be turned off on an individual basis.
     *
     * When configuring tiered storage with a cloud provider, one usually needs an access key. A
     * reference to this, an "authorization token", is usually provided in the configuration string.
     * This simple example uses "dir_store", a local directory based stand-in for a cloud provider,
     * and no explicit access keys are needed.
     *
     * For learning purposes, one can single step this program and watch files created in the
     * "bucket" subdirectory during the tiered checkpoint.
     */
    snprintf(config, sizeof(config),
      "create,"
      "tiered_storage=(bucket=%s,bucket_prefix=pfx-,local_retention=5,name=%s),"
      "extensions=(%s/ext/storage_sources/%s/libwiredtiger_%s.so)",
      BUCKET_NAME, STORAGE_SOURCE, BUILD_DIR, STORAGE_SOURCE, STORAGE_SOURCE);

    /* Create the home directory, and the bucket directory underneath it. */
    (void)snprintf(
      buf, sizeof(buf), "rm -rf %s && mkdir %s && mkdir %s/%s", home, home, home, BUCKET_NAME);
    error_check(system(buf));

    /* Configure the connection to use tiered storage. */
    error_check(wiredtiger_open(home, NULL, config, &conn));

    /* Open a session for the current thread's work. */
    error_check(conn->open_session(conn, NULL, NULL, &session));

    /* Create a table that lives locally. Tiered storage is disabled for this file. */
    error_check(session->create(
      session, LOCAL_URI, "key_format=i,value_format=S,tiered_storage=(name=none)"));

    /* Create a table using tiered storage. */
    error_check(session->create(session, TIERED_URI, "key_format=i,value_format=S"));

    /* Add entries to both tables. */
    add_data(session, LOCAL_URI, 0, N_ENTRIES);
    add_data(session, TIERED_URI, 0, N_ENTRIES);

    /*
     * Do a regular checkpoint. Checkpoints are usually done in their own thread with their own
     * session. Data is synchronized to local storage.
     */
    error_check(session->checkpoint(session, NULL));

    /* Add more entries to both tables. */
    add_data(session, LOCAL_URI, N_ENTRIES, N_ENTRIES);
    add_data(session, TIERED_URI, N_ENTRIES, N_ENTRIES);

    /*
     * Do a tiered checkpoint. For tiered tables, new data is flushed (synchronized) to the
     * configured tiered storage.
     */
    error_check(session->checkpoint(session, "flush_tier=(enabled)"));

    /* Show the data. */
    show_data(session, LOCAL_URI, 2 * N_ENTRIES, "items added");
    show_data(session, TIERED_URI, 2 * N_ENTRIES, "items added and flush_tier call");

    /* Add still more entries to both tables. */
    add_data(session, LOCAL_URI, 2 * N_ENTRIES, N_ENTRIES);
    add_data(session, TIERED_URI, 2 * N_ENTRIES, N_ENTRIES);

    /*
     * Another regular checkpoint. No new data is flushed to tiered storage.
     */
    error_check(session->checkpoint(session, NULL));

    /*
     * In the tiered table, some of the entries (up to key 2 * N_ENTRIES - 1), has been put into
     * tiered storage, and the rest is backed by a local file. However, all queries on the data look
     * the same.
     */
    show_data(session, LOCAL_URI, N_ENTRIES, "more items added");
    show_data(session, TIERED_URI, N_ENTRIES, "more items added that have not been flushed");

    /* Close all handles. */
    error_check(session->close(session, NULL));
    error_check(conn->close(conn, NULL));

    return (EXIT_SUCCESS);
}
