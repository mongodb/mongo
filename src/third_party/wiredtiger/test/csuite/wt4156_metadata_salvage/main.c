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

#include <sys/wait.h>
#include <signal.h>

#define CORRUPT "file:zzz-corrupt.SS"
#define KEY "key"
#define VALUE "value,value,value"

#define SAVE "SAVE"

/*
 * NOTE: This assumes the default page size of 4096. If that changes these sizes need to change
 * along with it.
 */
#define APP_MD_SIZE 4096
#define APP_BUF_SIZE (3 * 1024)
#define APP_STR "Long app metadata intended to force a page per entry. "

static uint64_t data_val;
static const char *home;
static bool test_abort = false;
static bool test_out_of_sync = false;
static WT_SESSION *wt_session;

/*
 * handle_message --
 *     TODO: Add a comment describing this function.
 */
static int
handle_message(WT_EVENT_HANDLER *handler, WT_SESSION *session, int error, const char *message)
{
    (void)(handler);

    (void)fprintf(stderr, "%s: %s\n", message, session->strerror(session, error));
    if (test_abort) {
        fprintf(stderr, "Got unexpected error. Aborting\n");
        abort();
    }
    return (0);
}

static WT_EVENT_HANDLER event_handler = {handle_message, NULL, NULL, NULL};

typedef struct table_info {
    const char *name;
    const char *kvformat;
    bool verified;
} TABLE_INFO;

/*
 * byte_str --
 *     A byte-string version to find a sub-string. The metadata we read contains a lot of zeroes so
 *     we cannot use string-based functions.
 */
static uint8_t *
byte_str(uint8_t *buf, size_t bufsize, const char *str)
{
    size_t buflen, slen;
    uint8_t *end, *p, *s;
    int c;

    p = buf;
    end = buf + bufsize;
    s = NULL;
    c = (int)str[0];
    buflen = bufsize;
    slen = strlen(str);
    /*
     * Find the first character and then compare.
     */
    while ((s = memchr(p, c, buflen)) != NULL) {
        /*
         * If we don't have enough buffer left to compare we do not have a match.
         */
        buflen = (size_t)(end - s);
        if (buflen < slen)
            return (NULL);
        if (memcmp(s, str, slen) == 0)
            return (s);
        /*
         * This one didn't match, increment in the buffer and find the next one.
         */
        ++s;
        --buflen;
        p = s;
    }
    return (NULL);
}

/*
 * cursor_insert --
 *     Insert some data into a table.
 */
static void
cursor_insert(const char *uri, uint64_t i)
{
    WT_CURSOR *cursor;
    WT_ITEM vu;
    char keybuf[100], valuebuf[100];
    bool recno;

    memset(&vu, 0, sizeof(vu));

    /* Open a cursor. */
    testutil_check(wt_session->open_cursor(wt_session, uri, NULL, NULL, &cursor));
    /* Operations change based on the key/value formats. */
    recno = strcmp(cursor->key_format, "r") == 0;
    if (recno)
        cursor->set_key(cursor, i);
    else {
        testutil_check(__wt_snprintf(keybuf, sizeof(keybuf), "%s-%" PRIu64, KEY, i));
        cursor->set_key(cursor, keybuf);
    }
    strcpy(valuebuf, VALUE);
    cursor->set_value(cursor, valuebuf);
    testutil_check(cursor->insert(cursor));
    testutil_check(cursor->close(cursor));
}

/*
 * create_data --
 *     Create a table and insert a piece of data.
 */
static void
create_data(TABLE_INFO *t)
{
    size_t len;
    uint64_t i;
    char buf[APP_BUF_SIZE], cfg[APP_MD_SIZE];

    memset(buf, 0, sizeof(buf));
    memset(cfg, 0, sizeof(cfg));

    /*
     * Create an app-specific metadata string that fills most of page so that each table in the
     * metadata has its own page.
     */
    len = strlen(APP_STR);
    for (i = 0; i + len < APP_BUF_SIZE; i += len)
        testutil_check(__wt_snprintf(&buf[i], APP_BUF_SIZE - i, "%s", APP_STR));
    testutil_check(__wt_snprintf(cfg, sizeof(cfg), "%s,app_metadata=\"%s\"", t->kvformat, buf));
    testutil_check(wt_session->create(wt_session, t->name, cfg));
    data_val = 1;
    cursor_insert(t->name, data_val);
}

/*
 * corrupt_file --
 *     Corrupt the file by scribbling on the provided URI string.
 */
