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
#include "model/test/subprocess.h"
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
static const model::data_value key1("Key 1");
static const model::data_value key2("Key 2");
static const model::data_value key3("Key 3");
static const model::data_value key4("Key 4");
static const model::data_value key5("Key 5");

/* Values. */
static const model::data_value value1("Value 1");
static const model::data_value value2("Value 2");
static const model::data_value value3("Value 3");
static const model::data_value value4("Value 4");
static const model::data_value value5("Value 5");

/*
 * test_rts --
 *     The basic test of the RTS model.
 */
static void
test_rts(void)
{
    model::kv_database database;
    model::kv_table_ptr table = database.create_table("table");

    /* Transactions. */
    model::kv_transaction_ptr txn1, txn2;

    /* Test RTS before setting the stable timestamp. */
    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key1, value1));
    txn1->commit(5);
    database.rollback_to_stable();
    testutil_assert(table->get(key1) == value1);

    /* Add some data. */
    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key1, value1));
    txn1->commit(10);
    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key2, value2));
    txn1->commit(20);

    /* Set the stable timestamp, do RTS, and verify. */
    testutil_check(database.set_stable_timestamp(15));
    database.rollback_to_stable();
    testutil_assert(table->get(key1) == value1);
    testutil_assert(table->get(key2) == model::NONE);

    /* Add some data with lower timestamp than would be possible before RTS. */
    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key2, value2));
    txn1->commit(18);

    /* Test illegal behaviors. */
    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key1, value1));
    model_testutil_assert_exception(database.rollback_to_stable(), model::model_exception);
    txn1->rollback();

    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key1, value1));
    txn1->prepare(1000);
    model_testutil_assert_exception(database.rollback_to_stable(), model::model_exception);
    txn1->rollback();
}

/*
 * test_rts_wt --
 *     The basic test of the RTS model, also in WiredTiger.
 */
static void
test_rts_wt(void)
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

    std::string test_home = std::string(home) + DIR_DELIM_STR + "rts";
    testutil_recreate_dir(test_home.c_str());
    testutil_wiredtiger_open(opts, test_home.c_str(), ENV_CONFIG, nullptr, &conn, false, false);
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session));
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session1));
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session2));
    testutil_check(
      session->create(session, uri, "key_format=S,value_format=S,log=(enabled=false)"));

    /* Test RTS before setting the stable timestamp. */
    wt_model_txn_begin_both(txn1, session1);
    wt_model_txn_insert_both(table, uri, txn1, session1, key1, value1);
    wt_model_txn_commit_both(txn1, session1, 5);
    wt_model_rollback_to_stable_both();
    wt_model_assert(table, uri, key1);

    /* Add some data. */
    wt_model_txn_begin_both(txn1, session1);
    wt_model_txn_insert_both(table, uri, txn1, session1, key1, value1);
    wt_model_txn_commit_both(txn1, session1, 10);
    wt_model_txn_begin_both(txn1, session1);
    wt_model_txn_insert_both(table, uri, txn1, session1, key2, value2);
    wt_model_txn_commit_both(txn1, session1, 20);

    /* Set the stable timestamp, do RTS, and verify. */
    wt_model_set_stable_timestamp_both(15);
    wt_model_rollback_to_stable_both();
    wt_model_assert(table, uri, key1);
    wt_model_assert(table, uri, key2);

    /* Add some data with lower timestamp than would be possible before RTS. */
    wt_model_txn_begin_both(txn1, session1);
    wt_model_txn_insert_both(table, uri, txn1, session1, key2, value2);
    wt_model_txn_commit_both(txn1, session1, 18);

    /* Clean up. */
    testutil_check(session->close(session, nullptr));
    testutil_check(session1->close(session1, nullptr));
    testutil_check(session2->close(session2, nullptr));
    testutil_check(conn->close(conn, nullptr));

    /* Verify using the debug log. */
    verify_using_debug_log(opts, test_home.c_str(), true);
}

/*
 * test_rts_crash_wt --
 *     The RTS followed by a crash, also in WiredTiger.
 */
