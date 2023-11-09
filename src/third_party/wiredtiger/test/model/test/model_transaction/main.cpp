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
#include "model/util.h"

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

/*
 * test_transaction_basic --
 *     The basic test of the transaction model.
 */
static void
test_transaction_basic(void)
{
    model::kv_database database;
    model::kv_table_ptr table = database.create_table("table");

    /* Keys. */
    const model::data_value key1("Key 1");
    const model::data_value key2("Key 2");
    const model::data_value key3("Key 3");

    /* Values. */
    const model::data_value value1("Value 1");
    const model::data_value value2("Value 2");
    const model::data_value value3("Value 3");
    const model::data_value value4("Value 4");
    const model::data_value value5("Value 5");
    const model::data_value value6("Value 6");

    /* Transactions. */
    model::kv_transaction_ptr txn1, txn2;

    /* A basic test with two transactions. */
    txn1 = database.begin_transaction();
    txn2 = database.begin_transaction();
    testutil_check(table->insert(txn1, key1, value1));
    testutil_check(table->insert(txn2, key2, value2));
    testutil_assert(table->get(txn1, key1) == value1);
    testutil_assert(table->get(txn2, key2) == value2);
    testutil_assert(table->get(txn2, key1) == model::NONE);
    testutil_assert(table->get(txn1, key2) == model::NONE);
    txn1->commit(10);
    txn2->commit(10);
    testutil_assert(table->get(key1) == value1);
    testutil_assert(table->get(key2) == value2);

    /* Check the read timestamp. */
    txn1 = database.begin_transaction(5);
    testutil_assert(table->get(txn1, key1) == model::NONE);
    txn1->commit(10);
    txn1 = database.begin_transaction(10);
    testutil_assert(table->get(txn1, key1) == value1);
    txn1->commit(15);

    /* Check transaction conflicts: Concurrent update. */
    txn1 = database.begin_transaction();
    txn2 = database.begin_transaction();
    testutil_check(table->insert(txn1, key1, value3));
    testutil_check(table->insert(txn1, key1, value1));
    testutil_assert(table->insert(txn2, key1, value4) == WT_ROLLBACK);
    testutil_assert(!txn1->failed());
    testutil_assert(txn2->failed());
    txn1->commit(20);
    txn2->commit(20);
    testutil_assert(table->get(key1) == value1);

    txn1 = database.begin_transaction();
    txn2 = database.begin_transaction();
    testutil_check(table->insert(txn1, key1, value3));
    txn1->commit(30);
    testutil_assert(table->insert(txn2, key1, value4) == WT_ROLLBACK);
    txn2->commit(30);
    testutil_assert(table->get(key1) == value3);

    /* Check transaction conflicts: Update not in the transaction snapshot. */
    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key1, value1));
    txn2 = database.begin_transaction();
    txn1->commit(40);
    testutil_assert(table->insert(txn2, key1, value4) == WT_ROLLBACK);
    txn2->commit(40);
    testutil_assert(table->get(key1) == value1);

    /* Set timestamp. */
    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key1, value4));
    txn1->set_commit_timestamp(42);
    testutil_check(table->insert(txn1, key2, value5));
    txn1->set_commit_timestamp(44);
    testutil_check(table->insert(txn1, key3, value6));
    txn1->commit(50);

    testutil_assert(table->get(key1, 50) == value4);
    testutil_assert(table->get(key2, 42) == value5);
    testutil_assert(table->get(key3, 44) == value6);
    testutil_assert(table->get(key1, 50 - 1) != value4);
    testutil_assert(table->get(key2, 42 - 1) != value5);
    testutil_assert(table->get(key3, 44 - 1) != value6);

    /* Set timestamp: Check timestamp order within the same key. */
    txn1 = database.begin_transaction();
    txn1->set_commit_timestamp(52);
    testutil_check(table->insert(txn1, key1, value1));
    testutil_check(table->insert(txn1, key1, value2));
    txn1->set_commit_timestamp(55);
    testutil_check(table->insert(txn1, key1, value3));
    txn1->set_commit_timestamp(53);
    model_testutil_assert_exception(table->insert(txn1, key1, value3),
      model::wiredtiger_abort_exception); /* WT aborts at commit. */
    txn1->commit(60);
    testutil_assert(table->get(key1, 52) == value2);
    testutil_assert(table->get(key1, 53) == value2);
    testutil_assert(table->get(key1, 55) == value3);

    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key1, value1));
    txn1->set_commit_timestamp(65);
    model_testutil_assert_exception(table->insert(txn1, key1, value4),
      model::wiredtiger_abort_exception); /* WT aborts at checkpoint. */
    txn1->commit(70);
    testutil_assert(table->get(key1, 65) == value3);
    testutil_assert(table->get(key1, 70) == value1);

    /* Roll back a transaction. */
    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key1, value2));
    testutil_check(table->insert(txn1, key2, value2));
    txn1->rollback();
    testutil_assert(table->get(key1) == value1);
    testutil_assert(table->get(key2) == value5);

    /* Reset the transaction snapshot. */
    txn1 = database.begin_transaction();
    txn2 = database.begin_transaction();
    testutil_check(table->insert(txn1, key1, value3));
    txn1->commit(80);
    txn2->reset_snapshot();
    testutil_assert(table->get(txn2, key1) == value3);
    testutil_check(table->insert(txn2, key1, value4));
    txn2->commit(90);
    testutil_assert(table->get(key1) == value4);
}

