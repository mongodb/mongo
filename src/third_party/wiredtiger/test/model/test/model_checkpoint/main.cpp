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

/* Keys. */
const model::data_value key1("Key 1");
const model::data_value key2("Key 2");
const model::data_value key3("Key 3");
const model::data_value key4("Key 4");
const model::data_value key5("Key 5");
const model::data_value key6("Key 6");

/* Values. */
const model::data_value value1("Value 1");
const model::data_value value2("Value 2");
const model::data_value value3("Value 3");
const model::data_value value4("Value 4");
const model::data_value value5("Value 5");

/*
 * test_checkpoint --
 *     The basic test of the checkpoint model.
 */
static void
test_checkpoint(void)
{
    model::kv_database database;
    model::kv_table_ptr table = database.create_table("table");

    /* Transactions. */
    model::kv_transaction_ptr txn1, txn2;

    /* Throwaway out parameter. */
    model::data_value v;

    /* Add some data. */
    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key1, value1));
    txn1->commit(10);
    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key2, value2));
    txn1->commit(20);

    /* Create a named checkpoint. */
    model::kv_checkpoint_ptr ckpt1 = database.create_checkpoint("ckpt1");

    /* Set the stable timestamp and create an unnamed checkpoint. */
    testutil_check(database.set_stable_timestamp(15));
    model::kv_checkpoint_ptr ckpt = database.create_checkpoint();

    /* Add more data. */
    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key3, value3));
    txn1->commit(30);

    /* Verify that we have the data that we expect. */
    testutil_assert(table->get(ckpt1, key1) == value1);
    testutil_assert(table->get(ckpt1, key2) == value2); /* The stable timestamp is not yet set. */
    testutil_assert(table->get(ckpt1, key3) == model::NONE);
    testutil_assert(table->get(ckpt, key1) == value1);
    testutil_assert(table->get(ckpt, key2) == model::NONE);
    testutil_assert(table->get(ckpt, key3) == model::NONE);

    /* Verify that we have the data that we expect - with read timestamps. */
    testutil_assert(table->get(ckpt1, key1, 15) == value1);
    testutil_assert(table->get(ckpt1, key2, 15) == model::NONE);
    testutil_assert(table->get(ckpt1, key3, 15) == model::NONE);

    /* Add two more keys; check that only that the latest committed data are included. */
    txn1 = database.begin_transaction();
    txn2 = database.begin_transaction();
    testutil_check(table->insert(txn1, key4, value3));
    testutil_check(table->insert(txn1, key4, value4));
    testutil_check(table->insert(txn2, key5, value5));
    txn1->commit(40);
    testutil_check(database.set_stable_timestamp(40));
    model::kv_checkpoint_ptr ckpt2 = database.create_checkpoint("ckpt2");
    testutil_assert(table->get(ckpt2, key3) == value3);
    testutil_assert(table->get(ckpt2, key4) == value4);
    testutil_assert(table->get(ckpt2, key5) == model::NONE);
    txn2->commit(50);

    /* Check contains_any. */
    testutil_assert(!table->contains_any(ckpt2, key4, value1));
    testutil_assert(!table->contains_any(ckpt2, key4, value2));
    testutil_assert(table->contains_any(ckpt2, key4, value3));
    testutil_assert(table->contains_any(ckpt2, key4, value4));
    testutil_assert(!table->contains_any(ckpt2, key5, value5));

    /* Test with prepared transactions. */
    txn1 = database.begin_transaction();
    txn2 = database.begin_transaction();
    testutil_check(table->insert(txn1, key1, value4));
    testutil_check(table->insert(txn2, key2, value5));
    txn1->prepare(55);
    txn2->prepare(55);
    txn1->commit(60, 60);
    txn2->commit(60, 65);
    testutil_check(database.set_stable_timestamp(60));
    model::kv_checkpoint_ptr ckpt3 = database.create_checkpoint("ckpt3");
    testutil_assert(table->get(ckpt3, key1) == value4);
    testutil_assert(table->get(ckpt3, key2) == value2); /* The old value. */
    testutil_assert(table->get(ckpt3, key3) == value3);

    /* Test moving the stable timestamp backwards - this should fail. */
    testutil_assert(database.set_stable_timestamp(50) == EINVAL);
    testutil_assert(database.stable_timestamp() == 60);
    model::kv_checkpoint_ptr ckpt4 = database.create_checkpoint("ckpt4");
    testutil_assert(table->get(ckpt4, key1) == value4);
    testutil_assert(table->get(ckpt4, key2) == value2);
    testutil_assert(table->get(ckpt4, key3) == value3);

    /* Test illegal update behaviors. */
    testutil_check(database.set_stable_timestamp(60));
    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key1, value1));
    model_testutil_assert_exception(txn1->prepare(60), model::wiredtiger_abort_exception);
    txn1->rollback();

    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key1, value1));
    txn1->prepare(62);
    testutil_check(database.set_stable_timestamp(62));
    model_testutil_assert_exception(txn1->commit(60, 62), model::wiredtiger_abort_exception);
    txn1->rollback();

    txn1 = database.begin_transaction();
    model_testutil_assert_exception(
      txn1->set_commit_timestamp(62), model::wiredtiger_abort_exception);
    txn1->rollback();

    /*
     * Add some data in a transaction, prepare it, checkpoint, commit, then crash. On restart, the
     * checkpoint should not have the prepared transaction's write.
     */
    /*
     * Note: this is hard run against WT in this test suite because, if WT is shut down with a
     * prepared transaction, WT will abort the entire process. See
     * test/model/workloads/WT-14832.workload to reproduce it with the runner.
     */
    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key6, value1));
    txn1->prepare(65);
    model::kv_checkpoint_ptr chkpt5 = database.checkpoint();
    txn1->commit(65, 65);
    database.restart(true);
    testutil_assert(table->get_ext(key6, v) == WT_NOTFOUND);
}