static void
test_rts_crash_wt(void)
{
    const char *uri = "table:table";
    model::kv_database database;
    model::kv_table_ptr table = database.create_table("table");
    model::kv_transaction_ptr txn1;

    /* Add some data. */
    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key1, value1));
    txn1->commit(10);
    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key1, value2));
    txn1->commit(20);
    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key1, value3));
    txn1->commit(30);

    /* Set the stable timestamp, rollback to stable, crash, and verify. */
    testutil_check(database.set_stable_timestamp(20));
    database.rollback_to_stable();
    database.crash();
    testutil_assert(database.stable_timestamp() == 20);
    testutil_assert(table->get(key1) == value2);

    /* Repeat in WiredTiger. */
    std::string test_home = std::string(home) + DIR_DELIM_STR + "rts-crash";
    testutil_recreate_dir(test_home.c_str());
    in_subprocess_abort
    {
        WT_CONNECTION *conn;
        WT_SESSION *session;
        WT_SESSION *session1;

        testutil_wiredtiger_open(opts, test_home.c_str(), ENV_CONFIG, nullptr, &conn, false, false);
        testutil_check(conn->open_session(conn, nullptr, nullptr, &session));
        testutil_check(conn->open_session(conn, nullptr, nullptr, &session1));
        testutil_check(
          session->create(session, uri, "key_format=S,value_format=S,log=(enabled=false)"));

        /* Add some data. */
        wt_txn_begin(session1);
        wt_txn_insert(session1, uri, key1, value1);
        wt_txn_commit(session1, 10);
        wt_txn_begin(session1);
        wt_txn_insert(session1, uri, key1, value2);
        wt_txn_commit(session1, 20);
        wt_txn_begin(session1);
        wt_txn_insert(session1, uri, key1, value3);
        wt_txn_commit(session1, 30);

        /* Set the stable timestamp, rollback to stable, and crash. */
        wt_set_stable_timestamp(conn, 20);
        wt_rollback_to_stable(conn);
    }

    WT_CONNECTION *conn;
    WT_SESSION *session;
    WT_SESSION *session1;

    /* Reopen and verify. */
    testutil_wiredtiger_open(opts, test_home.c_str(), ENV_CONFIG, nullptr, &conn, false, false);
    testutil_assert(wt_get_stable_timestamp(conn) == database.stable_timestamp());
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session));
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session1));
    wt_model_assert(table, uri, key1);

    /* Add some data with lower timestamp than would be possible before RTS. */
    wt_model_txn_begin_both(txn1, session1);
    wt_model_txn_insert_both(table, uri, txn1, session1, key1, value4);
    wt_model_txn_commit_both(txn1, session1, 25);
    wt_model_assert(table, uri, key1);

    /* Clean up. */
    testutil_check(session->close(session, nullptr));
    testutil_check(session1->close(session1, nullptr));
    testutil_check(conn->close(conn, nullptr));

    /* Verify using the debug log. */
    verify_using_debug_log(opts, test_home.c_str(), true);
}

/*
 * test_restart_wt1 --
 *     Restart scenario 1: No explicit checkpoint.
 */
static void
test_restart_wt1(void)
{
    const char *uri = "table:table";
    model::kv_database database;
    model::kv_table_ptr table = database.create_table("table");
    model::kv_transaction_ptr txn1;

    /* Add some data. */
    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key1, value1));
    txn1->commit(10);
    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key2, value2));
    txn1->commit(20);

    /* Set the stable timestamp, restart, and verify. */
    testutil_check(database.set_stable_timestamp(15));
    database.restart();
    testutil_assert(database.stable_timestamp() == 15);
    testutil_assert(table->get(key1) == value1);
    testutil_assert(table->get(key2) == model::NONE);

    /* Repeat in WiredTiger. */
    std::string test_home = std::string(home) + DIR_DELIM_STR + "restart1";
    testutil_recreate_dir(test_home.c_str());
    in_subprocess
    {
        WT_CONNECTION *conn;
        WT_SESSION *session;
        WT_SESSION *session1;

        testutil_wiredtiger_open(opts, test_home.c_str(), ENV_CONFIG, nullptr, &conn, false, false);
        testutil_check(conn->open_session(conn, nullptr, nullptr, &session));
        testutil_check(conn->open_session(conn, nullptr, nullptr, &session1));
        testutil_check(
          session->create(session, uri, "key_format=S,value_format=S,log=(enabled=false)"));

        /* Add some data. */
        wt_txn_begin(session1);
        wt_txn_insert(session1, uri, key1, value1);
        wt_txn_commit(session1, 10);
        wt_txn_begin(session1);
        wt_txn_insert(session1, uri, key2, value2);
        wt_txn_commit(session1, 20);

        /* Set the stable timestamp. */
        wt_set_stable_timestamp(conn, 15);

        /* Clean up. */
        testutil_check(session->close(session, nullptr));
        testutil_check(session1->close(session1, nullptr));
        testutil_check(conn->close(conn, nullptr));
    }

    WT_CONNECTION *conn;
    WT_SESSION *session;
    WT_SESSION *session1;

    /* Reopen and verify. */
    testutil_wiredtiger_open(opts, test_home.c_str(), ENV_CONFIG, nullptr, &conn, false, false);
    testutil_assert(wt_get_stable_timestamp(conn) == database.stable_timestamp());
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session));
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session1));
    wt_model_assert(table, uri, key1);
    wt_model_assert(table, uri, key2);

    /* Add some data with lower timestamp than would be possible before RTS. */
    wt_model_txn_begin_both(txn1, session1);
    wt_model_txn_insert_both(table, uri, txn1, session1, key2, value2);
    wt_model_txn_commit_both(txn1, session1, 18);
    wt_model_assert(table, uri, key2);

    /* Clean up. */
    testutil_check(session->close(session, nullptr));
    testutil_check(session1->close(session1, nullptr));
    testutil_check(conn->close(conn, nullptr));

    /* Verify using the debug log. */
    verify_using_debug_log(opts, test_home.c_str(), true);
}

