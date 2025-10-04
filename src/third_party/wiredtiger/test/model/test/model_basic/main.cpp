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

#include "model/driver/debug_log_parser.h"
#include "model/test/util.h"
#include "model/test/wiredtiger_util.h"
#include "model/kv_database.h"

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
#define ENV_CONFIG                                             \
    "cache_size=20M,create,"                                   \
    "debug_mode=(table_logging=true,checkpoint_retention=5),"  \
    "eviction_updates_target=20,eviction_updates_trigger=90,"  \
    "log=(enabled,file_max=10M,remove=false),session_max=100," \
    "statistics=(all),statistics_log=(wait=1,json,on_close)"

/* Byte values. */
const model::data_value byte1((uint64_t)1);
const model::data_value byte2((uint64_t)2);
const model::data_value byte3((uint64_t)3);
const model::data_value byte4((uint64_t)4);
const model::data_value byte5((uint64_t)5);

/* Keys. */
const model::data_value key1("Key 1");
const model::data_value key2("Key 2");
const model::data_value key3("Key 3");
const model::data_value key4("Key 4");
const model::data_value key5("Key 5");
const model::data_value keyX("Key X");

/* Column keys. */
const model::data_value recno1((uint64_t)1);
const model::data_value recno2((uint64_t)2);
const model::data_value recno3((uint64_t)3);
const model::data_value recno4((uint64_t)4);
const model::data_value recno5((uint64_t)5);
const model::data_value recnoX((uint64_t)10);

/* Values. */
const model::data_value value1("Value 1");
const model::data_value value2("Value 2");
const model::data_value value3("Value 3");
const model::data_value value4("Value 4");
const model::data_value value5("Value 5");

/*
 * test_data_value --
 *     Data value unit tests.
 */
static void
test_data_value(void)
{
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
    model::kv_database database;
    model::kv_table_ptr table = database.create_table("table");

    /* Populate the table with a few values and check that we get the expected results. */
    testutil_check(table->insert(key1, value1, 10));
    testutil_check(table->insert(key1, value2, 20));
    testutil_check(table->remove(key1, 30));
    testutil_check(table->insert(key1, value4, 40));

    testutil_assert(table->get(key1, 10) == value1);
    testutil_assert(table->get(key1, 20) == value2);
    testutil_assert(table->get(key1, 30) == model::NONE);
    testutil_assert(table->get(key1, 40) == value4);

    testutil_assert(table->get(key1, 5) == model::NONE);
    testutil_assert(table->get(key1, 15) == value1);
    testutil_assert(table->get(key1, 25) == value2);
    testutil_assert(table->get(key1, 35) == model::NONE);
    testutil_assert(table->get(key1, 45) == value4);
    testutil_assert(table->get(key1) == value4);

    /* Test globally visible (non-timestamped) updates. */
    testutil_check(table->insert(key2, value1));
    testutil_assert(table->get(key2, 0) == value1);
    testutil_assert(table->get(key2, 10) == value1);
    testutil_assert(table->get(key2) == value1);

    testutil_check(table->remove(key2));
    testutil_assert(table->get(key2) == model::NONE);

    /* Try a missing key. */
    testutil_assert(table->get(keyX) == model::NONE);

    testutil_assert(table->remove(keyX) == WT_NOTFOUND);
    testutil_assert(table->get(keyX) == model::NONE);

    /* Try timestamped updates to the second key. */
    testutil_check(table->insert(key2, value3, 30));
    testutil_assert(table->get(key2, 5) == model::NONE);
    testutil_assert(table->get(key2, 35) == value3);
    testutil_assert(table->get(key2) == value3);

    /* Test multiple inserts with the same timestamp. */
    testutil_check(table->insert(key1, value1, 50));
    testutil_check(table->insert(key1, value2, 50));
    testutil_check(table->insert(key1, value3, 50));
    testutil_check(table->insert(key1, value4, 60));
    testutil_assert(table->get(key1, 50) == value3);
    testutil_assert(table->get(key1, 55) == value3);
    testutil_assert(table->get(key1) == value4);

    testutil_assert(!table->contains_any(key1, value1, 5));
    testutil_assert(!table->contains_any(key1, value2, 5));
    testutil_assert(!table->contains_any(key1, value3, 5));
    testutil_assert(!table->contains_any(key1, value4, 5));

    testutil_assert(table->contains_any(key1, value1, 50));
    testutil_assert(table->contains_any(key1, value2, 50));
    testutil_assert(table->contains_any(key1, value3, 50));
    testutil_assert(!table->contains_any(key1, value4, 50));

    testutil_assert(table->contains_any(key1, value1, 55));
    testutil_assert(table->contains_any(key1, value2, 55));
    testutil_assert(table->contains_any(key1, value3, 55));
    testutil_assert(!table->contains_any(key1, value4, 55));

    testutil_assert(!table->contains_any(key1, value1, 60));
    testutil_assert(!table->contains_any(key1, value2, 60));
    testutil_assert(!table->contains_any(key1, value3, 60));
    testutil_assert(table->contains_any(key1, value4, 60));

    /* Test insert without overwrite. */
    testutil_assert(table->insert(key1, value1, 60, false) == WT_DUPLICATE_KEY);
    testutil_assert(table->insert(key1, value1, 65, false) == WT_DUPLICATE_KEY);
    testutil_check(table->remove(key1, 65));
    testutil_check(table->insert(key1, value1, 70, false));

    /* Test updates. */
    testutil_check(table->update(key1, value2, 70));
    testutil_check(table->update(key1, value3, 75));
    testutil_assert(table->get(key1, 70) == value2);
    testutil_assert(table->get(key1, 75) == value3);
    testutil_check(table->remove(key1, 80));
    testutil_assert(table->update(key1, value1, 80, false) == WT_NOTFOUND);
    testutil_assert(table->update(key1, value1, 85, false) == WT_NOTFOUND);
}