/*
 * test_checkpoint_wt --
 *     The basic test of the checkpoint model, also in WiredTiger.
 */
static void
test_checkpoint_wt(void)
{
    model::kv_database database;
    model::kv_table_ptr table = database.create_table("table");

    /* Transactions. */
    model::kv_transaction_ptr txn1, txn2;

    /* Create the test's home directory and database. */
    WT_CONNECTION *conn;
    WT_SESSION *session;
    WT_SESSION *session1;
    WT_SESSION *session2;
    const char *uri = "table:table";

    std::string test_home = std::string(home) + DIR_DELIM_STR + "checkpoint";
    testutil_recreate_dir(test_home.c_str());
    testutil_wiredtiger_open(opts, test_home.c_str(), ENV_CONFIG, nullptr, &conn, false, false);
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session));
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session1));
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session2));
    testutil_check(
      session->create(session, uri, "key_format=S,value_format=S,log=(enabled=false)"));

    testutil_assert(database.stable_timestamp() == wt_get_stable_timestamp(conn));

    /* Add some data. */
    wt_model_txn_begin_both(txn1, session1);
    wt_model_txn_insert_both(table, uri, txn1, session1, key1, value1);
    wt_model_txn_commit_both(txn1, session1, 10);
    wt_model_txn_begin_both(txn1, session1);
    wt_model_txn_insert_both(table, uri, txn1, session1, key2, value2);
    wt_model_txn_commit_both(txn1, session1, 20);

    /* Create a named checkpoint. */
    wt_model_ckpt_create_both("ckpt1");

    /* Set the stable timestamp and create an unnamed checkpoint. */
    wt_model_set_stable_timestamp_both(15);
    wt_model_ckpt_create_both(nullptr);

    /* Add more data. */
    wt_model_txn_begin_both(txn1, session1);
    wt_model_txn_insert_both(table, uri, txn1, session1, key3, value3);
    wt_model_txn_commit_both(txn1, session1, 30);

    /* Verify that we have the data that we expect. */
    wt_model_ckpt_assert(table, uri, "ckpt1", key1);
    wt_model_ckpt_assert(table, uri, "ckpt1", key2);
    wt_model_ckpt_assert(table, uri, "ckpt1", key3);
    wt_model_ckpt_assert(table, uri, nullptr, key1);
    wt_model_ckpt_assert(table, uri, nullptr, key2);
    wt_model_ckpt_assert(table, uri, nullptr, key3);

    /* Verify that we have the data that we expect - with read timestamps. */
    wt_model_ckpt_assert(table, uri, "ckpt1", key1, 15);
    wt_model_ckpt_assert(table, uri, "ckpt1", key2, 15);
    wt_model_ckpt_assert(table, uri, "ckpt1", key3, 15);

    /* Add two more keys; check that only that the latest committed data are included. */
    wt_model_txn_begin_both(txn1, session1);
    wt_model_txn_begin_both(txn2, session2);
    wt_model_txn_insert_both(table, uri, txn1, session1, key4, value3);
    wt_model_txn_insert_both(table, uri, txn1, session1, key4, value4);
    wt_model_txn_insert_both(table, uri, txn2, session2, key5, value5);
    wt_model_txn_commit_both(txn1, session1, 40);
    wt_model_set_stable_timestamp_both(40);
    wt_model_ckpt_create_both("ckpt2");
    wt_model_ckpt_assert(table, uri, "ckpt2", key3);
    wt_model_ckpt_assert(table, uri, "ckpt2", key4);
    wt_model_ckpt_assert(table, uri, "ckpt2", key5);
    wt_model_txn_commit_both(txn2, session2, 50);

    /* Test with prepared transactions. */
    wt_model_txn_begin_both(txn1, session1);
    wt_model_txn_begin_both(txn2, session2);
    wt_model_txn_insert_both(table, uri, txn1, session1, key1, value4);
    wt_model_txn_insert_both(table, uri, txn2, session2, key2, value5);
    wt_model_txn_prepare_both(txn1, session1, 55);
    wt_model_txn_prepare_both(txn2, session2, 55);
    wt_model_txn_commit_both(txn1, session1, 60, 60);
    wt_model_txn_commit_both(txn2, session2, 60, 65);
    wt_model_set_stable_timestamp_both(60);
    wt_model_ckpt_create_both("ckpt3");
    wt_model_ckpt_assert(table, uri, "ckpt3", key1);
    wt_model_ckpt_assert(table, uri, "ckpt3", key2);
    wt_model_ckpt_assert(table, uri, "ckpt3", key3);

    /* Test moving the stable timestamp backwards - this should fail. */
    wt_model_set_stable_timestamp_both(50);
    testutil_assert(database.stable_timestamp() == wt_get_stable_timestamp(conn));
    wt_model_ckpt_create_both("ckpt4");
    wt_model_ckpt_assert(table, uri, "ckpt4", key1);
    wt_model_ckpt_assert(table, uri, "ckpt4", key2);
    wt_model_ckpt_assert(table, uri, "ckpt4", key3);

    /* Verify. */
    wt_model_set_stable_timestamp_both(65); /* Advance the timestamp to the very end. */
    testutil_assert(table->verify_noexcept(conn));

    /* Verify checkpoints. */
    testutil_assert(table->verify_noexcept(conn, database.checkpoint("ckpt1")));
    testutil_assert(table->verify_noexcept(conn, database.checkpoint("ckpt2")));
    testutil_assert(table->verify_noexcept(conn, database.checkpoint("ckpt3")));
    testutil_assert(table->verify_noexcept(conn, database.checkpoint("ckpt4")));

    /* Clean up. */
    testutil_check(session->close(session, nullptr));
    testutil_check(session1->close(session1, nullptr));
    testutil_check(session2->close(session2, nullptr));
    testutil_check(conn->close(conn, nullptr));

    /* Reopen the database. We must do this for debug log printing to work. */
    testutil_wiredtiger_open(opts, test_home.c_str(), ENV_CONFIG, nullptr, &conn, false, false);
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session));

    /* Verify using the debug log. */
    model::kv_database db_from_debug_log;
    model::debug_log_parser::from_debug_log(db_from_debug_log, conn);
    testutil_assert(db_from_debug_log.table("table")->verify_noexcept(conn));

    /* Print the debug log to JSON. */
    std::string tmp_json = create_tmp_file(test_home.c_str(), "debug-log-", ".json");
    wt_print_debug_log(conn, tmp_json.c_str());

    /* Verify using the debug log JSON. */
    model::kv_database db_from_debug_log_json;
    model::debug_log_parser::from_json(db_from_debug_log_json, tmp_json.c_str());
    testutil_assert(db_from_debug_log_json.table("table")->verify_noexcept(conn));

    /* Verify checkpoints. */
    testutil_assert(db_from_debug_log.table("table")->verify_noexcept(
      conn, db_from_debug_log.checkpoint("ckpt1")));
    testutil_assert(db_from_debug_log.table("table")->verify_noexcept(
      conn, db_from_debug_log.checkpoint("ckpt2")));
    testutil_assert(db_from_debug_log.table("table")->verify_noexcept(
      conn, db_from_debug_log.checkpoint("ckpt3")));
    testutil_assert(db_from_debug_log.table("table")->verify_noexcept(
      conn, db_from_debug_log.checkpoint("ckpt4")));

    /* Verify checkpoints - using the debug log JSON. */
    testutil_assert(db_from_debug_log_json.table("table")->verify_noexcept(
      conn, db_from_debug_log_json.checkpoint("ckpt1")));
    testutil_assert(db_from_debug_log_json.table("table")->verify_noexcept(
      conn, db_from_debug_log_json.checkpoint("ckpt2")));
    testutil_assert(db_from_debug_log_json.table("table")->verify_noexcept(
      conn, db_from_debug_log_json.checkpoint("ckpt3")));
    testutil_assert(db_from_debug_log_json.table("table")->verify_noexcept(
      conn, db_from_debug_log_json.checkpoint("ckpt4")));

    /* Clean up. */
    testutil_check(session->close(session, nullptr));
    testutil_check(conn->close(conn, nullptr));
}