/*
 * test_restart_wt2 --
 *     Restart scenario 2: With an explicit checkpoint.
 */
static void
test_restart_wt2(void)
{
    const char *uri = "table:table";
    model::kv_database database;
    model::kv_table_ptr table = database.create_table("table");
    model::kv_transaction_ptr txn1;

    /* Add some data. */
    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key1, value1));
    txn1->commit(10);
    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key2, value2));
    txn1->commit(20);

    /* Create an unnamed checkpoint, restart, and verify. */
    testutil_check(database.set_stable_timestamp(15));
    database.create_checkpoint();
    database.restart();
    testutil_assert(database.stable_timestamp() == 15);
    testutil_assert(table->get(key1) == value1);
    testutil_assert(table->get(key2) == model::NONE);

    /* Repeat in WiredTiger. */
    std::string test_home = std::string(home) + DIR_DELIM_STR + "restart2";
    testutil_recreate_dir(test_home.c_str());
    in_subprocess
    {
        WT_CONNECTION *conn;
        WT_SESSION *session;
        WT_SESSION *session1;

        testutil_wiredtiger_open(opts, test_home.c_str(), ENV_CONFIG, nullptr, &conn, false, false);
        testutil_check(conn->open_session(conn, nullptr, nullptr, &session));
        testutil_check(conn->open_session(conn, nullptr, nullptr, &session1));
        testutil_check(
          session->create(session, uri, "key_format=S,value_format=S,log=(enabled=false)"));

        /* Add some data. */
        wt_txn_begin(session1);
        wt_txn_insert(session1, uri, key1, value1);
        wt_txn_commit(session1, 10);
        wt_txn_begin(session1);
        wt_txn_insert(session1, uri, key2, value2);
        wt_txn_commit(session1, 20);

        /* Create an unnamed checkpoint. */
        wt_set_stable_timestamp(conn, 15);
        wt_ckpt_create(session, nullptr);

        /* Clean up. */
        testutil_check(session->close(session, nullptr));
        testutil_check(session1->close(session1, nullptr));
        testutil_check(conn->close(conn, nullptr));
    }

    WT_CONNECTION *conn;
    WT_SESSION *session;
    WT_SESSION *session1;

    /* Reopen and verify. */
    testutil_wiredtiger_open(opts, test_home.c_str(), ENV_CONFIG, nullptr, &conn, false, false);
    testutil_assert(wt_get_stable_timestamp(conn) == database.stable_timestamp());
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session));
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session1));
    wt_model_assert(table, uri, key1);
    wt_model_assert(table, uri, key2);

    /* Add some data with lower timestamp than would be possible before RTS. */
    wt_model_txn_begin_both(txn1, session1);
    wt_model_txn_insert_both(table, uri, txn1, session1, key2, value2);
    wt_model_txn_commit_both(txn1, session1, 18);
    wt_model_assert(table, uri, key2);

    /* Clean up. */
    testutil_check(session->close(session, nullptr));
    testutil_check(session1->close(session1, nullptr));
    testutil_check(conn->close(conn, nullptr));

    /* Verify using the debug log. */
    verify_using_debug_log(opts, test_home.c_str(), true);
}