/*
 * test_transaction_basic_wt --
 *     The basic test of the transaction model, also in WiredTiger.
 */
static void
test_transaction_basic_wt(void)
{
    model::kv_database database;
    model::kv_table_ptr table = database.create_table("table");

    /* Keys. */
    const model::data_value key1("Key 1");
    const model::data_value key2("Key 2");
    const model::data_value key3("Key 3");

    /* Values. */
    const model::data_value value1("Value 1");
    const model::data_value value2("Value 2");
    const model::data_value value3("Value 3");
    const model::data_value value4("Value 4");
    const model::data_value value5("Value 5");
    const model::data_value value6("Value 6");

    /* Transactions. */
    model::kv_transaction_ptr txn1, txn2;

    /* Create the test's home directory and database. */
    WT_CONNECTION *conn;
    WT_SESSION *session;
    WT_SESSION *session1;
    WT_SESSION *session2;
    const char *uri = "table:table";

    testutil_recreate_dir(home);
    testutil_wiredtiger_open(opts, home, ENV_CONFIG, nullptr, &conn, false, false);
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session));
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session1));
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session2));
    testutil_check(
      session->create(session, uri, "key_format=S,value_format=S,log=(enabled=false)"));

    /* A basic test with two transactions. */
    wt_model_txn_begin_both(txn1, session1);
    wt_model_txn_begin_both(txn2, session2);
    wt_model_txn_insert_both(table, uri, txn1, session1, key1, value1);
    wt_model_txn_insert_both(table, uri, txn2, session2, key2, value2);
    wt_model_txn_assert(table, uri, txn1, session1, key1);
    wt_model_txn_assert(table, uri, txn2, session2, key2);
    wt_model_txn_assert(table, uri, txn1, session1, key2);
    wt_model_txn_assert(table, uri, txn2, session2, key1);
    wt_model_txn_commit_both(txn1, session1, 10);
    wt_model_txn_commit_both(txn2, session2, 10);

    /* Check the read timestamp. */
    wt_model_txn_begin_both(txn1, session1, 5);
    wt_model_txn_assert(table, uri, txn1, session1, key1);
    wt_model_txn_commit_both(txn1, session1, 10);
    wt_model_txn_begin_both(txn1, session1, 10);
    wt_model_txn_assert(table, uri, txn1, session1, key1);
    wt_model_txn_commit_both(txn1, session1, 15);

    /* Check transaction conflicts: Concurrent update. */
    wt_model_txn_begin_both(txn1, session1);
    wt_model_txn_begin_both(txn2, session2);
    wt_model_txn_insert_both(table, uri, txn1, session1, key1, value3);
    wt_model_txn_insert_both(table, uri, txn1, session1, key1, value4);
    wt_model_txn_insert_both(table, uri, txn2, session2, key1, value4); /* Rollback. */
    wt_model_txn_commit_both(txn1, session1, 20);
    wt_model_txn_commit_both(txn2, session2, 20);

    wt_model_txn_begin_both(txn1, session1);
    wt_model_txn_begin_both(txn2, session2);
    wt_model_txn_insert_both(table, uri, txn1, session1, key1, value3);
    wt_model_txn_commit_both(txn1, session1, 30);
    wt_model_txn_insert_both(table, uri, txn2, session2, key1, value4); /* Rollback. */
    wt_model_txn_commit_both(txn2, session2, 30);

    /* Check transaction conflicts: Update not in the transaction snapshot. */
    wt_model_txn_begin_both(txn1, session1);
    wt_model_txn_insert_both(table, uri, txn1, session1, key1, value1);
    wt_model_txn_begin_both(txn2, session2);
    wt_model_txn_commit_both(txn1, session1, 40);
    wt_model_txn_insert_both(table, uri, txn2, session2, key1, value4); /* Rollback. */
    wt_model_txn_commit_both(txn2, session2, 40);

    // Not testing conflict between a transactional update and an update outside of a transaction;
    // this can result in a hang or an abort.

    /* Set timestamp. */
    wt_model_txn_begin_both(txn1, session1);
    wt_model_txn_insert_both(table, uri, txn1, session1, key1, value4);
    wt_model_txn_set_timestamp_both(txn1, session1, 43);
    wt_model_txn_insert_both(table, uri, txn1, session1, key2, value5);
    wt_model_txn_set_timestamp_both(txn1, session1, 44);
    wt_model_txn_insert_both(table, uri, txn1, session1, key3, value6);
    wt_model_txn_commit_both(txn1, session1, 50);

    wt_model_assert(table, uri, key1, 50);
    wt_model_assert(table, uri, key2, 42);
    wt_model_assert(table, uri, key3, 44);
    wt_model_assert(table, uri, key1, 50 - 1);
    wt_model_assert(table, uri, key2, 42 - 1);
    wt_model_assert(table, uri, key3, 44 - 1);

    /* Set timestamp: Check timestamp order within the same key. */
    wt_model_txn_begin_both(txn1, session1);
    wt_model_txn_set_timestamp_both(txn1, session1, 52);
    wt_model_txn_insert_both(table, uri, txn1, session1, key1, value1);
    wt_model_txn_insert_both(table, uri, txn1, session1, key1, value2);
    wt_model_txn_set_timestamp_both(txn1, session1, 55);
    wt_model_txn_insert_both(table, uri, txn1, session1, key1, value3);
    wt_model_txn_set_timestamp_both(txn1, session1, 53);
    // Cannot insert key 1 at timestamp 53: Commit would result in abort.
    // wt_txn_insert(session1, uri, key1, value3);
    wt_model_txn_commit_both(txn1, session1, 60);
    wt_model_assert(table, uri, key1, 52);
    wt_model_assert(table, uri, key1, 53);
    wt_model_assert(table, uri, key1, 55);

    wt_model_txn_begin_both(txn1, session1);
    wt_model_txn_insert_both(table, uri, txn1, session1, key1, value1);
    wt_model_txn_set_timestamp_both(txn1, session1, 65);
    // Cannot insert key 1 at timestamp 65: Reconciliation would trigger abort.
    // wt_txn_insert(session1, uri, key1, value4);
    wt_model_txn_commit_both(txn1, session1, 70);
    wt_model_assert(table, uri, key1, 65);
    wt_model_assert(table, uri, key1, 70);

    /* Roll back a transaction. */
    wt_model_txn_begin_both(txn1, session1);
    wt_model_txn_insert_both(table, uri, txn1, session1, key1, value2);
    wt_model_txn_insert_both(table, uri, txn1, session1, key1, value2);
    wt_model_txn_rollback_both(txn1, session1);
    wt_model_assert(table, uri, key1);
    wt_model_assert(table, uri, key2);

    /* Reset the transaction snapshot. */
    wt_model_txn_begin_both(txn1, session1);
    wt_model_txn_begin_both(txn2, session2);
    wt_model_txn_insert_both(table, uri, txn1, session1, key1, value3);
    wt_model_txn_commit_both(txn1, session1, 80);
    wt_model_txn_reset_snapshot_both(txn2, session2);
    wt_model_txn_assert(table, uri, txn2, session2, key1);
    wt_model_txn_insert_both(table, uri, txn2, session2, key1, value4); /* No conflict. */
    wt_model_txn_commit_both(txn2, session2, 90);
    wt_model_assert(table, uri, key1);

    /* Verify. */
    testutil_assert(table->verify_noexcept(conn));

    /* Clean up. */
    testutil_check(session->close(session, nullptr));
    testutil_check(session1->close(session1, nullptr));
    testutil_check(session2->close(session2, nullptr));
    testutil_check(conn->close(conn, nullptr));
}