static void
corrupt_file(const char *file_name, const char *uri)
{
    struct stat sb;
    FILE *fp;
    size_t meta_size;
    long off;
    uint8_t *buf, *corrupt;
    char path[256];
    bool corrupted;

    /*
     * Open the file, read its contents. Find the string "corrupt" and modify one byte at that
     * offset. That will cause a checksum error when WiredTiger next reads it.
     */
    testutil_check(__wt_snprintf(path, sizeof(path), "%s/%s", home, file_name));
    if ((fp = fopen(path, "r+")) == NULL)
        testutil_die(errno, "fopen: %s", path);
    testutil_check(fstat(fileno(fp), &sb));
    meta_size = (size_t)sb.st_size;
    buf = dcalloc(meta_size, 1);
    if (fread(buf, 1, meta_size, fp) != meta_size)
        testutil_die(errno, "fread: %" WT_SIZET_FMT, meta_size);
    corrupted = false;
    /*
     * Corrupt all occurrences of the string in the file.
     */
    while ((corrupt = byte_str(buf, meta_size, uri)) != NULL) {
        corrupted = true;
        testutil_assert(*(char *)corrupt != 'X');
        *(char *)corrupt = 'X';
        off = (long)(corrupt - buf);
        if (fseek(fp, off, SEEK_SET) != 0)
            testutil_die(errno, "fseek: %ld", off);
        if (fwrite("X", 1, 1, fp) != 1)
            testutil_die(errno, "fwrite");
    }
    if (!corrupted)
        testutil_die(errno, "corrupt string did not occur");
    if (fclose(fp) != 0)
        testutil_die(errno, "fclose");
    free(buf);
}

/*
 * file_exists --
 *     Return if the file exists.
 */
static int
file_exists(const char *path)
{
    struct stat sb;

    return (stat(path, &sb) == 0);
}

/*
 * reset_verified --
 *     Reset the verified field in the table array.
 */
static void
reset_verified(TABLE_INFO *tables)
{
    TABLE_INFO *t;

    for (t = tables; t->name != NULL; t++)
        t->verified = false;
}

/*
 * verify_metadata --
 *     Verify all the tables expected are in the metadata. We expect all but the "corrupt" table
 *     name.
 */
static void
verify_metadata(WT_CONNECTION *conn, TABLE_INFO *tables)
{
    TABLE_INFO *t;
    WT_CURSOR *cursor;
    WT_DECL_RET;
    const char *kv;

    /*
     * Open a metadata cursor.
     */
    testutil_check(conn->open_session(conn, NULL, NULL, &wt_session));
    testutil_check(wt_session->open_cursor(wt_session, "metadata:", NULL, NULL, &cursor));
    reset_verified(tables);

    /*
     * We have to walk the cursor and walk the tables to match up that the expected tables are in
     * the metadata. It is not efficient, but the list of tables is small. Walk the cursor once and
     * the array of tables each time.
     */
    while ((ret = cursor->next(cursor)) == 0) {
        testutil_check(cursor->get_key(cursor, &kv));
        for (t = tables; t->name != NULL; t++) {
            if (strcmp(t->name, kv) == 0) {
                testutil_assert(t->verified == false);
                t->verified = true;
                break;
            }
        }
    }
    testutil_assert(ret == WT_NOTFOUND);
    testutil_check(cursor->close(cursor));
    /*
     * Any tables that were salvaged, make sure we can read the data. The corrupt table should never
     * be salvaged.
     */
    for (t = tables; t->name != NULL; t++) {
        if (strcmp(t->name, CORRUPT) == 0 && !test_out_of_sync)
            testutil_assert(t->verified == false);
        else if (t->verified != true)
            printf("%s not seen in metadata\n", t->name);
        else {
            if ((ret = wt_session->open_cursor(wt_session, t->name, NULL, NULL, &cursor)) != 0) {
                /*
                 * It is possible for the metadata file to contain a table entry and no associated
                 * file entry as WiredTiger didn't salvage the block associated with the file entry.
                 */
                if (ret == ENOENT)
                    continue;
                testutil_die(ret, "failed to open cursor on table");
            }
            while ((ret = cursor->next(cursor)) == 0) {
                testutil_check(cursor->get_value(cursor, &kv));
                testutil_assert(strcmp(kv, VALUE) == 0);
            }
            testutil_assert(ret == WT_NOTFOUND);
            testutil_check(cursor->close(cursor));
            printf("%s metadata salvaged and data verified\n", t->name);
        }
    }
}

