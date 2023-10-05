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

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>

#include "wiredtiger.h"
extern "C" {
#include "test_util.h"
}

#include "model/kv_table.h"

/*
 * Command-line arguments.
 */
#define SHARED_PARSE_OPTIONS "h:p"

static char home[PATH_MAX]; /* Program working dir */
static TEST_OPTS *opts, _opts;

extern int __wt_optind;
extern char *__wt_optarg;

static void usage(void) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));

/*
 * Configuration.
 */
#define ENV_CONFIG                                            \
    "cache_size=20M,create,"                                  \
    "debug_mode=(table_logging=true,checkpoint_retention=5)," \
    "eviction_updates_target=20,eviction_updates_trigger=90," \
    "log=(enabled,file_max=10M,remove=true),session_max=100," \
    "statistics=(all),statistics_log=(wait=1,json,on_close)"

/*
 * wt_get --
 *     Read from WiredTiger.
 */
static model::data_value
wt_get(WT_SESSION *session, const char *uri, const model::data_value &key,
  model::timestamp_t timestamp = UINT64_MAX)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    const char *value;
    char cfg[64];

    value = nullptr;

    if (timestamp == 0)
        testutil_check(session->begin_transaction(session, nullptr));
    else {
        testutil_snprintf(cfg, sizeof(cfg), "read_timestamp=%" PRIx64, timestamp);
        testutil_check(session->begin_transaction(session, cfg));
    }
    testutil_check(session->open_cursor(session, uri, nullptr, nullptr, &cursor));

    model::set_wt_cursor_key(cursor, key);
    testutil_check_error_ok(ret = cursor->search(cursor), WT_NOTFOUND);
    if (ret == 0)
        testutil_check(cursor->get_value(cursor, &value));

    testutil_check(cursor->close(cursor));
    testutil_check(session->commit_transaction(session, nullptr));
    return ret == 0 ? model::data_value(value) : model::NONE;
}

/*
 * wt_insert --
 *     Write to WiredTiger.
 */
static int
wt_insert(WT_SESSION *session, const char *uri, const model::data_value &key,
  const model::data_value &value, model::timestamp_t timestamp = 0, bool overwrite = true)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    char cfg[64];

    testutil_check(session->begin_transaction(session, nullptr));
    testutil_check(session->open_cursor(
      session, uri, nullptr, overwrite ? nullptr : "overwrite=false", &cursor));

    model::set_wt_cursor_key(cursor, key);
    model::set_wt_cursor_value(cursor, value);
    testutil_check_error_ok(ret = cursor->insert(cursor), WT_DUPLICATE_KEY);

    testutil_check(cursor->close(cursor));
    if (timestamp == 0)
        cfg[0] = '\0';
    else
        testutil_snprintf(cfg, sizeof(cfg), "commit_timestamp=%" PRIx64, timestamp);
    testutil_check(session->commit_transaction(session, cfg));

    return ret;
}

/*
 * wt_remove --
 *     Delete from WiredTiger.
 */
static int
wt_remove(WT_SESSION *session, const char *uri, const model::data_value &key,
  model::timestamp_t timestamp = 0)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    char cfg[64];

    testutil_check(session->begin_transaction(session, nullptr));
    testutil_check(session->open_cursor(session, uri, nullptr, nullptr, &cursor));

    model::set_wt_cursor_key(cursor, key);
    testutil_check_error_ok(ret = cursor->remove(cursor), WT_NOTFOUND);

    testutil_check(cursor->close(cursor));
    if (timestamp == 0)
        cfg[0] = '\0';
    else
        testutil_snprintf(cfg, sizeof(cfg), "commit_timestamp=%" PRIx64, timestamp);
    testutil_check(session->commit_transaction(session, cfg));

    return ret;
}

/*
 * wt_update --
 *     Update a key in WiredTiger.
 */
static int
wt_update(WT_SESSION *session, const char *uri, const model::data_value &key,
  const model::data_value &value, model::timestamp_t timestamp = 0, bool overwrite = true)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    char cfg[64];

    testutil_check(session->begin_transaction(session, nullptr));
    testutil_check(session->open_cursor(
      session, uri, nullptr, overwrite ? nullptr : "overwrite=false", &cursor));

    model::set_wt_cursor_key(cursor, key);
    model::set_wt_cursor_value(cursor, value);
    testutil_check_error_ok(ret = cursor->update(cursor), WT_NOTFOUND);

    testutil_check(cursor->close(cursor));
    if (timestamp == 0)
        cfg[0] = '\0';
    else
        testutil_snprintf(cfg, sizeof(cfg), "commit_timestamp=%" PRIx64, timestamp);
    testutil_check(session->commit_transaction(session, cfg));

    return ret;
}