/*
 * test_model_basic_wt --
 *     The basic test of the model - with WiredTiger.
 */
static void
test_model_basic_wt(void)
{
    model::kv_database database;
    model::kv_table_ptr table = database.create_table("table");

    /* Create the test's home directory and database. */
    WT_CONNECTION *conn;
    WT_SESSION *session;
    const char *uri = "table:table";

    std::string test_home = std::string(home) + DIR_DELIM_STR + "basic";
    testutil_recreate_dir(test_home.c_str());
    testutil_wiredtiger_open(opts, test_home.c_str(), ENV_CONFIG, nullptr, &conn, false, false);
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

    testutil_assert(table->verify_noexcept(conn));

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

    testutil_assert(table->verify_noexcept(conn));

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

    /* Verify. */
    testutil_assert(table->verify_noexcept(conn));

    /* Now try to get the verification to fail. */
    testutil_check(table->remove(key2, 1000));
    testutil_assert(!table->verify_noexcept(conn));

    /* Clean up. */
    testutil_check(session->close(session, nullptr));
    testutil_check(conn->close(conn, nullptr));

    /* Verify using the debug log. */
    verify_using_debug_log(opts, test_home.c_str(), true);
}

/*
 * test_model_basic_column_wt --
 *     The basic test of the model - with columns and with WiredTiger.
 */
static void
test_model_basic_column_wt(void)
{
    model::kv_database database;

    model::kv_table_config table_config;
    table_config.type = model::kv_table_type::column;
    model::kv_table_ptr table = database.create_table("table", table_config);

    /* Create the test's home directory and database. */
    WT_CONNECTION *conn;
    WT_SESSION *session;
    const char *uri = "table:table";

    std::string test_home = std::string(home) + DIR_DELIM_STR + "basic-column";
    testutil_recreate_dir(test_home.c_str());
    testutil_wiredtiger_open(opts, test_home.c_str(), ENV_CONFIG, nullptr, &conn, false, false);
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session));
    testutil_check(
      session->create(session, uri, "key_format=r,value_format=S,log=(enabled=false)"));

    /* Populate the table with a few values and check that we get the expected results. */
    wt_model_insert_both(table, uri, recno1, value1, 10);
    wt_model_insert_both(table, uri, recno1, value2, 20);
    wt_model_remove_both(table, uri, recno1, 30);
    wt_model_insert_both(table, uri, recno1, value4, 40);

    wt_model_assert(table, uri, recno1, 10);
    wt_model_assert(table, uri, recno1, 20);
    wt_model_assert(table, uri, recno1, 30);
    wt_model_assert(table, uri, recno1, 40);

    wt_model_assert(table, uri, recno1, 5);
    wt_model_assert(table, uri, recno1, 15);
    wt_model_assert(table, uri, recno1, 25);
    wt_model_assert(table, uri, recno1, 35);
    wt_model_assert(table, uri, recno1, 45);
    wt_model_assert(table, uri, recno1);

    testutil_assert(table->verify_noexcept(conn));

    /* Test globally visible (non-timestamped) updates. */
    wt_model_insert_both(table, uri, recno2, value1);
    wt_model_assert(table, uri, recno2, 0);
    wt_model_assert(table, uri, recno2, 10);
    wt_model_assert(table, uri, recno2);

    wt_model_remove_both(table, uri, recno2);
    wt_model_assert(table, uri, recno2);

    /* Try a missing recno. */
    wt_model_assert(table, uri, recnoX);

    wt_model_remove_both(table, uri, recnoX);
    wt_model_assert(table, uri, recnoX);

    /* Try timestamped updates to the second recno. */
    wt_model_insert_both(table, uri, recno2, value3, 30);

    wt_model_assert(table, uri, recno2, 5);
    wt_model_assert(table, uri, recno2, 35);
    wt_model_assert(table, uri, recno2);

    /* Test multiple inserts with the same timestamp. */
    wt_model_insert_both(table, uri, recno1, value1, 50);
    wt_model_insert_both(table, uri, recno1, value2, 50);
    wt_model_insert_both(table, uri, recno1, value3, 50);
    wt_model_insert_both(table, uri, recno1, value4, 60);

    wt_model_assert(table, uri, recno1, 50);
    wt_model_assert(table, uri, recno1, 55);
    wt_model_assert(table, uri, recno1);

    testutil_assert(table->verify_noexcept(conn));

    /* Test insert without overwrite. */
    wt_model_insert_both(table, uri, recno1, value1, 60, false);
    wt_model_insert_both(table, uri, recno1, value1, 65, false);
    wt_model_remove_both(table, uri, recno1, 65);
    wt_model_insert_both(table, uri, recno1, value1, 70, false);

    /* Test updates. */
    wt_model_update_both(table, uri, recno1, value2, 70);
    wt_model_update_both(table, uri, recno1, value3, 75);
    wt_model_assert(table, uri, recno1, 70);
    wt_model_assert(table, uri, recno1, 75);
    wt_model_remove_both(table, uri, recno1, 80);
    wt_model_update_both(table, uri, recno1, value1, 80, false);
    wt_model_update_both(table, uri, recno1, value1, 85, false);

    /* Verify. */
    testutil_assert(table->verify_noexcept(conn));

    /* Now try to get the verification to fail. */
    testutil_check(table->remove(recno2, 1000));
    testutil_assert(!table->verify_noexcept(conn));

    /* Clean up. */
    testutil_check(session->close(session, nullptr));
    testutil_check(conn->close(conn, nullptr));

    /* Verify using the debug log. */
    verify_using_debug_log(opts, test_home.c_str(), true);
}

/*
 * test_model_basic_column_fix_wt --
 *     The basic test of the model - with FLCS and with WiredTiger.
 */