/*
 * test_checkpoint_restart_wt --
 *     Check loading checkpoints with database restarts.
 */
static void
test_checkpoint_restart_wt(void)
{
    model::kv_database database;
    model::kv_table_ptr table = database.create_table("table");

    /* Create the test's home directory and database. */
    WT_CONNECTION *conn;
    WT_SESSION *session, *session2;
    const char *uri = "table:table";

    std::string test_home = std::string(home) + DIR_DELIM_STR + "checkpoint-restart";
    testutil_recreate_dir(test_home.c_str());
    testutil_wiredtiger_open(opts, test_home.c_str(), ENV_CONFIG, nullptr, &conn, false, false);
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session));
    testutil_check(
      session->create(session, uri, "key_format=S,value_format=S,log=(enabled=false)"));

    /* Transaction. */
    model::kv_transaction_ptr txn;

    /* Add some data. */
    wt_model_txn_begin_both(txn, session);
    wt_model_txn_insert_both(table, uri, txn, session, key1, value1);
    wt_model_txn_commit_both(txn, session, 10);
    wt_model_txn_begin_both(txn, session);
    wt_model_txn_insert_both(table, uri, txn, session, key2, value2);
    wt_model_txn_commit_both(txn, session, 20);

    /* Create a named checkpoint. */
    wt_model_set_stable_timestamp_both(15);
    wt_model_ckpt_create_both("ckpt1");

    /* Add some data. */
    wt_model_txn_begin_both(txn, session);
    wt_model_txn_insert_both(table, uri, txn, session, key3, value3);
    wt_model_txn_commit_both(txn, session, 30);
    wt_model_txn_begin_both(txn, session);
    wt_model_txn_insert_both(table, uri, txn, session, key4, value4);
    wt_model_txn_commit_both(txn, session, 40);

    /* Create a named checkpoint. */
    wt_model_set_stable_timestamp_both(35);
    wt_model_ckpt_create_both("ckpt2");

    /* Create a nameless checkpoint and restart. */
    wt_model_set_stable_timestamp_both(40);
    wt_model_ckpt_create_both(nullptr);
    testutil_check(session->close(session, nullptr));
    testutil_check(conn->close(conn, nullptr));
    testutil_wiredtiger_open(opts, test_home.c_str(), ENV_CONFIG, nullptr, &conn, false, false);
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session));

    /* Add some data. */
    wt_model_txn_begin_both(txn, session);
    wt_model_txn_insert_both(table, uri, txn, session, key1, value2);
    wt_model_txn_commit_both(txn, session, 50);
    wt_model_txn_begin_both(txn, session);
    wt_model_txn_insert_both(table, uri, txn, session, key2, value3);
    wt_model_txn_commit_both(txn, session, 60);

    /* Create a named checkpoint. */
    wt_model_set_stable_timestamp_both(55);
    wt_model_ckpt_create_both("ckpt3");

    /* Add some data. */
    wt_model_txn_begin_both(txn, session);
    wt_model_txn_insert_both(table, uri, txn, session, key3, value4);
    wt_model_txn_commit_both(txn, session, 70);

    /* Add some data. Take a checkpoint while a transaction is still running. */
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session2));
    wt_model_txn_begin_both(txn, session2);
    wt_model_txn_insert_both(table, uri, txn, session2, key4, value5);
    wt_model_set_stable_timestamp_both(75);
    wt_model_ckpt_create_both("ckpt4");
    wt_model_txn_commit_both(txn, session2, 80);
    testutil_check(session2->close(session2, nullptr));

    /* Create a nameless checkpoint and restart. */
    wt_model_set_stable_timestamp_both(80);
    wt_model_ckpt_create_both(nullptr);
    testutil_check(session->close(session, nullptr));
    testutil_check(conn->close(conn, nullptr));
    testutil_wiredtiger_open(opts, test_home.c_str(), ENV_CONFIG, nullptr, &conn, false, false);
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session));

    /* Add some data - use prepared transactions. */
    wt_model_txn_begin_both(txn, session);
    wt_model_txn_insert_both(table, uri, txn, session, key1, value3);
    wt_model_txn_prepare_both(txn, session, 90);
    wt_model_txn_commit_both(txn, session, 94, 98);
    wt_model_txn_begin_both(txn, session);
    wt_model_txn_insert_both(table, uri, txn, session, key2, value4);
    wt_model_txn_prepare_both(txn, session, 100);
    wt_model_txn_commit_both(txn, session, 104, 108);

    /* Create a named checkpoint. */
    wt_model_set_stable_timestamp_both(95);
    wt_model_ckpt_create_both("ckpt5");

    /* Add some data - use prepared transactions. */
    wt_model_txn_begin_both(txn, session);
    wt_model_txn_insert_both(table, uri, txn, session, key3, value5);
    wt_model_txn_prepare_both(txn, session, 110);
    wt_model_txn_commit_both(txn, session, 114, 118);
    wt_model_txn_begin_both(txn, session);
    wt_model_txn_insert_both(table, uri, txn, session, key4, value1);
    wt_model_txn_prepare_both(txn, session, 120);
    wt_model_txn_commit_both(txn, session, 124, 128);

    /* Create a named checkpoint. */
    wt_model_set_stable_timestamp_both(115);
    wt_model_ckpt_create_both("ckpt6");

    /* Create a nameless checkpoint and restart. */
    wt_model_set_stable_timestamp_both(129);
    wt_model_ckpt_create_both(nullptr);
    testutil_check(session->close(session, nullptr));
    testutil_check(conn->close(conn, nullptr));
    testutil_wiredtiger_open(opts, test_home.c_str(), ENV_CONFIG, nullptr, &conn, false, false);
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session));

    /* Verify using the debug log. */
    model::kv_database db_from_debug_log;
    model::debug_log_parser::from_debug_log(db_from_debug_log, conn);
    model::kv_table_ptr t = db_from_debug_log.table("table");
    testutil_assert(t->verify_noexcept(conn));
    testutil_assert(t->verify_noexcept(conn, db_from_debug_log.checkpoint("ckpt1")));
    testutil_assert(t->verify_noexcept(conn, db_from_debug_log.checkpoint("ckpt2")));
    testutil_assert(t->verify_noexcept(conn, db_from_debug_log.checkpoint("ckpt3")));
    testutil_assert(t->verify_noexcept(conn, db_from_debug_log.checkpoint("ckpt4")));
    testutil_assert(t->verify_noexcept(conn, db_from_debug_log.checkpoint("ckpt5")));
    testutil_assert(t->verify_noexcept(conn, db_from_debug_log.checkpoint("ckpt6")));

    /* Clean up. */
    testutil_check(session->close(session, nullptr));
    testutil_check(conn->close(conn, nullptr));
}