/*
 * test_restart_wt3 --
 *     Restart scenario 3: Exit while having active transactions.
 */
static void
test_restart_wt3(void)
{
    const char *uri = "table:table";
    model::kv_database database;
    model::kv_table_ptr table = database.create_table("table");
    model::kv_transaction_ptr txn1, txn2;

    /* Add some data. */
    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key1, value1));
    txn1->commit(10);
    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key2, value2));
    txn1->commit(20);

    /* Add a concurrent transaction. */
    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key3, value3));
    testutil_check(table->insert(txn1, key4, value4));

    /* Add a prepared transaction. */
    txn2 = database.begin_transaction();
    testutil_check(table->insert(txn2, key5, value5));
    txn2->prepare(14);

    /* Create an unnamed checkpoint, restart, and verify. */
    testutil_check(database.set_stable_timestamp(15));
    database.create_checkpoint();
    database.restart();
    testutil_assert(database.stable_timestamp() == 15);
    testutil_assert(table->get(key1) == value1);
    testutil_assert(table->get(key2) == model::NONE);
    testutil_assert(table->get(key3) == model::NONE);
    testutil_assert(table->get(key4) == model::NONE);
    testutil_assert(table->get(key5) == model::NONE);

    /* Repeat in WiredTiger. */
    std::string test_home = std::string(home) + DIR_DELIM_STR + "restart3";
    testutil_recreate_dir(test_home.c_str());
    in_subprocess
    {
        WT_CONNECTION *conn;
        WT_SESSION *session;
        WT_SESSION *session1;
        WT_SESSION *session2;

        testutil_wiredtiger_open(opts, test_home.c_str(), ENV_CONFIG, nullptr, &conn, false, false);
        testutil_check(conn->open_session(conn, nullptr, nullptr, &session));
        testutil_check(conn->open_session(conn, nullptr, nullptr, &session1));
        testutil_check(conn->open_session(conn, nullptr, nullptr, &session2));
        testutil_check(
          session->create(session, uri, "key_format=S,value_format=S,log=(enabled=false)"));

        /* Add some data. */
        wt_txn_begin(session1);
        wt_txn_insert(session1, uri, key1, value1);
        wt_txn_commit(session1, 10);
        wt_txn_begin(session1);
        wt_txn_insert(session1, uri, key2, value2);
        wt_txn_commit(session1, 20);

        /* Add a concurrent transaction. */
        wt_txn_begin(session1);
        wt_txn_insert(session1, uri, key3, value3);
        wt_txn_insert(session1, uri, key4, value4);

        /* Add a prepared transaction. */
        wt_txn_begin(session2);
        wt_txn_insert(session2, uri, key5, value5);
        wt_txn_prepare(session2, 14);

        /* Create an unnamed checkpoint. */
        wt_set_stable_timestamp(conn, 15);
        wt_ckpt_create(session, nullptr);

        /* Clean up. */
        testutil_check(session->close(session, nullptr));
        testutil_check(session1->close(session1, nullptr));
        testutil_check(session2->close(session2, nullptr));
        testutil_check(conn->close(conn, nullptr));
    }

    WT_CONNECTION *conn;
    WT_SESSION *session;
    WT_SESSION *session1;

    /* Reopen and verify. */
    testutil_wiredtiger_open(opts, test_home.c_str(), ENV_CONFIG, nullptr, &conn, false, false);
    testutil_assert(wt_get_stable_timestamp(conn) == database.stable_timestamp());
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session));
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session1));
    wt_model_assert(table, uri, key1);
    wt_model_assert(table, uri, key2);
    wt_model_assert(table, uri, key3);
    wt_model_assert(table, uri, key4);
    wt_model_assert(table, uri, key5);

    /* Add some data with lower timestamp than would be possible before RTS. */
    wt_model_txn_begin_both(txn1, session1);
    wt_model_txn_insert_both(table, uri, txn1, session1, key2, value2);
    wt_model_txn_commit_both(txn1, session1, 18);
    wt_model_assert(table, uri, key2);

    /* Clean up. */
    testutil_check(session->close(session, nullptr));
    testutil_check(session1->close(session1, nullptr));
    testutil_check(conn->close(conn, nullptr));

    /* Verify using the debug log. */
    verify_using_debug_log(opts, test_home.c_str(), true);
}