static void
test_model_basic_column_fix_wt(void)
{
    model::kv_database database;

    model::kv_table_config table_config;
    table_config.type = model::kv_table_type::column_fix;
    model::kv_table_ptr table = database.create_table("table", table_config);

    /* Create the test's home directory and database. */
    WT_CONNECTION *conn;
    WT_SESSION *session;
    const char *uri = "table:table";

    std::string test_home = std::string(home) + DIR_DELIM_STR + "basic-column-fix";
    testutil_recreate_dir(test_home.c_str());
    testutil_wiredtiger_open(opts, test_home.c_str(), ENV_CONFIG, nullptr, &conn, false, false);
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session));
    testutil_check(
      session->create(session, uri, "key_format=r,value_format=8t,log=(enabled=false)"));

    /* Populate the table with a few values and check that we get the expected results. */
    wt_model_insert_both(table, uri, recno2, byte1, 10);
    wt_model_insert_both(table, uri, recno2, byte2, 20);
    wt_model_remove_both(table, uri, recno2, 30);
    wt_model_insert_both(table, uri, recno2, byte4, 40);

    wt_model_assert(table, uri, recno2, 10);
    wt_model_assert(table, uri, recno2, 20);
    wt_model_assert(table, uri, recno2, 30);
    wt_model_assert(table, uri, recno2, 40);

    wt_model_assert(table, uri, recno2, 5);
    wt_model_assert(table, uri, recno2, 15);
    wt_model_assert(table, uri, recno2, 25);
    wt_model_assert(table, uri, recno2, 35);
    wt_model_assert(table, uri, recno2, 45);
    wt_model_assert(table, uri, recno2);

    testutil_assert(table->verify_noexcept(conn));

    /* Test globally visible (non-timestamped) updates. */
    wt_model_insert_both(table, uri, recno3, byte1);
    wt_model_assert(table, uri, recno3, 0);
    wt_model_assert(table, uri, recno3, 10);
    wt_model_assert(table, uri, recno3);

    wt_model_remove_both(table, uri, recno3);
    wt_model_assert(table, uri, recno3);

    /* Try missing recnos. */
    wt_model_assert(table, uri, recno1);
    wt_model_assert(table, uri, recno4);

    wt_model_remove_both(table, uri, recno4);
    wt_model_assert(table, uri, recno4);

    /* Try a missing recno, if a higher recno exists. */
    wt_model_insert_both(table, uri, recno5, byte5, 30);
    wt_model_assert(table, uri, recno4);

    /* Try timestamped updates on the recno that had only non-timestamped updates so far. */
    wt_model_insert_both(table, uri, recno3, byte3, 30);

    wt_model_assert(table, uri, recno3, 5);
    wt_model_assert(table, uri, recno3, 35);
    wt_model_assert(table, uri, recno3);

    /* Test multiple inserts with the same timestamp. */
    wt_model_insert_both(table, uri, recno2, byte1, 50);
    wt_model_insert_both(table, uri, recno2, byte2, 50);
    wt_model_insert_both(table, uri, recno2, byte3, 50);
    wt_model_insert_both(table, uri, recno2, byte4, 60);

    wt_model_assert(table, uri, recno2, 50);
    wt_model_assert(table, uri, recno2, 55);
    wt_model_assert(table, uri, recno2);

    /* Test insert without overwrite. */
    wt_model_insert_both(table, uri, recno2, byte1, 60, false);
    wt_model_insert_both(table, uri, recno2, byte1, 65, false);
    wt_model_remove_both(table, uri, recno2, 65);
    wt_model_insert_both(table, uri, recno2, byte1, 70, false);

    /* Test updates. */
    wt_model_update_both(table, uri, recno2, byte2, 70);
    wt_model_update_both(table, uri, recno2, byte3, 75);
    wt_model_assert(table, uri, recno2, 70);
    wt_model_assert(table, uri, recno2, 75);
    wt_model_remove_both(table, uri, recno2, 80);
    wt_model_update_both(table, uri, recno2, byte1, 80, false);
    wt_model_update_both(table, uri, recno2, byte1, 85, false);

    /* Verify. */
    testutil_assert(table->verify_noexcept(conn));

    /* Now try to get the verification to fail. */
    testutil_check(table->remove(recno2, 1000));
    testutil_assert(!table->verify_noexcept(conn));

    /* Clean up. */
    testutil_check(session->close(session, nullptr));
    testutil_check(conn->close(conn, nullptr));

    /* Verify using the debug log. */
    verify_using_debug_log(opts, test_home.c_str(), true);
}

/*
 * test_model_logged --
 *     Test tables that use logging.
 */
