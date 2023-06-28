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

#include "format.h"

static void copy_file_into_directory(WT_SESSION *, const char *);
static void get_file_metadata(WT_SESSION *, const char **, const char **);
static void populate_table(WT_SESSION *);
static void verify_import(WT_SESSION *);

/*
 * Import directory initialize command, remove and create import directory, to place new database
 * connection.
 */
#define HOME_IMPORT_INIT_CMD "rm -rf %s/" IMPORT_DIR "&& mkdir %s/" IMPORT_DIR
#define IMPORT_DIR "IMPORT"
/*
 * The number of entries in the import table, primary use for validating contents after import.
 * There is no benefit to varying the number of entries in the import table.
 */
#define IMPORT_ENTRIES WT_THOUSAND
#define IMPORT_TABLE_CONFIG "key_format=i,value_format=i"
#define IMPORT_URI "table:import"
#define IMPORT_URI_FILE "file:import.wt"

/*
 * import --
 *     Periodically import table.
 */
WT_THREAD_RET
import(void *arg)
{
    WT_CONNECTION *conn, *import_conn;
    WT_SESSION *import_session, *session;
    size_t cmd_len;
    uint32_t import_value;
    u_int period;
    char buf[2048], *cmd;
    const char *file_config, *table_config;

    WT_UNUSED(arg);
    conn = g.wts_conn;
    file_config = table_config = NULL;
    import_value = 0;

    /*
     * Create a new database, primarily used for testing import.
     */
    cmd_len = strlen(g.home) * 2 + strlen(HOME_IMPORT_INIT_CMD) + 1;
    cmd = dmalloc(cmd_len);
    testutil_check(__wt_snprintf(cmd, cmd_len, HOME_IMPORT_INIT_CMD, g.home, g.home));
    testutil_checkfmt(system(cmd), "%s", "import directory creation failed");
    free(cmd);

    cmd_len = strlen(g.home) + strlen(IMPORT_DIR) + 10;
    cmd = dmalloc(cmd_len);
    testutil_check(__wt_snprintf(cmd, cmd_len, "%s/%s", g.home, IMPORT_DIR));
    /* Open a connection to the database, creating it if necessary. */
    create_database(cmd, &import_conn);
    free(cmd);

    /*
     * Open two sessions, one for test/format database and one for the import database.
     */
    testutil_check(import_conn->open_session(import_conn, NULL, NULL, &import_session));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Create new table and populate with data in import database. */
    testutil_checkfmt(
      import_session->create(import_session, IMPORT_URI, IMPORT_TABLE_CONFIG), "%s", IMPORT_URI);
    populate_table(import_session);

    /* Grab metadata information for table from import database connection. */
    get_file_metadata(import_session, &file_config, &table_config);

    while (!g.workers_finished) {
        /* Copy table into test/format database directory. */
        copy_file_into_directory(import_session, "import.wt");

        /* Perform import with either repair or file metadata. */
        import_value = mmrand(&g.extra_rnd, 0, 1);
        if (import_value == 0)
            testutil_check(__wt_snprintf(buf, sizeof(buf), "import=(enabled,repair=true)"));
        else
            testutil_check(__wt_snprintf(buf, sizeof(buf),
              "%s,import=(enabled,repair=false,file_metadata=(%s))", table_config, file_config));
        testutil_check(session->create(session, IMPORT_URI, buf));

        verify_import(session);

        /* Drop import table, so we can import the table again */
        testutil_drop(session, IMPORT_URI, NULL);

        period = mmrand(&g.extra_rnd, 1, 10);
        while (period > 0 && !g.workers_finished) {
            --period;
            __wt_sleep(1, 0);
        }
    }
    wts_close(&import_conn);
    testutil_check(session->close(session, NULL));
    return (WT_THREAD_RET_VALUE);
}

/*
 * verify_import --
 *     Verify all the values inside the imported table.
 */
static void
verify_import(WT_SESSION *session)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    int iteration, key, value;

    iteration = 0;
    testutil_check(session->open_cursor(session, IMPORT_URI, NULL, NULL, &cursor));

    while ((ret = cursor->next(cursor)) == 0) {
        testutil_check(cursor->get_key(cursor, &key));
        testutil_assert(key == iteration);
        testutil_check(cursor->get_value(cursor, &value));
        testutil_assert(value == iteration);
        iteration++;
    }
    testutil_assert(iteration == IMPORT_ENTRIES);
    scan_end_check(ret == WT_NOTFOUND);
    testutil_check(cursor->close(cursor));
}

/*
 * populate_table --
 *     Populate the import table with simple data.
 */
static void
populate_table(WT_SESSION *session)
{
    WT_CURSOR *cursor;
    int i;

    testutil_check(session->open_cursor(session, IMPORT_URI, NULL, NULL, &cursor));

    for (i = 0; i < IMPORT_ENTRIES; ++i) {
        cursor->set_key(cursor, i);
        cursor->set_value(cursor, i);
        testutil_check(cursor->insert(cursor));
    }
    testutil_check(cursor->close(cursor));
    testutil_check(session->checkpoint(session, NULL));
}

/*
 * get_file_metadata --
 *     Get import file and table metadata information from import database connection.
 */
static void
get_file_metadata(WT_SESSION *session, const char **file_config, const char **table_config)
{
    WT_CURSOR *metadata_cursor;

    testutil_check(session->open_cursor(session, "metadata:", NULL, NULL, &metadata_cursor));
    metadata_cursor->set_key(metadata_cursor, IMPORT_URI);
    testutil_check(metadata_cursor->search(metadata_cursor));
    metadata_cursor->get_value(metadata_cursor, table_config);

    metadata_cursor->set_key(metadata_cursor, IMPORT_URI_FILE);
    testutil_check(metadata_cursor->search(metadata_cursor));
    metadata_cursor->get_value(metadata_cursor, file_config);

    testutil_check(metadata_cursor->close(metadata_cursor));
}

/*
 * copy_file_into_directory --
 *     Copy a single file into the test/format directory.
 */
static void
copy_file_into_directory(WT_SESSION *session, const char *name)
{
    size_t buf_len;
    char to[64];

    buf_len = strlen(name) + 10;
    testutil_check(__wt_snprintf(to, buf_len, "../%s", name));
    testutil_check(__wt_copy_and_sync(session, name, to));
}