/*
 * test_crash_wt1 --
 *     Crash scenario 1: No checkpoint.
 */
static void
test_crash_wt1(void)
{
    const char *uri = "table:table";
    model::kv_database database;
    model::kv_table_ptr table = database.create_table("table");
    model::kv_transaction_ptr txn1;

    /* Add some data. */
    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key1, value1));
    txn1->commit(10);
    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key2, value2));
    txn1->commit(20);

    /* Set the stable timestamp, crash, and verify. */
    testutil_check(database.set_stable_timestamp(15));
    database.crash();
    testutil_assert(database.stable_timestamp() == model::k_timestamp_none);
    testutil_assert(table->get(key1) == model::NONE);
    testutil_assert(table->get(key2) == model::NONE);

    /* Repeat in WiredTiger. */
    std::string test_home = std::string(home) + DIR_DELIM_STR + "crash1";
    testutil_recreate_dir(test_home.c_str());
    in_subprocess_abort
    {
        WT_CONNECTION *conn;
        WT_SESSION *session;
        WT_SESSION *session1;

        testutil_wiredtiger_open(opts, test_home.c_str(), ENV_CONFIG, nullptr, &conn, false, false);
        testutil_check(conn->open_session(conn, nullptr, nullptr, &session));
        testutil_check(conn->open_session(conn, nullptr, nullptr, &session1));
        testutil_check(
          session->create(session, uri, "key_format=S,value_format=S,log=(enabled=false)"));

        /* Add some data. */
        wt_txn_begin(session1);
        wt_txn_insert(session1, uri, key1, value1);
        wt_txn_commit(session1, 10);
        wt_txn_begin(session1);
        wt_txn_insert(session1, uri, key2, value2);
        wt_txn_commit(session1, 20);
    }

    WT_CONNECTION *conn;
    WT_SESSION *session;

    /* Reopen and verify. */
    testutil_wiredtiger_open(opts, test_home.c_str(), ENV_CONFIG, nullptr, &conn, false, false);
    testutil_assert(wt_get_stable_timestamp(conn) == database.stable_timestamp());
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session));
    wt_model_assert(table, uri, key1);
    wt_model_assert(table, uri, key2);

    /* Clean up. */
    testutil_check(session->close(session, nullptr));
    testutil_check(conn->close(conn, nullptr));

    /* Verify using the debug log. */
    verify_using_debug_log(opts, test_home.c_str(), true);
}

/*
 * test_crash_wt2 --
 *     Crash scenario 2: Basic RTS.
 */
static void
test_crash_wt2(void)
{
    const char *uri = "table:table";
    model::kv_database database;
    model::kv_table_ptr table = database.create_table("table");
    model::kv_transaction_ptr txn1;

    /* Add some data. */
    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key1, value1));
    txn1->commit(10);
    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key2, value2));
    txn1->commit(20);

    /* Create an unnamed checkpoint, crash, and verify. */
    testutil_check(database.set_stable_timestamp(15));
    database.create_checkpoint();
    testutil_check(database.set_stable_timestamp(25));
    database.crash();
    testutil_assert(database.stable_timestamp() == 15);
    testutil_assert(table->get(key1) == value1);
    testutil_assert(table->get(key2) == model::NONE);

    /* Repeat in WiredTiger. */
    std::string test_home = std::string(home) + DIR_DELIM_STR + "crash2";
    testutil_recreate_dir(test_home.c_str());
    in_subprocess_abort
    {
        WT_CONNECTION *conn;
        WT_SESSION *session;
        WT_SESSION *session1;

        testutil_wiredtiger_open(opts, test_home.c_str(), ENV_CONFIG, nullptr, &conn, false, false);
        testutil_check(conn->open_session(conn, nullptr, nullptr, &session));
        testutil_check(conn->open_session(conn, nullptr, nullptr, &session1));
        testutil_check(
          session->create(session, uri, "key_format=S,value_format=S,log=(enabled=false)"));

        /* Add some data. */
        wt_txn_begin(session1);
        wt_txn_insert(session1, uri, key1, value1);
        wt_txn_commit(session1, 10);
        wt_txn_begin(session1);
        wt_txn_insert(session1, uri, key2, value2);
        wt_txn_commit(session1, 20);

        /* Create an unnamed checkpoint. */
        wt_set_stable_timestamp(conn, 15);
        wt_ckpt_create(session, nullptr);
    }

    WT_CONNECTION *conn;
    WT_SESSION *session;
    WT_SESSION *session1;

    /* Reopen and verify. */
    testutil_wiredtiger_open(opts, test_home.c_str(), ENV_CONFIG, nullptr, &conn, false, false);
    testutil_assert(wt_get_stable_timestamp(conn) == database.stable_timestamp());
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session));
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session1));
    wt_model_assert(table, uri, key1);
    wt_model_assert(table, uri, key2);

    /* Add some data with lower timestamp than would be possible before RTS. */
    wt_model_txn_begin_both(txn1, session1);
    wt_model_txn_insert_both(table, uri, txn1, session1, key2, value2);
    wt_model_txn_commit_both(txn1, session1, 18);
    wt_model_assert(table, uri, key2);

    /* Clean up. */
    testutil_check(session->close(session, nullptr));
    testutil_check(session1->close(session1, nullptr));
    testutil_check(conn->close(conn, nullptr));

    /* Verify using the debug log. */
    verify_using_debug_log(opts, test_home.c_str(), true);
}