static void
test_model_basic_logged(void)
{
    model::kv_database database;

    model::kv_table_config table_config;
    table_config.log_enabled = true;
    model::kv_table_ptr table = database.create_table("table", table_config);

    /* Populate the table with a few values and check that timestamps are ignored. */
    testutil_check(table->insert(key1, value1, 10));
    testutil_check(table->insert(key1, value2, 20));
    testutil_check(table->remove(key1, 30));
    testutil_check(table->insert(key1, value4, 40));

    testutil_assert(table->get(key1, 5) == value4);
    testutil_assert(table->get(key1, 10) == value4);
    testutil_assert(table->get(key1, 20) == value4);
    testutil_assert(table->get(key1, 30) == value4);
    testutil_assert(table->get(key1, 40) == value4);
    testutil_assert(table->get(key1) == value4);

    /* Test non-timestamped updates. */
    testutil_check(table->insert(key2, value1));
    testutil_assert(table->get(key2, 0) == value1);
    testutil_assert(table->get(key2, 10) == value1);
    testutil_assert(table->get(key2) == value1);

    testutil_check(table->remove(key2));
    testutil_assert(table->get(key2) == model::NONE);

    /* Try a missing key. */
    testutil_assert(table->get(keyX) == model::NONE);

    testutil_assert(table->remove(keyX) == WT_NOTFOUND);
    testutil_assert(table->get(keyX) == model::NONE);

    /* Try timestamped updates to the second key. */
    testutil_check(table->insert(key2, value3, 30));
    testutil_assert(table->get(key2, 5) == value3);
    testutil_assert(table->get(key2, 35) == value3);
    testutil_assert(table->get(key2) == value3);

    /* Try non-timestamped updates to the same key. */
    testutil_check(table->insert(key2, value2));
    testutil_assert(table->get(key2) == value2);

    /* Test multiple inserts. */
    testutil_check(table->insert(key1, value1, 50));
    testutil_check(table->insert(key1, value2, 50));
    testutil_check(table->insert(key1, value3, 50));

    testutil_assert(table->contains_any(key1, value1));
    testutil_assert(table->contains_any(key1, value2));
    testutil_assert(table->contains_any(key1, value3));
    testutil_assert(!table->contains_any(key1, value5));

    /* Test insert without overwrite. */
    testutil_assert(table->insert(key1, value1, 60, false) == WT_DUPLICATE_KEY);
    testutil_assert(table->insert(key1, value1, 65, false) == WT_DUPLICATE_KEY);
    testutil_check(table->remove(key1, 65));
    testutil_check(table->insert(key1, value1, 70, false));

    /* Test updates. */
    testutil_check(table->update(key1, value2, 70));
    testutil_assert(table->get(key1, 70) == value2);
    testutil_check(table->remove(key1, 80));
    testutil_assert(table->update(key1, value1, 80, false) == WT_NOTFOUND);
    testutil_assert(table->update(key1, value1, 85, false) == WT_NOTFOUND);
}

/*
 * test_model_logged_wt --
 *     Test tables that use logging - with WiredTiger.
 */
static void
test_model_basic_logged_wt(void)
{
    model::kv_database database;

    model::kv_table_config table_config;
    table_config.log_enabled = true;
    model::kv_table_ptr table = database.create_table("table", table_config);

    /* Create the test's home directory and database. */
    WT_CONNECTION *conn;
    WT_SESSION *session;
    const char *uri = "table:table";

    std::string test_home = std::string(home) + DIR_DELIM_STR + "basic-logged";
    testutil_recreate_dir(test_home.c_str());
    testutil_wiredtiger_open(opts, test_home.c_str(), ENV_CONFIG, nullptr, &conn, false, false);
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session));
    testutil_check(session->create(session, uri, "key_format=S,value_format=S,log=(enabled=true)"));

    /* Populate the table with a few values and check that timestamps are ignored. */
    wt_model_insert_both(table, uri, key1, value1, 10);
    wt_model_insert_both(table, uri, key1, value2, 20);
    wt_model_remove_both(table, uri, key1, 30);
    wt_model_insert_both(table, uri, key1, value4, 40);

    wt_model_assert(table, uri, key1, 5);
    wt_model_assert(table, uri, key1, 10);
    wt_model_assert(table, uri, key1, 20);
    wt_model_assert(table, uri, key1, 30);
    wt_model_assert(table, uri, key1, 40);
    wt_model_assert(table, uri, key1);

    testutil_assert(table->verify_noexcept(conn));

    /* Test non-timestamped updates. */
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

    /* Try non-timestamped updates to the same key. */
    wt_model_insert_both(table, uri, key2, value2);
    wt_model_assert(table, uri, key2);

    /* Test multiple inserts with the same timestamp. */
    wt_model_insert_both(table, uri, key1, value1, 50);
    wt_model_insert_both(table, uri, key1, value2, 50);
    wt_model_insert_both(table, uri, key1, value3, 50);
    wt_model_insert_both(table, uri, key1, value4, 60);

    wt_model_assert(table, uri, key1, 50);
    wt_model_assert(table, uri, key1, 55);
    wt_model_assert(table, uri, key1);

    testutil_assert(table->verify_noexcept(conn));

    /* Test insert without overwrite. */
    wt_model_insert_both(table, uri, key1, value1, 60, false);
    wt_model_insert_both(table, uri, key1, value1, 65, false);
    wt_model_remove_both(table, uri, key1, 65);
    wt_model_insert_both(table, uri, key1, value1, 70, false);

    /* Test updates. */
    wt_model_update_both(table, uri, key1, value2, 70);
    wt_model_assert(table, uri, key1, 70);
    wt_model_remove_both(table, uri, key1, 80);
    wt_model_update_both(table, uri, key1, value1, 80, false);
    wt_model_update_both(table, uri, key1, value1, 85, false);

    /* Clean up. */
    testutil_check(session->close(session, nullptr));
    testutil_check(conn->close(conn, nullptr));

    /* Verify using the debug log. */
    verify_using_debug_log(opts, test_home.c_str(), true);
}

/*
 * test_model_truncate --
 *     Test truncation.
 */