/*
 * test_transaction_prepared --
 *     Test prepared transactions.
 */
static void
test_transaction_prepared(void)
{
    model::kv_database database;
    model::kv_table_ptr table = database.create_table("table");
    model::data_value v;

    /* Keys. */
    const model::data_value key1("Key 1");
    const model::data_value key2("Key 2");
    const model::data_value key3("Key 3");

    /* Values. */
    const model::data_value value1("Value 1");
    const model::data_value value2("Value 2");
    const model::data_value value3("Value 3");
    const model::data_value value4("Value 4");
    const model::data_value value5("Value 5");
    const model::data_value value6("Value 6");

    /* Transactions. */
    model::kv_transaction_ptr txn1, txn2;

    /* A basic test with two transactions. */
    txn1 = database.begin_transaction();
    txn2 = database.begin_transaction();
    testutil_check(table->insert(txn1, key1, value1));
    testutil_check(table->insert(txn2, key2, value2));
    testutil_assert(table->get(txn1, key1) == value1);
    testutil_assert(table->get(txn2, key2) == value2);
    testutil_assert(table->get(txn2, key1) == model::NONE);
    testutil_assert(table->get(txn1, key2) == model::NONE);
    txn1->prepare(5);
    txn2->prepare(5);
    testutil_assert(txn1->state() == model::kv_transaction_state::prepared);
    testutil_assert(txn2->state() == model::kv_transaction_state::prepared);
    txn1->commit(10, 10);
    txn2->commit(10, 15);
    testutil_assert(table->get(key1) == value1);
    testutil_assert(table->get(key2) == value2);
    testutil_assert(table->get(key1, 10) == value1);
    testutil_assert(table->get(key2, 10) == value2);

    /* Check transaction conflicts: Concurrent update. */
    txn1 = database.begin_transaction();
    txn2 = database.begin_transaction();
    testutil_check(table->insert(txn1, key1, value3));
    testutil_check(table->insert(txn1, key1, value1));
    txn1->prepare(20);
    testutil_assert(table->insert(txn2, key1, value4) == WT_ROLLBACK);
    testutil_assert(!txn1->failed());
    testutil_assert(txn2->failed());
    txn1->commit(20, 20);
    txn2->commit(20);
    testutil_assert(table->get(key1) == value1);

    txn1 = database.begin_transaction();
    txn2 = database.begin_transaction();
    testutil_check(table->insert(txn1, key1, value3));
    txn1->prepare(30);
    txn1->commit(30, 30);
    testutil_assert(table->insert(txn2, key1, value4) == WT_ROLLBACK);
    txn2->commit(30);
    testutil_assert(table->get(key1) == value3);

    /* Check prepare conflicts: Reading a prepared, but uncommitted update. */
    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key1, value1));
    txn1->prepare(40);
    testutil_assert(table->get_ext(key1, v) == WT_PREPARE_CONFLICT);
    testutil_assert(table->get_ext(key1, v, 40) == WT_PREPARE_CONFLICT);
    testutil_assert(table->get(key1, 39) == value3);
    testutil_assert(table->get(key1, 20) == value1);
    txn1->commit(40, 40);
    testutil_assert(table->get(key1, 40) == value1);

    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key1, value1));
    txn1->prepare(50);
    txn1->commit(50, 50);
    testutil_assert(table->get(key1) == value1);

    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key1, value2));
    txn1->prepare(60);
    txn1->rollback();
    testutil_assert(table->get(key1) == value1);
}