/*
 * test_checkpoint_logged --
 *     The basic test of the checkpoint model with logged tables.
 */
static void
test_checkpoint_logged(void)
{
    model::kv_database database;

    model::kv_table_config table_config;
    table_config.log_enabled = true;
    model::kv_table_ptr table = database.create_table("table", table_config);

    /* Transactions. */
    model::kv_transaction_ptr txn1, txn2;

    /* Add some data. */
    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key1, value1));
    txn1->commit(10);
    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key2, value2));
    txn1->commit(20);

    /* Create a named checkpoint. */
    model::kv_checkpoint_ptr ckpt1 = database.create_checkpoint("ckpt1");

    /* Set the stable timestamp and create an unnamed checkpoint. */
    testutil_check(database.set_stable_timestamp(15));
    model::kv_checkpoint_ptr ckpt = database.create_checkpoint();

    /* Add more data. */
    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key3, value3));
    txn1->commit(30);

    /* Verify that we have the data that we expect. */
    testutil_assert(table->get(ckpt1, key1) == value1);
    testutil_assert(table->get(ckpt1, key2) == value2); /* The stable timestamp is not yet set. */
    testutil_assert(table->get(ckpt1, key3) == model::NONE);
    testutil_assert(table->get(ckpt, key1) == value1);
    testutil_assert(table->get(ckpt, key2) == value2); /* The stable timestamp is ignored. */
    testutil_assert(table->get(ckpt, key3) == model::NONE);

    /* Verify that we have the data that we expect - with read timestamps. */
    testutil_assert(table->get(ckpt1, key1, 15) == value1);
    testutil_assert(table->get(ckpt1, key2, 15) == value2);
    testutil_assert(table->get(ckpt1, key3, 15) == model::NONE);

    /* Add two more keys; check that only that the latest committed data are included. */
    txn1 = database.begin_transaction();
    txn2 = database.begin_transaction();
    testutil_check(table->insert(txn1, key4, value3));
    testutil_check(table->insert(txn1, key4, value4));
    testutil_check(table->insert(txn2, key5, value5));
    txn1->commit(40);
    testutil_check(database.set_stable_timestamp(40));
    model::kv_checkpoint_ptr ckpt2 = database.create_checkpoint("ckpt2");
    testutil_assert(table->get(ckpt2, key3) == value3);
    testutil_assert(table->get(ckpt2, key4) == value4);
    testutil_assert(table->get(ckpt2, key5) == model::NONE);
    txn2->commit(50);

    /* Check contains_any. */
    testutil_assert(!table->contains_any(ckpt2, key4, value1));
    testutil_assert(!table->contains_any(ckpt2, key4, value2));
    testutil_assert(table->contains_any(ckpt2, key4, value3));
    testutil_assert(table->contains_any(ckpt2, key4, value4));
    testutil_assert(!table->contains_any(ckpt2, key5, value5));
}