/*
 * copy_database --
 *     Copy the database to the specified suffix. In addition, make a copy of the metadata and
 *     turtle files in that new directory.
 */
static void
copy_database(const char *sfx)
{
    WT_DECL_RET;
    char buf[1024];

    testutil_check(__wt_snprintf(buf, sizeof(buf),
      "rm -rf ./%s.%s; mkdir ./%s.%s; cp -p %s/* ./%s.%s", home, sfx, home, sfx, home, home, sfx));
    printf("copy: %s\n", buf);
    if ((ret = system(buf)) < 0)
        testutil_die(ret, "system: %s", buf);

    /*
     * Now, in the copied directory make a save copy of the metadata and turtle files to move around
     * and restore as needed during testing.
     */
    testutil_check(__wt_snprintf(buf, sizeof(buf), "cp -p %s.%s/%s %s.%s/%s.%s", home, sfx,
      WT_METADATA_TURTLE, home, sfx, WT_METADATA_TURTLE, SAVE));
    if ((ret = system(buf)) < 0)
        testutil_die(ret, "system: %s", buf);
    testutil_check(__wt_snprintf(buf, sizeof(buf), "cp -p %s.%s/%s %s.%s/%s.%s", home, sfx,
      WT_METAFILE, home, sfx, WT_METAFILE, SAVE));
    if ((ret = system(buf)) < 0)
        testutil_die(ret, "system: %s", buf);
}

/*
 * open_with_corruption --
 *     Call wiredtiger_open and expect a corruption error.
 */
static void
open_with_corruption(const char *sfx)
{
    WT_CONNECTION *conn;
    WT_DECL_RET;
    char buf[1024];

    /* We should not abort the test in the message handler. Set it here, don't inherit. */
    test_abort = false;
    if (sfx != NULL)
        testutil_check(__wt_snprintf(buf, sizeof(buf), "%s.%s", home, sfx));
    else
        testutil_check(__wt_snprintf(buf, sizeof(buf), "%s", home));

    /* Don't abort in the diagnostic builds on detecting corruption. */
    ret = wiredtiger_open(buf, &event_handler, "debug_mode=(corruption_abort=false)", &conn);

    /*
     * Not all out of sync combinations lead to corruption. We keep the previous checkpoint in the
     * file so some combinations of future or old turtle files and metadata files will succeed.
     */
    if (ret != WT_TRY_SALVAGE && ret != 0)
        fprintf(stderr, "OPEN_CORRUPT: wiredtiger_open returned %d\n", ret);
    testutil_assert(ret == WT_TRY_SALVAGE || ret == 0);

    if (ret == 0)
        testutil_check(conn->close(conn, NULL));
}

/*
 * open_with_salvage --
 *     TODO: Add a comment describing this function.
 */
static void
open_with_salvage(const char *sfx, TABLE_INFO *table_data)
{
    WT_CONNECTION *conn;
    char buf[1024];

    printf("=== wt_open with salvage ===\n");
    /*
     * Then call wiredtiger_open with the salvage configuration setting. That should succeed. We
     * should be able to then verify the contents of the metadata file.
     */
    test_abort = true;
    if (sfx != NULL)
        testutil_check(__wt_snprintf(buf, sizeof(buf), "%s.%s", home, sfx));
    else
        testutil_check(__wt_snprintf(buf, sizeof(buf), "%s", home));
    testutil_check(wiredtiger_open(buf, &event_handler, "salvage=true", &conn));
    testutil_assert(conn != NULL);
    if (sfx != NULL)
        testutil_check(__wt_snprintf(buf, sizeof(buf), "%s.%s/%s", home, sfx, WT_METAFILE_SLVG));
    else
        testutil_check(__wt_snprintf(buf, sizeof(buf), "%s/%s", home, WT_METAFILE_SLVG));
    testutil_assert(file_exists(buf));

    /*
     * Confirm we salvaged the metadata file by looking for the saved copy of the original metadata.
     */
    printf("verify with salvaged connection\n");
    verify_metadata(conn, &table_data[0]);
    testutil_check(conn->close(conn, NULL));
}

/*
 * open_normal --
 *     TODO: Add a comment describing this function.
 */