static void
test_model_truncate(bool logging)
{
    model::kv_database database;

    model::kv_table_config table_config;
    table_config.log_enabled = logging;
    model::kv_table_ptr table = database.create_table("table", table_config);

    /* Populate the table. */
    testutil_check(table->insert(key1, value1, 10));
    testutil_check(table->insert(key2, value2, 20));
    testutil_check(table->insert(key3, value3, 20));
    testutil_check(table->insert(key4, value4, 30));
    testutil_check(table->insert(key5, value5, 30));

    /* Truncate. */
    testutil_check(table->truncate(key2, key4, 30));
    testutil_assert(table->get(key1) == value1);
    testutil_assert(table->get(key2) == model::NONE);
    testutil_assert(table->get(key3) == model::NONE);
    testutil_assert(table->get(key4) == model::NONE);
    testutil_assert(table->get(key5) == value5);

    /* Add the keys back and try range truncates that involve the beginning and the end. */
    testutil_check(table->insert(key2, value2, 40));
    testutil_check(table->insert(key3, value3, 40));
    testutil_check(table->insert(key4, value4, 40));

    testutil_check(table->truncate(model::NONE, key2, 40));
    testutil_check(table->truncate(key4, model::NONE, 40));
    testutil_assert(table->get(key1) == model::NONE);
    testutil_assert(table->get(key2) == model::NONE);
    testutil_assert(table->get(key3) == value3);
    testutil_assert(table->get(key4) == model::NONE);
    testutil_assert(table->get(key5) == model::NONE);

    /* Now try the full range truncate. */
    testutil_check(table->insert(key2, value2, 50));
    testutil_check(table->insert(key3, value3, 50));
    testutil_check(table->insert(key4, value4, 50));

    testutil_check(table->truncate(model::NONE, model::NONE, 50));
    testutil_assert(table->get(key1) == model::NONE);
    testutil_assert(table->get(key2) == model::NONE);
    testutil_assert(table->get(key3) == model::NONE);
    testutil_assert(table->get(key4) == model::NONE);
    testutil_assert(table->get(key5) == model::NONE);

    /* Start and stop keys don't actually need to exist. */
    testutil_check(table->insert(key2, value2, 60));
    testutil_check(table->insert(key3, value3, 60));
    testutil_check(table->insert(key4, value4, 60));

    testutil_check(table->truncate(key1, key2, 60));
    testutil_check(table->truncate(key4, key5, 60));
    testutil_assert(table->get(key1) == model::NONE);
    testutil_assert(table->get(key2) == model::NONE);
    testutil_assert(table->get(key3) == value3);
    testutil_assert(table->get(key4) == model::NONE);
    testutil_assert(table->get(key5) == model::NONE);
}

/*
 * test_model_truncate_wt --
 *     Test truncation - with WiredTiger.
 */
static void
test_model_truncate_wt(bool logging)
{
    model::kv_database database;

    model::kv_table_config table_config;
    table_config.log_enabled = logging;
    model::kv_table_ptr table = database.create_table("table", table_config);

    /* Create the test's home directory and database. */
    WT_CONNECTION *conn;
    WT_SESSION *session;
    const char *uri = "table:table";

    std::string test_home = std::string(home) + DIR_DELIM_STR + "truncate";
    if (logging)
        test_home += "-logged";
    testutil_recreate_dir(test_home.c_str());
    testutil_wiredtiger_open(opts, test_home.c_str(), ENV_CONFIG, nullptr, &conn, false, false);
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session));
    std::string config = "key_format=S,value_format=S,log=(enabled=";
    config += std::string(logging ? "true" : "false") + ")";
    testutil_check(session->create(session, uri, config.c_str()));

    /* Populate the table. */
    wt_model_insert_both(table, uri, key1, value1, 10);
    wt_model_insert_both(table, uri, key2, value2, 20);
    wt_model_insert_both(table, uri, key3, value3, 20);
    wt_model_insert_both(table, uri, key4, value4, 30);
    wt_model_insert_both(table, uri, key5, value5, 30);

    /* Truncate. */
    wt_model_truncate_both(table, uri, key2, key4, 30);
    wt_model_assert(table, uri, key1);
    wt_model_assert(table, uri, key2);
    wt_model_assert(table, uri, key3);
    wt_model_assert(table, uri, key4);
    wt_model_assert(table, uri, key5);

    /* Add the keys back and try range truncates that involve the beginning and the end. */
    wt_model_insert_both(table, uri, key2, value2, 40);
    wt_model_insert_both(table, uri, key3, value3, 40);
    wt_model_insert_both(table, uri, key4, value4, 40);

    wt_model_truncate_both(table, uri, model::NONE, key2, 40);
    wt_model_truncate_both(table, uri, key4, model::NONE, 40);
    wt_model_assert(table, uri, key1);
    wt_model_assert(table, uri, key2);
    wt_model_assert(table, uri, key3);
    wt_model_assert(table, uri, key4);
    wt_model_assert(table, uri, key5);

    /* Now try the full range truncate. */
    wt_model_insert_both(table, uri, key2, value2, 50);
    wt_model_insert_both(table, uri, key3, value3, 50);
    wt_model_insert_both(table, uri, key4, value4, 50);

    wt_model_truncate_both(table, uri, model::NONE, model::NONE, 50);
    wt_model_assert(table, uri, key1);
    wt_model_assert(table, uri, key2);
    wt_model_assert(table, uri, key3);
    wt_model_assert(table, uri, key4);
    wt_model_assert(table, uri, key5);

    /* Start and stop keys don't actually need to exist. */
    wt_model_insert_both(table, uri, key2, value2, 60);
    wt_model_insert_both(table, uri, key3, value3, 60);
    wt_model_insert_both(table, uri, key4, value4, 60);

    wt_model_truncate_both(table, uri, key1, key2, 60);
    wt_model_truncate_both(table, uri, key4, key5, 60);
    wt_model_assert(table, uri, key1);
    wt_model_assert(table, uri, key2);
    wt_model_assert(table, uri, key3);
    wt_model_assert(table, uri, key4);
    wt_model_assert(table, uri, key5);

    /* Verify. */
    testutil_assert(table->verify_noexcept(conn));

    /* Now try to get the verification to fail. */
    testutil_check(table->insert(key2, value1, 1000));
    testutil_assert(!table->verify_noexcept(conn));

    /* Clean up. */
    testutil_check(session->close(session, nullptr));
    testutil_check(conn->close(conn, nullptr));

    /* Verify using the debug log. */
    verify_using_debug_log(opts, test_home.c_str(), true);
}

/*
 * test_model_truncate_column_wt --
 *     Test truncation - with columns and with WiredTiger.
 */