/*
 * test_crash_wt3 --
 *     Crash scenario 3: RTS that also has to abort active transactions.
 */
static void
test_crash_wt3(void)
{
    const char *uri = "table:table";
    model::kv_database database;
    model::kv_table_ptr table = database.create_table("table");
    model::kv_transaction_ptr txn1, txn2;

    /* Add some data. */
    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key1, value1));
    txn1->commit(10);
    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key2, value2));
    txn1->commit(20);

    /* Add a concurrent transaction. */
    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key3, value3));
    testutil_check(table->insert(txn1, key4, value4));

    /* Add a prepared transaction. */
    txn2 = database.begin_transaction();
    testutil_check(table->insert(txn2, key5, value5));
    txn2->prepare(14);

    /* Create an unnamed checkpoint, crash, and verify. */
    testutil_check(database.set_stable_timestamp(15));
    database.create_checkpoint();
    database.crash();
    testutil_assert(database.stable_timestamp() == 15);
    testutil_assert(table->get(key1) == value1);
    testutil_assert(table->get(key2) == model::NONE);
    testutil_assert(table->get(key3) == model::NONE);
    testutil_assert(table->get(key4) == model::NONE);
    testutil_assert(table->get(key5) == model::NONE);

    /* Repeat in WiredTiger. */
    std::string test_home = std::string(home) + DIR_DELIM_STR + "crash3";
    testutil_recreate_dir(test_home.c_str());
    in_subprocess_abort
    {
        WT_CONNECTION *conn;
        WT_SESSION *session;
        WT_SESSION *session1;
        WT_SESSION *session2;

        testutil_wiredtiger_open(opts, test_home.c_str(), ENV_CONFIG, nullptr, &conn, false, false);
        testutil_check(conn->open_session(conn, nullptr, nullptr, &session));
        testutil_check(conn->open_session(conn, nullptr, nullptr, &session1));
        testutil_check(conn->open_session(conn, nullptr, nullptr, &session2));
        testutil_check(
          session->create(session, uri, "key_format=S,value_format=S,log=(enabled=false)"));

        /* Add some data. */
        wt_txn_begin(session1);
        wt_txn_insert(session1, uri, key1, value1);
        wt_txn_commit(session1, 10);
        wt_txn_begin(session1);
        wt_txn_insert(session1, uri, key2, value2);
        wt_txn_commit(session1, 20);

        /* Add a concurrent transaction. */
        wt_txn_begin(session1);
        wt_txn_insert(session1, uri, key3, value3);
        wt_txn_insert(session1, uri, key4, value4);

        /* Add a prepared transaction. */
        wt_txn_begin(session2);
        wt_txn_insert(session2, uri, key5, value5);
        wt_txn_prepare(session2, 14);

        /* Create an unnamed checkpoint. */
        wt_set_stable_timestamp(conn, 15);
        wt_ckpt_create(session, nullptr);
    }

    WT_CONNECTION *conn;
    WT_SESSION *session;
    WT_SESSION *session1;

    /* Reopen and verify. */
    testutil_wiredtiger_open(opts, test_home.c_str(), ENV_CONFIG, nullptr, &conn, false, false);
    testutil_assert(wt_get_stable_timestamp(conn) == database.stable_timestamp());
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session));
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session1));
    wt_model_assert(table, uri, key1);
    wt_model_assert(table, uri, key2);
    wt_model_assert(table, uri, key3);
    wt_model_assert(table, uri, key4);
    wt_model_assert(table, uri, key5);

    /* Add some data with lower timestamp than would be possible before RTS. */
    wt_model_txn_begin_both(txn1, session1);
    wt_model_txn_insert_both(table, uri, txn1, session1, key2, value2);
    wt_model_txn_commit_both(txn1, session1, 18);
    wt_model_assert(table, uri, key2);

    /* Clean up. */
    testutil_check(session->close(session, nullptr));
    testutil_check(session1->close(session1, nullptr));
    testutil_check(conn->close(conn, nullptr));

    /* Verify using the debug log. */
    verify_using_debug_log(opts, test_home.c_str(), true);
}