/*
 * wt_model_assert --
 *     Check that the key has the same value in the model as in the database.
 */
#define wt_model_assert(table, uri, key, ...) \
    testutil_assert(table.get(key, ##__VA_ARGS__) == wt_get(session, uri, key, ##__VA_ARGS__));

/*
 * wt_model_insert_both --
 *     Insert both into the model and the database.
 */
#define wt_model_insert_both(table, uri, key, value, ...)      \
    testutil_assert(table.insert(key, value, ##__VA_ARGS__) == \
      wt_insert(session, uri, key, value, ##__VA_ARGS__));

/*
 * wt_model_remove_both --
 *     Remove both from the model and from the database.
 */
#define wt_model_remove_both(table, uri, key, ...) \
    testutil_assert(                               \
      table.remove(key, ##__VA_ARGS__) == wt_remove(session, uri, key, ##__VA_ARGS__));

/*
 * wt_model_update_both --
 *     Update both in the model and in the database.
 */
#define wt_model_update_both(table, uri, key, value, ...)      \
    testutil_assert(table.update(key, value, ##__VA_ARGS__) == \
      wt_update(session, uri, key, value, ##__VA_ARGS__));

/*
 * test_data_value --
 *     Data value unit tests.
 */
static void
test_data_value(void)
{
    const model::data_value key1("Key 1");
    const model::data_value key2("Key 2");

    testutil_assert(strcmp(key1.wt_type(), "S") == 0);
    testutil_assert(key1 == model::data_value("Key 1"));
    testutil_assert(key2 == model::data_value("Key 2"));

    std::ostringstream ss_key1;
    ss_key1 << key1;
    testutil_assert(ss_key1.str() == "Key 1");

    testutil_assert(key1 < key2);
    testutil_assert(key2 > key1);
    testutil_assert(!(key1 > key2));
    testutil_assert(!(key2 < key1));

    testutil_assert(key1 <= key2);
    testutil_assert(key2 >= key1);
    testutil_assert(key1 == key1);
    testutil_assert(!(key1 >= key2));
    testutil_assert(!(key2 <= key1));
    testutil_assert(!(key1 != key1));

    /* NONE. */

    testutil_assert(model::NONE.none());
    testutil_assert(!key1.none());

    testutil_assert(model::NONE == model::data_value::create_none());
    testutil_assert(key1 != model::NONE);
    testutil_assert(model::NONE < key1);
    testutil_assert(model::NONE <= key1);

    /* Non-string keys, WiredTiger types "q" and "Q". */

    const model::data_value key1_q(static_cast<int64_t>(10));
    const model::data_value key2_q(static_cast<int64_t>(20));

    const model::data_value key1_Q(static_cast<uint64_t>(10));
    const model::data_value key2_Q(static_cast<uint64_t>(20));

    testutil_assert(strcmp(key1_q.wt_type(), "q") == 0);
    testutil_assert(key1_q == model::data_value(static_cast<int64_t>(10)));

    testutil_assert(strcmp(key1_Q.wt_type(), "Q") == 0);
    testutil_assert(key1_Q == model::data_value(static_cast<uint64_t>(10)));

    std::ostringstream ss_key1_q;
    ss_key1_q << key1_q;
    testutil_assert(ss_key1_q.str() == "10");

    std::ostringstream ss_key1_Q;
    ss_key1_Q << key1_Q;
    testutil_assert(ss_key1_Q.str() == "10");

    testutil_assert(key1_q != key1_Q);
    testutil_assert(key2_q != key2_Q);

    testutil_assert(key1_q < key2_q);
    testutil_assert(key2_q > key1_q);

    testutil_assert(key1_Q < key2_Q);
    testutil_assert(key2_Q > key1_Q);
}

/*
 * test_model_basic --
 *     The basic test of the model.
 */
static void
test_model_basic(void)
{
    model::kv_table table("table");

    /* Keys. */
    model::data_value key1("Key 1");
    model::data_value key2("Key 2");
    model::data_value keyX("Key X");

    /* Values. */
    model::data_value value1("Value 1");
    model::data_value value2("Value 2");
    model::data_value value3("Value 3");
    model::data_value value4("Value 4");

    /* Populate the table with a few values and check that we get the expected results. */
    testutil_check(table.insert(key1, value1, 10));
    testutil_check(table.insert(key1, value2, 20));
    testutil_check(table.remove(key1, 30));
    testutil_check(table.insert(key1, value4, 40));

    testutil_assert(table.get(key1, 10) == value1);
    testutil_assert(table.get(key1, 20) == value2);
    testutil_assert(table.get(key1, 30) == model::NONE);
    testutil_assert(table.get(key1, 40) == value4);

    testutil_assert(table.get(key1, 5) == model::NONE);
    testutil_assert(table.get(key1, 15) == value1);
    testutil_assert(table.get(key1, 25) == value2);
    testutil_assert(table.get(key1, 35) == model::NONE);
    testutil_assert(table.get(key1, 45) == value4);
    testutil_assert(table.get(key1) == value4);

    /* Test globally visible (non-timestamped) updates. */
    testutil_check(table.insert(key2, value1));
    testutil_assert(table.get(key2, 0) == value1);
    testutil_assert(table.get(key2, 10) == value1);
    testutil_assert(table.get(key2) == value1);

    testutil_check(table.remove(key2));
    testutil_assert(table.get(key2) == model::NONE);

    /* Try a missing key. */
    testutil_assert(table.get(keyX) == model::NONE);

    testutil_assert(table.remove(keyX) == WT_NOTFOUND);
    testutil_assert(table.get(keyX) == model::NONE);

    /* Try timestamped updates to the second key. */
    testutil_check(table.insert(key2, value3, 30));
    testutil_assert(table.get(key2, 5) == model::NONE);
    testutil_assert(table.get(key2, 35) == value3);
    testutil_assert(table.get(key2) == value3);

    /* Test multiple inserts with the same timestamp. */
    testutil_check(table.insert(key1, value1, 50));
    testutil_check(table.insert(key1, value2, 50));
    testutil_check(table.insert(key1, value3, 50));
    testutil_check(table.insert(key1, value4, 60));
    testutil_assert(table.get(key1, 50) == value3);
    testutil_assert(table.get(key1, 55) == value3);
    testutil_assert(table.get(key1) == value4);

    testutil_assert(!table.contains_any(key1, value1, 5));
    testutil_assert(!table.contains_any(key1, value2, 5));
    testutil_assert(!table.contains_any(key1, value3, 5));
    testutil_assert(!table.contains_any(key1, value4, 5));

    testutil_assert(table.contains_any(key1, value1, 50));
    testutil_assert(table.contains_any(key1, value2, 50));
    testutil_assert(table.contains_any(key1, value3, 50));
    testutil_assert(!table.contains_any(key1, value4, 50));

    testutil_assert(table.contains_any(key1, value1, 55));
    testutil_assert(table.contains_any(key1, value2, 55));
    testutil_assert(table.contains_any(key1, value3, 55));
    testutil_assert(!table.contains_any(key1, value4, 55));

    testutil_assert(!table.contains_any(key1, value1, 60));
    testutil_assert(!table.contains_any(key1, value2, 60));
    testutil_assert(!table.contains_any(key1, value3, 60));
    testutil_assert(table.contains_any(key1, value4, 60));

    /* Test insert without overwrite. */
    testutil_assert(table.insert(key1, value1, 60, false) == WT_DUPLICATE_KEY);
    testutil_assert(table.insert(key1, value1, 65, false) == WT_DUPLICATE_KEY);
    testutil_check(table.remove(key1, 65));
    testutil_check(table.insert(key1, value1, 70, false));

    /* Test updates. */
    testutil_check(table.update(key1, value2, 70));
    testutil_check(table.update(key1, value3, 75));
    testutil_assert(table.get(key1, 70) == value2);
    testutil_assert(table.get(key1, 75) == value3);
    testutil_check(table.remove(key1, 80));
    testutil_assert(table.update(key1, value1, 80, false) == WT_NOTFOUND);
    testutil_assert(table.update(key1, value1, 85, false) == WT_NOTFOUND);
}

/*
 * test_model_basic_wt --
 *     The basic test of the model - with WiredTiger.
 */
static void
test_model_basic_wt(void)
{
    /* Table. */
    model::kv_table table("table");

    /* Keys. */
    model::data_value key1("Key 1");
    model::data_value key2("Key 2");
    model::data_value keyX("Key X");

    /* Values. */
    model::data_value value1("Value 1");
    model::data_value value2("Value 2");
    model::data_value value3("Value 3");
    model::data_value value4("Value 4");

    /* Create the test's home directory and database. */
    WT_CONNECTION *conn;
    WT_SESSION *session;
    const char *uri = "table:table";

    testutil_recreate_dir(home);
    testutil_wiredtiger_open(opts, home, ENV_CONFIG, nullptr, &conn, false, false);
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session));
    testutil_check(
      session->create(session, uri, "key_format=S,value_format=S,log=(enabled=false)"));

    /* Populate the table with a few values and check that we get the expected results. */
    wt_model_insert_both(table, uri, key1, value1, 10);
    wt_model_insert_both(table, uri, key1, value2, 20);
    wt_model_remove_both(table, uri, key1, 30);
    wt_model_insert_both(table, uri, key1, value4, 40);

    wt_model_assert(table, uri, key1, 10);
    wt_model_assert(table, uri, key1, 20);
    wt_model_assert(table, uri, key1, 30);
    wt_model_assert(table, uri, key1, 40);

    wt_model_assert(table, uri, key1, 5);
    wt_model_assert(table, uri, key1, 15);
    wt_model_assert(table, uri, key1, 25);
    wt_model_assert(table, uri, key1, 35);
    wt_model_assert(table, uri, key1, 45);
    wt_model_assert(table, uri, key1);

    testutil_assert(table.verify_noexcept(conn));

    /* Test globally visible (non-timestamped) updates. */
    wt_model_insert_both(table, uri, key2, value1);
    wt_model_assert(table, uri, key2, 0);
    wt_model_assert(table, uri, key2, 10);
    wt_model_assert(table, uri, key2);

    wt_model_remove_both(table, uri, key2);
    wt_model_assert(table, uri, key2);

    /* Try a missing key. */
    wt_model_assert(table, uri, keyX);

    wt_model_remove_both(table, uri, keyX);
    wt_model_assert(table, uri, keyX);

    /* Try timestamped updates to the second key. */
    wt_model_insert_both(table, uri, key2, value3, 30);

    wt_model_assert(table, uri, key2, 5);
    wt_model_assert(table, uri, key2, 35);
    wt_model_assert(table, uri, key2);

    /* Test multiple inserts with the same timestamp. */
    wt_model_insert_both(table, uri, key1, value1, 50);
    wt_model_insert_both(table, uri, key1, value2, 50);
    wt_model_insert_both(table, uri, key1, value3, 50);
    wt_model_insert_both(table, uri, key1, value4, 60);

    wt_model_assert(table, uri, key1, 50);
    wt_model_assert(table, uri, key1, 55);
    wt_model_assert(table, uri, key1);

    testutil_assert(table.verify_noexcept(conn));

    /* Test insert without overwrite. */
    wt_model_insert_both(table, uri, key1, value1, 60, false);
    wt_model_insert_both(table, uri, key1, value1, 65, false);
    wt_model_remove_both(table, uri, key1, 65);
    wt_model_insert_both(table, uri, key1, value1, 70, false);

    /* Test updates. */
    wt_model_update_both(table, uri, key1, value2, 70);
    wt_model_update_both(table, uri, key1, value3, 75);
    wt_model_assert(table, uri, key1, 70);
    wt_model_assert(table, uri, key1, 75);
    wt_model_remove_both(table, uri, key1, 80);
    wt_model_update_both(table, uri, key1, value1, 80, false);
    wt_model_update_both(table, uri, key1, value1, 85, false);

    testutil_assert(table.verify_noexcept(conn));

    /* Now try to get the verification to fail. */
    testutil_check(table.remove(key2, 1000));
    testutil_assert(!table.verify_noexcept(conn));

    /* Clean up. */
    testutil_check(session->close(session, nullptr));
    testutil_check(conn->close(conn, nullptr));
}

/*
 * usage --
 *     Print usage help for the program.
 */
static void
usage(void)
{
    fprintf(stderr, "usage: %s%s\n", progname, opts->usage);
    exit(EXIT_FAILURE);
}

/*
 * main --
 *     The main entry point for the test.
 */
int
main(int argc, char *argv[])
{
    int ch;

    (void)testutil_set_progname(argv);

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));

    /*
     * Parse the command-line arguments.
     */
    testutil_parse_begin_opt(argc, argv, SHARED_PARSE_OPTIONS, opts);
    while ((ch = __wt_getopt(progname, argc, argv, SHARED_PARSE_OPTIONS)) != EOF)
        switch (ch) {
        default:
            if (testutil_parse_single_opt(opts, ch) != 0)
                usage();
        }
    argc -= __wt_optind;
    if (argc != 0)
        usage();

    testutil_parse_end_opt(opts);
    testutil_work_dir_from_path(home, sizeof(home), opts->home);

    /*
     * Tests.
     */
    test_data_value();
    test_model_basic();
    test_model_basic_wt();

    /*
     * Clean up.
     */
    /* Delete the work directory. */
    if (!opts->preserve)
        testutil_remove(home);

    testutil_cleanup(opts);
    return EXIT_SUCCESS;
}