static void
open_normal(const char *sfx, TABLE_INFO *table_data)
{
    WT_CONNECTION *conn;
    char buf[1024];

    printf("=== wt_open normal ===\n");
    if (sfx != NULL)
        testutil_check(__wt_snprintf(buf, sizeof(buf), "%s.%s", home, sfx));
    else
        testutil_check(__wt_snprintf(buf, sizeof(buf), "%s", home));
    testutil_check(wiredtiger_open(buf, &event_handler, NULL, &conn));
    verify_metadata(conn, &table_data[0]);
    testutil_check(conn->close(conn, NULL));
}

/*
 * run_all_verification --
 *     TODO: Add a comment describing this function.
 */
static void
run_all_verification(const char *sfx, TABLE_INFO *t)
{
    open_with_corruption(sfx);
    open_with_salvage(sfx, t);
    open_normal(sfx, t);
}

/*
 * main --
 *     TODO: Add a comment describing this function.
 */
int
main(int argc, char *argv[])
{
    /*
     * Add a bunch of tables so that some of the metadata ends up on other pages and a good number
     * of tables are available after salvage completes.
     */
    TABLE_INFO table_data[] = {{"file:aaa-file.SS", "key_format=S,value_format=S", false},
      {"file:bbb-file.rS", "key_format=r,value_format=S", false},
      {"lsm:ccc-lsm.SS", "key_format=S,value_format=S", false},
      {"table:ddd-table.SS", "key_format=S,value_format=S", false},
      {"table:eee-table.rS", "key_format=r,value_format=S", false},
      {"file:fff-file.SS", "key_format=S,value_format=S", false},
      {"file:ggg-file.rS", "key_format=r,value_format=S", false},
      {"lsm:hhh-lsm.SS", "key_format=S,value_format=S", false},
      {"table:iii-table.SS", "key_format=S,value_format=S", false},
      {"table:jjj-table.rS", "key_format=r,value_format=S", false},
      {CORRUPT, "key_format=S,value_format=S", false}, {NULL, NULL, false}};
    TABLE_INFO *t;
    TEST_OPTS *opts, _opts;
    WT_DECL_RET;
    char buf[1024];

    /* Bypass this test for ASAN builds */
    if (testutil_is_flag_set("TESTUTIL_BYPASS_ASAN"))
        return (EXIT_SUCCESS);

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    testutil_check(testutil_parse_opts(argc, argv, opts));
    /*
     * Set a global. We use this everywhere.
     */
    home = opts->home;
    testutil_make_work_dir(home);

    testutil_check(wiredtiger_open(home, &event_handler, "create", &opts->conn));

    testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &wt_session));
    /*
     * Create a bunch of different tables.
     */
    for (t = table_data; t->name != NULL; t++)
        create_data(t);

    testutil_check(opts->conn->close(opts->conn, NULL));
    opts->conn = NULL;

    /*
     * Make copy of original directory.
     */
    copy_database(SAVE);
    /*
     * Damage/corrupt WiredTiger.wt.
     */
    printf("corrupt metadata\n");
    corrupt_file(WT_METAFILE, CORRUPT);
    testutil_check(__wt_snprintf(
      buf, sizeof(buf), "cp -p %s/WiredTiger.wt ./%s.%s/WiredTiger.wt.CORRUPT", home, home, SAVE));
    printf("copy: %s\n", buf);
    if ((ret = system(buf)) < 0)
        testutil_die(ret, "system: %s", buf);
    run_all_verification(NULL, &table_data[0]);

    /*
     * Damage/corrupt WiredTiger.turtle.
     */
    printf("corrupt turtle\n");
    corrupt_file(WT_METADATA_TURTLE, WT_METAFILE_URI);
    testutil_check(__wt_snprintf(buf, sizeof(buf),
      "cp -p %s/WiredTiger.turtle ./%s.%s/WiredTiger.turtle.CORRUPT", home, home, SAVE));
    printf("copy: %s\n", buf);
    if ((ret = system(buf)) < 0)
        testutil_die(ret, "system: %s", buf);
    run_all_verification(NULL, &table_data[0]);

    /* Remove saved copy of the original database directory. */
    testutil_check(__wt_snprintf(buf, sizeof(buf), "rm -rf %s.%s", home, SAVE));
    printf("cleanup and remove: %s\n", buf);
    if ((ret = system(buf)) < 0)
        testutil_die(ret, "system: %s", buf);

    /*
     * Cleanup from test. This will delete the database directory along with the core files left
     * there by our children.
     */
    testutil_cleanup(opts);

    return (EXIT_SUCCESS);
}