/*
 * test_transaction_prepared_wt --
 *     Test prepared transactions, also in WiredTiger.
 */
static void
test_transaction_prepared_wt(void)
{
    model::kv_database database;
    model::kv_table_ptr table = database.create_table("table");

    /* Keys. */
    const model::data_value key1("Key 1");
    const model::data_value key2("Key 2");
    const model::data_value key3("Key 3");

    /* Values. */
    const model::data_value value1("Value 1");
    const model::data_value value2("Value 2");
    const model::data_value value3("Value 3");
    const model::data_value value4("Value 4");
    const model::data_value value5("Value 5");
    const model::data_value value6("Value 6");

    /* Transactions. */
    model::kv_transaction_ptr txn1, txn2;

    /* Create the test's home directory and database. */
    WT_CONNECTION *conn;
    WT_SESSION *session;
    WT_SESSION *session1;
    WT_SESSION *session2;
    const char *uri = "table:table";

    testutil_recreate_dir(home);
    testutil_wiredtiger_open(opts, home, ENV_CONFIG, nullptr, &conn, false, false);
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session));
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session1));
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session2));
    testutil_check(
      session->create(session, uri, "key_format=S,value_format=S,log=(enabled=false)"));

    /* A basic test with two transactions. */
    wt_model_txn_begin_both(txn1, session1);
    wt_model_txn_begin_both(txn2, session2);
    wt_model_txn_insert_both(table, uri, txn1, session1, key1, value1);
    wt_model_txn_insert_both(table, uri, txn2, session2, key2, value2);
    wt_model_txn_assert(table, uri, txn1, session1, key1);
    wt_model_txn_assert(table, uri, txn2, session2, key2);
    wt_model_txn_assert(table, uri, txn1, session1, key2);
    wt_model_txn_assert(table, uri, txn2, session2, key1);
    wt_model_txn_prepare_both(txn1, session1, 5);
    wt_model_txn_prepare_both(txn2, session2, 5);
    wt_model_txn_commit_both(txn1, session1, 10, 10);
    wt_model_txn_commit_both(txn2, session2, 10, 15);
    wt_model_assert(table, uri, key1, 10);
    wt_model_assert(table, uri, key2, 10);

    /* Check transaction conflicts: Concurrent update. */
    wt_model_txn_begin_both(txn1, session1);
    wt_model_txn_begin_both(txn2, session2);
    wt_model_txn_insert_both(table, uri, txn1, session1, key1, value3);
    wt_model_txn_insert_both(table, uri, txn1, session1, key1, value4);
    wt_model_txn_prepare_both(txn1, session1, 20);
    wt_model_txn_insert_both(table, uri, txn2, session2, key1, value4); /* Rollback. */
    wt_model_txn_commit_both(txn1, session1, 20, 20);
    wt_model_txn_commit_both(txn2, session2, 20);

    wt_model_txn_begin_both(txn1, session1);
    wt_model_txn_begin_both(txn2, session2);
    wt_model_txn_insert_both(table, uri, txn1, session1, key1, value3);
    wt_model_txn_prepare_both(txn1, session1, 30);
    wt_model_txn_commit_both(txn1, session1, 30, 30);
    wt_model_txn_insert_both(table, uri, txn2, session2, key1, value4); /* Rollback. */
    wt_model_txn_commit_both(txn2, session2, 30);

    /* Check prepare conflicts: Reading a prepared, but uncommitted update. */
    wt_model_txn_begin_both(txn1, session1);
    wt_model_txn_insert_both(table, uri, txn1, session1, key1, value1);
    wt_model_txn_prepare_both(txn1, session1, 40);
    wt_model_assert(table, uri, key1);     /* Prepare conflict. */
    wt_model_assert(table, uri, key1, 40); /* Prepare conflict. */
    wt_model_assert(table, uri, key1, 39); /* Success. */
    wt_model_assert(table, uri, key1, 20); /* Success. */
    wt_model_txn_commit_both(txn1, session1, 40, 40);
    wt_model_assert(table, uri, key1, 40);

    wt_model_txn_begin_both(txn1, session1);
    wt_model_txn_insert_both(table, uri, txn1, session1, key1, value1);
    wt_model_txn_prepare_both(txn1, session1, 50);
    wt_model_txn_commit_both(txn1, session1, 50, 50);
    wt_model_assert(table, uri, key1, 50); /* Success. */

    wt_model_txn_begin_both(txn1, session1);
    wt_model_txn_insert_both(table, uri, txn1, session1, key1, value2);
    wt_model_txn_prepare_both(txn1, session1, 60);
    wt_model_txn_rollback_both(txn1, session1);
    wt_model_assert(table, uri, key1, 60); /* Success. */

    /* Verify. */
    testutil_assert(table->verify_noexcept(conn));

    /* Clean up. */
    testutil_check(session->close(session, nullptr));
    testutil_check(session1->close(session1, nullptr));
    testutil_check(session2->close(session2, nullptr));
    testutil_check(conn->close(conn, nullptr));

    /* Reopen the database. We must do this for debug log printing to work. */
    testutil_wiredtiger_open(opts, home, ENV_CONFIG, nullptr, &conn, false, false);
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session));

    /* Verify using the debug log. */
    model::kv_database db_from_debug_log;
    model::debug_log_parser::from_debug_log(db_from_debug_log, conn);
    testutil_assert(db_from_debug_log.table("table")->verify_noexcept(conn));

    /* Print the debug log to JSON. */
    std::string tmp_json = create_tmp_file(home, "debug-log-", ".json");
    wt_print_debug_log(conn, tmp_json.c_str());

    /* Verify using the debug log JSON. */
    model::kv_database db_from_debug_log_json;
    model::debug_log_parser::from_json(db_from_debug_log_json, tmp_json.c_str());
    testutil_assert(db_from_debug_log_json.table("table")->verify_noexcept(conn));

    /* Now try to get the verification to fail. */
    wt_remove(session, uri, key2, 1000);
    testutil_assert(!db_from_debug_log.table("table")->verify_noexcept(conn));

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

    /*
     * Tests.
     */
    try {
        ret = EXIT_SUCCESS;
        test_transaction_basic();
        test_transaction_basic_wt();
        test_transaction_prepared();
        test_transaction_prepared_wt();
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