static void
test_model_truncate_column_wt(bool logging)
{
    model::kv_database database;

    model::kv_table_config table_config;
    table_config.log_enabled = logging;
    table_config.type = model::kv_table_type::column;
    model::kv_table_ptr table = database.create_table("table", table_config);

    /* Create the test's home directory and database. */
    WT_CONNECTION *conn;
    WT_SESSION *session;
    const char *uri = "table:table";

    std::string test_home = std::string(home) + DIR_DELIM_STR + "truncate-column";
    if (logging)
        test_home += "-logged";
    testutil_recreate_dir(test_home.c_str());
    testutil_wiredtiger_open(opts, test_home.c_str(), ENV_CONFIG, nullptr, &conn, false, false);
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session));
    std::string config = "key_format=r,value_format=S,log=(enabled=";
    config += std::string(logging ? "true" : "false") + ")";
    testutil_check(session->create(session, uri, config.c_str()));

    /* Populate the table. */
    wt_model_insert_both(table, uri, recno1, value1, 10);
    wt_model_insert_both(table, uri, recno2, value2, 20);
    wt_model_insert_both(table, uri, recno3, value3, 20);
    wt_model_insert_both(table, uri, recno4, value4, 30);
    wt_model_insert_both(table, uri, recno5, value5, 30);

    /* Truncate. */
    wt_model_truncate_both(table, uri, recno2, recno4, 30);
    wt_model_assert(table, uri, recno1);
    wt_model_assert(table, uri, recno2);
    wt_model_assert(table, uri, recno3);
    wt_model_assert(table, uri, recno4);
    wt_model_assert(table, uri, recno5);

    /* Add the recnos back and try range truncates that involve the beginning and the end. */
    wt_model_insert_both(table, uri, recno2, value2, 40);
    wt_model_insert_both(table, uri, recno3, value3, 40);
    wt_model_insert_both(table, uri, recno4, value4, 40);

    wt_model_truncate_both(table, uri, model::NONE, recno2, 40);
    wt_model_truncate_both(table, uri, recno4, model::NONE, 40);
    wt_model_assert(table, uri, recno1);
    wt_model_assert(table, uri, recno2);
    wt_model_assert(table, uri, recno3);
    wt_model_assert(table, uri, recno4);
    wt_model_assert(table, uri, recno5);

    /* Now try the full range truncate. */
    wt_model_insert_both(table, uri, recno2, value2, 50);
    wt_model_insert_both(table, uri, recno3, value3, 50);
    wt_model_insert_both(table, uri, recno4, value4, 50);

    wt_model_truncate_both(table, uri, model::NONE, model::NONE, 50);
    wt_model_assert(table, uri, recno1);
    wt_model_assert(table, uri, recno2);
    wt_model_assert(table, uri, recno3);
    wt_model_assert(table, uri, recno4);
    wt_model_assert(table, uri, recno5);

    /* Start and stop recnos don't actually need to exist. */
    wt_model_insert_both(table, uri, recno2, value2, 60);
    wt_model_insert_both(table, uri, recno3, value3, 60);
    wt_model_insert_both(table, uri, recno4, value4, 60);

    wt_model_truncate_both(table, uri, recno1, recno2, 60);
    wt_model_truncate_both(table, uri, recno4, recno5, 60);
    wt_model_assert(table, uri, recno1);
    wt_model_assert(table, uri, recno2);
    wt_model_assert(table, uri, recno3);
    wt_model_assert(table, uri, recno4);
    wt_model_assert(table, uri, recno5);

    /* Verify. */
    testutil_assert(table->verify_noexcept(conn));

    /* Now try to get the verification to fail. */
    testutil_check(table->insert(recno2, value1, 1000));
    testutil_assert(!table->verify_noexcept(conn));

    /* Clean up. */
    testutil_check(session->close(session, nullptr));
    testutil_check(conn->close(conn, nullptr));

    /* Verify using the debug log. */
    verify_using_debug_log(opts, test_home.c_str(), true);
}

/*
 * test_model_truncate_column_fix_wt --
 *     Test truncation - with FLCS and with WiredTiger.
 */