/*
 * test_checkpoint_logged_wt --
 *     The basic test of the checkpoint model with logged tables, also in WiredTiger.
 */
static void
test_checkpoint_logged_wt(void)
{
    model::kv_database database;

    model::kv_table_config table_config;
    table_config.log_enabled = true;
    model::kv_table_ptr table = database.create_table("table", table_config);

    /* Transactions. */
    model::kv_transaction_ptr txn1, txn2;

    /* Create the test's home directory and database. */
    WT_CONNECTION *conn;
    WT_SESSION *session;
    WT_SESSION *session1;
    WT_SESSION *session2;
    const char *uri = "table:table";

    std::string test_home = std::string(home) + DIR_DELIM_STR + "logged";
    testutil_recreate_dir(test_home.c_str());
    testutil_wiredtiger_open(opts, test_home.c_str(), ENV_CONFIG, nullptr, &conn, false, false);
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session));
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session1));
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session2));
    testutil_check(session->create(session, uri, "key_format=S,value_format=S,log=(enabled=true)"));

    testutil_assert(database.stable_timestamp() == wt_get_stable_timestamp(conn));

    /* Add some data. */
    wt_model_txn_begin_both(txn1, session1);
    wt_model_txn_insert_both(table, uri, txn1, session1, key1, value1);
    wt_model_txn_commit_both(txn1, session1);
    wt_model_txn_begin_both(txn1, session1);
    wt_model_txn_insert_both(table, uri, txn1, session1, key2, value2);
    wt_model_txn_commit_both(txn1, session1);

    /* Create a named checkpoint. */
    wt_model_ckpt_create_both("ckpt1");

    /* Set the stable timestamp and create an unnamed checkpoint. */
    wt_model_set_stable_timestamp_both(15);
    wt_model_ckpt_create_both(nullptr);

    /* Add more data. */
    wt_model_txn_begin_both(txn1, session1);
    wt_model_txn_insert_both(table, uri, txn1, session1, key3, value3);
    wt_model_txn_commit_both(txn1, session1);

    /* Verify that we have the data that we expect. */
    wt_model_ckpt_assert(table, uri, "ckpt1", key1);
    wt_model_ckpt_assert(table, uri, "ckpt1", key2);
    wt_model_ckpt_assert(table, uri, "ckpt1", key3);
    wt_model_ckpt_assert(table, uri, nullptr, key1);
    wt_model_ckpt_assert(table, uri, nullptr, key2);
    wt_model_ckpt_assert(table, uri, nullptr, key3);

    /* Verify that we have the data that we expect - with read timestamps. */
    wt_model_ckpt_assert(table, uri, "ckpt1", key1, 15);
    wt_model_ckpt_assert(table, uri, "ckpt1", key2, 15);
    wt_model_ckpt_assert(table, uri, "ckpt1", key3, 15);

    /* Add two more keys; check that only that the latest committed data are included. */
    wt_model_txn_begin_both(txn1, session1);
    wt_model_txn_begin_both(txn2, session2);
    wt_model_txn_insert_both(table, uri, txn1, session1, key4, value3);
    wt_model_txn_insert_both(table, uri, txn1, session1, key4, value4);
    wt_model_txn_insert_both(table, uri, txn2, session2, key5, value5);
    wt_model_txn_commit_both(txn1, session1);
    wt_model_set_stable_timestamp_both(40);
    wt_model_ckpt_create_both("ckpt2");
    wt_model_ckpt_assert(table, uri, "ckpt2", key3);
    wt_model_ckpt_assert(table, uri, "ckpt2", key4);
    wt_model_ckpt_assert(table, uri, "ckpt2", key5);
    wt_model_txn_commit_both(txn2, session2);

    /* Verify. */
    testutil_assert(table->verify_noexcept(conn));

    /* Verify checkpoints. */
    testutil_assert(table->verify_noexcept(conn, database.checkpoint("ckpt1")));
    testutil_assert(table->verify_noexcept(conn, database.checkpoint("ckpt2")));

    /* Clean up. */
    testutil_check(session->close(session, nullptr));
    testutil_check(session1->close(session1, nullptr));
    testutil_check(session2->close(session2, nullptr));
    testutil_check(conn->close(conn, nullptr));

    /* Reopen the database. We must do this for debug log printing to work. */
    testutil_wiredtiger_open(opts, test_home.c_str(), ENV_CONFIG, nullptr, &conn, false, false);
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session));

    /* Verify using the debug log. */
    model::kv_database db_from_debug_log;
    model::debug_log_parser::from_debug_log(db_from_debug_log, conn);
    testutil_assert(db_from_debug_log.table("table")->verify_noexcept(conn));

    /* Print the debug log to JSON. */
    std::string tmp_json = create_tmp_file(test_home.c_str(), "debug-log-", ".json");
    wt_print_debug_log(conn, tmp_json.c_str());

    /* Verify using the debug log JSON. */
    model::kv_database db_from_debug_log_json;
    model::debug_log_parser::from_json(db_from_debug_log_json, tmp_json.c_str());
    testutil_assert(db_from_debug_log_json.table("table")->verify_noexcept(conn));

    /* Verify checkpoints. */
    testutil_assert(db_from_debug_log.table("table")->verify_noexcept(
      conn, db_from_debug_log.checkpoint("ckpt1")));
    testutil_assert(db_from_debug_log.table("table")->verify_noexcept(
      conn, db_from_debug_log.checkpoint("ckpt2")));

    /* Verify checkpoints - using the debug log JSON. */
    testutil_assert(db_from_debug_log_json.table("table")->verify_noexcept(
      conn, db_from_debug_log_json.checkpoint("ckpt1")));
    testutil_assert(db_from_debug_log_json.table("table")->verify_noexcept(
      conn, db_from_debug_log_json.checkpoint("ckpt2")));

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
    testutil_recreate_dir(home);

    /*
     * Tests.
     */
    try {
        ret = EXIT_SUCCESS;
        test_checkpoint();
        test_checkpoint_wt();
        test_checkpoint_restart_wt();
        test_checkpoint_logged();
        test_checkpoint_logged_wt();
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