/*
 * test_logged_wt --
 *     Test RTS with logged tables.
 */
static void
test_logged_wt(void)
{
    const char *uri = "table:table";
    model::kv_database database;

    model::kv_table_config table_config;
    table_config.log_enabled = true;
    model::kv_table_ptr table = database.create_table("table", table_config);

    model::kv_transaction_ptr txn1, txn2;

    /* Add some data. */
    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key1, value1));
    txn1->commit(10);
    txn1 = database.begin_transaction();
    testutil_check(table->insert(txn1, key2, value2));
    txn1->commit(20);

    /* Add a concurrent transaction. */
    txn2 = database.begin_transaction();
    testutil_check(table->insert(txn2, key3, value3));
    testutil_check(table->insert(txn2, key4, value4));

    /* Create an unnamed checkpoint, crash, and verify. */
    testutil_check(database.set_stable_timestamp(15));
    database.create_checkpoint();
    database.crash();
    testutil_assert(table->get(key1) == value1);
    testutil_assert(table->get(key2) == value2); /* RTS did not undo this insert. */
    testutil_assert(table->get(key3) == model::NONE);
    testutil_assert(table->get(key4) == model::NONE);
    testutil_assert(table->get(key5) == model::NONE);

    /* Repeat in WiredTiger. */
    std::string test_home = std::string(home) + DIR_DELIM_STR + "logged";
    testutil_recreate_dir(test_home.c_str());
    in_subprocess_abort
    {
        WT_CONNECTION *conn;
        WT_SESSION *session;
        WT_SESSION *session1;
        WT_SESSION *session2;

        testutil_wiredtiger_open(opts, test_home.c_str(), ENV_CONFIG, nullptr, &conn, false, false);
        testutil_check(conn->open_session(conn, nullptr, nullptr, &session));
        testutil_check(conn->open_session(conn, nullptr, nullptr, &session1));
        testutil_check(conn->open_session(conn, nullptr, nullptr, &session2));
        testutil_check(
          session->create(session, uri, "key_format=S,value_format=S,log=(enabled=true)"));

        /* Add some data. */
        wt_txn_begin(session1);
        wt_txn_insert(session1, uri, key1, value1);
        wt_txn_commit(session1, 10);
        wt_txn_begin(session1);
        wt_txn_insert(session1, uri, key2, value2);
        wt_txn_commit(session1, 20);

        /* Add a concurrent transaction. */
        wt_txn_begin(session1);
        wt_txn_insert(session1, uri, key3, value3);
        wt_txn_insert(session1, uri, key4, value4);

        /* Create an unnamed checkpoint. */
        wt_set_stable_timestamp(conn, 15);
        wt_ckpt_create(session, nullptr);
    }

    WT_CONNECTION *conn;
    WT_SESSION *session;
    WT_SESSION *session1;

    /* Reopen and verify. */
    testutil_wiredtiger_open(opts, test_home.c_str(), ENV_CONFIG, nullptr, &conn, false, false);
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session));
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session1));
    wt_model_assert(table, uri, key1);
    wt_model_assert(table, uri, key2);
    wt_model_assert(table, uri, key3);
    wt_model_assert(table, uri, key4);
    wt_model_assert(table, uri, key5);

    /* Clean up. */
    testutil_check(session->close(session, nullptr));
    testutil_check(session1->close(session1, nullptr));
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
        test_rts();
        test_rts_wt();
        test_rts_crash_wt();
        test_restart_wt1();
        test_restart_wt2();
        test_restart_wt3();
        test_crash_wt1();
        test_crash_wt2();
        test_crash_wt3();
        test_logged_wt();
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