static void
test_model_truncate_column_fix_wt(bool logging)
{
    model::kv_database database;

    model::kv_table_config table_config;
    table_config.log_enabled = logging;
    table_config.type = model::kv_table_type::column_fix;
    model::kv_table_ptr table = database.create_table("table", table_config);

    /* Create the test's home directory and database. */
    WT_CONNECTION *conn;
    WT_SESSION *session;
    const char *uri = "table:table";

    std::string test_home = std::string(home) + DIR_DELIM_STR + "truncate-column-fix";
    if (logging)
        test_home += "-logged";
    testutil_recreate_dir(test_home.c_str());
    testutil_wiredtiger_open(opts, test_home.c_str(), ENV_CONFIG, nullptr, &conn, false, false);
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session));
    std::string config = "key_format=r,value_format=8t,log=(enabled=";
    config += std::string(logging ? "true" : "false") + ")";
    testutil_check(session->create(session, uri, config.c_str()));

    /* Populate the table. */
    wt_model_insert_both(table, uri, recno1, byte1, 10);
    wt_model_insert_both(table, uri, recno2, byte2, 20);
    wt_model_insert_both(table, uri, recno3, byte3, 20);
    wt_model_insert_both(table, uri, recno4, byte4, 30);
    wt_model_insert_both(table, uri, recno5, byte5, 30);

    /* Truncate. */
    wt_model_truncate_both(table, uri, recno2, recno4, 30);
    wt_model_assert(table, uri, recno1);
    wt_model_assert(table, uri, recno2);
    wt_model_assert(table, uri, recno3);
    wt_model_assert(table, uri, recno4);
    wt_model_assert(table, uri, recno5);

    /* Add the recnos back and try range truncates that involve the beginning and the end. */
    wt_model_insert_both(table, uri, recno2, byte2, 40);
    wt_model_insert_both(table, uri, recno3, byte3, 40);
    wt_model_insert_both(table, uri, recno4, byte4, 40);

    wt_model_truncate_both(table, uri, model::NONE, recno2, 40);
    wt_model_truncate_both(table, uri, recno4, model::NONE, 40);
    wt_model_assert(table, uri, recno1);
    wt_model_assert(table, uri, recno2);
    wt_model_assert(table, uri, recno3);
    wt_model_assert(table, uri, recno4);
    wt_model_assert(table, uri, recno5);

    /* Now try the full range truncate. */
    wt_model_insert_both(table, uri, recno2, byte2, 50);
    wt_model_insert_both(table, uri, recno3, byte3, 50);
    wt_model_insert_both(table, uri, recno4, byte4, 50);

    wt_model_truncate_both(table, uri, model::NONE, model::NONE, 50);
    wt_model_assert(table, uri, recno1);
    wt_model_assert(table, uri, recno2);
    wt_model_assert(table, uri, recno3);
    wt_model_assert(table, uri, recno4);
    wt_model_assert(table, uri, recno5);

    /* Start and stop recnos don't actually need to exist. */
    wt_model_insert_both(table, uri, recno2, byte2, 60);
    wt_model_insert_both(table, uri, recno3, byte3, 60);
    wt_model_insert_both(table, uri, recno4, byte4, 60);

    wt_model_truncate_both(table, uri, recno1, recno2, 60);
    wt_model_truncate_both(table, uri, recno4, recno5, 60);
    wt_model_assert(table, uri, recno1);
    wt_model_assert(table, uri, recno2);
    wt_model_assert(table, uri, recno3);
    wt_model_assert(table, uri, recno4);
    wt_model_assert(table, uri, recno5);

    /* Verify. */
    testutil_assert(table->verify_noexcept(conn));

    /* Now try to get the verification to fail. */
    testutil_check(table->insert(recnoX, byte1, 1000));
    testutil_assert(!table->verify_noexcept(conn));

    /* Clean up. */
    testutil_check(session->close(session, nullptr));
    testutil_check(conn->close(conn, nullptr));

    /* Verify using the debug log. */
    verify_using_debug_log(opts, test_home.c_str(), true);
}

/*
 * test_model_oldest --
 *     Test setting the oldest timestamp.
 */
static void
test_model_oldest(void)
{
    model::kv_database database;
    model::kv_table_ptr table = database.create_table("table");

    /* Populate the table with data from a few different timestamps. */
    testutil_check(table->insert(key1, value1, 10));
    testutil_check(table->insert(key1, value2, 20));
    testutil_check(table->insert(key1, value3, 30));
    testutil_check(table->insert(key1, value4, 40));
    testutil_check(table->insert(key1, value5, 50));

    /* Set the oldest timestamp. */
    testutil_check(database.set_oldest_timestamp(30));
    testutil_assert(database.oldest_timestamp() == 30);

    /* Verify the behavior. */
    model::data_value v;
    testutil_assert(table->get_ext(key1, v, 10) == EINVAL);
    testutil_assert(table->get_ext(key1, v, 20) == EINVAL);
    testutil_assert(table->get(key1, 30) == value3);
    testutil_assert(table->get(key1, 40) == value4);
    testutil_assert(table->get(key1, 50) == value5);

    /* Set the oldest timestamp again. */
    testutil_check(database.set_oldest_timestamp(50));
    testutil_assert(database.oldest_timestamp() == 50);

    /* Verify the behavior. */
    testutil_assert(table->get_ext(key1, v, 10) == EINVAL);
    testutil_assert(table->get_ext(key1, v, 20) == EINVAL);
    testutil_assert(table->get_ext(key1, v, 30) == EINVAL);
    testutil_assert(table->get_ext(key1, v, 40) == EINVAL);
    testutil_assert(table->get(key1, 50) == value5);

    /* Test moving the oldest timestamp backwards - this should fail. */
    testutil_assert(database.set_oldest_timestamp(10) == EINVAL);
    testutil_assert(database.oldest_timestamp() == 50);

    /* Test setting the stable timestamp to before the oldest timestamp - this should also fail. */
    testutil_assert(database.set_stable_timestamp(10) == EINVAL);
    testutil_assert(database.stable_timestamp() == model::k_timestamp_none);

    /* The oldest timestamp should reset, because we don't have the stable timestamp. */
    database.restart();
    testutil_assert(database.oldest_timestamp() == 0);

    /* Now try it with both the oldest and stable timestamps. */
    testutil_check(database.set_oldest_timestamp(50));
    testutil_check(database.set_stable_timestamp(55));
    database.restart();
    testutil_assert(database.oldest_timestamp() == 50);

    /* Try setting the oldest timestamp to, and then ahead of, the stable timestamp. */
    testutil_check(database.set_oldest_timestamp(55));
    testutil_assert(database.set_oldest_timestamp(60) == EINVAL);
    testutil_assert(database.oldest_timestamp() == 55);
}

/*
 * test_model_oldest_wt --
 *     Test setting the oldest timestamp - with WiredTiger.
 */
static void
test_model_oldest_wt(void)
{
    model::kv_database database;
    model::kv_table_ptr table = database.create_table("table");

    /* Create the test's home directory and database. */
    WT_CONNECTION *conn;
    WT_SESSION *session;
    const char *uri = "table:table";

    std::string test_home = std::string(home) + DIR_DELIM_STR + "oldest";
    testutil_recreate_dir(test_home.c_str());
    testutil_wiredtiger_open(opts, test_home.c_str(), ENV_CONFIG, nullptr, &conn, false, false);
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session));
    testutil_check(
      session->create(session, uri, "key_format=S,value_format=S,log=(enabled=false)"));

    /* Populate the table with data from a few different timestamps. */
    wt_model_insert_both(table, uri, key1, value1, 10);
    wt_model_insert_both(table, uri, key1, value2, 20);
    wt_model_insert_both(table, uri, key1, value3, 30);
    wt_model_insert_both(table, uri, key1, value4, 40);
    wt_model_insert_both(table, uri, key1, value5, 50);

    /* Set the oldest timestamp. */
    wt_model_set_oldest_timestamp_both(30);
    testutil_assert(database.oldest_timestamp() == wt_get_oldest_timestamp(conn));

    /* Verify the behavior. */
    wt_model_assert(table, uri, key1, 10);
    wt_model_assert(table, uri, key1, 20);
    wt_model_assert(table, uri, key1, 30);
    wt_model_assert(table, uri, key1, 40);
    wt_model_assert(table, uri, key1, 50);

    /* Set the oldest timestamp again. */
    wt_model_set_oldest_timestamp_both(50);
    testutil_assert(database.oldest_timestamp() == wt_get_oldest_timestamp(conn));

    /* Verify the behavior. */
    wt_model_assert(table, uri, key1, 10);
    wt_model_assert(table, uri, key1, 20);
    wt_model_assert(table, uri, key1, 30);
    wt_model_assert(table, uri, key1, 40);
    wt_model_assert(table, uri, key1, 50);

    /* Test moving the oldest timestamp backwards - this should fail. */
    wt_model_set_oldest_timestamp_both(10);
    testutil_assert(database.oldest_timestamp() == wt_get_oldest_timestamp(conn));

    /* Test setting the stable timestamp to before the oldest timestamp - this should also fail. */
    wt_model_set_stable_timestamp_both(10);
    testutil_assert(database.stable_timestamp() == wt_get_stable_timestamp(conn));

    /* Verify. */
    testutil_assert(table->verify_noexcept(conn));

    /* Restart the database. */
    database.restart();
    testutil_check(session->close(session, nullptr));
    testutil_check(conn->close(conn, nullptr));
    testutil_wiredtiger_open(opts, test_home.c_str(), ENV_CONFIG, nullptr, &conn, false, false);
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session));

    /* The oldest timestamp should reset, because we don't have the stable timestamp. */
    testutil_assert(database.oldest_timestamp() == wt_get_oldest_timestamp(conn));

    /* Now try it with both the oldest and stable timestamps. */
    wt_model_set_oldest_timestamp_both(50);
    wt_model_set_stable_timestamp_both(55);
    database.restart();
    testutil_check(session->close(session, nullptr));
    testutil_check(conn->close(conn, nullptr));
    testutil_wiredtiger_open(opts, test_home.c_str(), ENV_CONFIG, nullptr, &conn, false, false);
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session));
    testutil_assert(database.oldest_timestamp() == wt_get_oldest_timestamp(conn));

    /* Try setting the oldest timestamp to, and then ahead of, the stable timestamp. */
    wt_model_set_oldest_timestamp_both(55);
    wt_model_set_oldest_timestamp_both(60);
    testutil_assert(database.oldest_timestamp() == wt_get_oldest_timestamp(conn));

    /* Clean up. */
    testutil_check(session->close(session, nullptr));
    testutil_check(conn->close(conn, nullptr));

    /* Verify using the debug log. */
    verify_using_debug_log(opts, test_home.c_str(), true);
}

/*
 * test_model_debug_log_verify_wt --
 *     Test the debug log based verification.
 */
static void
test_model_debug_log_verify_wt(void)
{
    model::kv_database database;
    model::kv_table_ptr table = database.create_table("table");

    /* Create the test's home directory and database. */
    WT_CONNECTION *conn;
    WT_SESSION *session;
    const char *uri = "table:table";

    std::string test_home = std::string(home) + DIR_DELIM_STR + "debug-log";
    testutil_recreate_dir(test_home.c_str());
    testutil_wiredtiger_open(opts, test_home.c_str(), ENV_CONFIG, nullptr, &conn, false, false);
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session));
    testutil_check(
      session->create(session, uri, "key_format=Q,value_format=Q,log=(enabled=false)"));

    /* Insert a few key-value pairs to check that the debug log parser unpacks numbers correctly. */
    for (uint64_t i = 0; i < 10 * WT_MILLION; i = (i * 7) + 1)
        wt_model_insert_both(table, uri, model::data_value(i), model::data_value(2 * i));

    /* Checkpoint and clean up. */
    wt_model_ckpt_create_both(nullptr);
    testutil_check(session->close(session, nullptr));
    testutil_check(conn->close(conn, nullptr));

    /* Verify using the debug log. */
    verify_using_debug_log(opts, test_home.c_str(), true);
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
    WT_DECL_RET;

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
    testutil_recreate_dir(home);

    /*
     * Tests.
     */
    try {
        ret = EXIT_SUCCESS;
        test_data_value();
        test_model_basic();
        test_model_basic_wt();
        test_model_basic_column_wt();
        test_model_basic_column_fix_wt();
        test_model_basic_logged();
        test_model_basic_logged_wt();
        test_model_truncate(false);
        test_model_truncate_wt(false);
        test_model_truncate(true);
        test_model_truncate_wt(true);
        test_model_truncate_column_wt(false);
        test_model_truncate_column_wt(true);
        test_model_truncate_column_fix_wt(false);
        test_model_truncate_column_fix_wt(true);
        test_model_oldest();
        test_model_oldest_wt();
        test_model_debug_log_verify_wt();
    } catch (std::exception &e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        ret = EXIT_FAILURE;
    }

    /*
     * Clean up.
     */
    /* Delete the work directory. */
    if (!opts->preserve)
        testutil_remove(home);

    testutil_cleanup(opts);
    return ret;
}
