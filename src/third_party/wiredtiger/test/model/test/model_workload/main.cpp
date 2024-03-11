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

#include "model/driver/kv_workload.h"
#include "model/driver/kv_workload_generator.h"
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

/* Values. */
const model::data_value value1("Value 1");
const model::data_value value2("Value 2");
const model::data_value value3("Value 3");
const model::data_value value4("Value 4");
const model::data_value value5("Value 5");
const model::data_value value6("Value 6");

/* Table IDs. */
constexpr model::table_id_t k_table1_id = 1;

/*
 * test_workload_basic --
 *     The basic test of the workload executor.
 */
static void
test_workload_basic(void)
{
    model::kv_workload workload;
    workload << model::operation::create_table(k_table1_id, "table1", "S", "S")
             << model::operation::begin_transaction(1) << model::operation::begin_transaction(2)
             << model::operation::insert(k_table1_id, 1, key1, value1)
             << model::operation::insert(k_table1_id, 2, key2, value2)
             << model::operation::commit_transaction(1) << model::operation::commit_transaction(2)
             << model::operation::begin_transaction(1)
             << model::operation::insert(k_table1_id, 1, key3, value3)
             << model::operation::insert(k_table1_id, 1, key4, value4)
             << model::operation::remove(k_table1_id, 1, key1)
             << model::operation::commit_transaction(1) << model::operation::begin_transaction(1)
             << model::operation::truncate(k_table1_id, 1, key4, key4)
             << model::operation::commit_transaction(1);

    /* Run the workload in the model. */
    model::kv_database database;
    workload.run(database);

    /* Verify the contents of the model. */
    model::kv_table_ptr table = database.table("table1");
    testutil_assert(table->get(key1) == model::NONE);
    testutil_assert(table->get(key2) == value2);
    testutil_assert(table->get(key3) == value3);
    testutil_assert(table->get(key4) == model::NONE);

    /* Run the workload in WiredTiger and verify. */
    std::string test_home = std::string(home) + DIR_DELIM_STR + "basic";
    verify_workload(workload, opts, test_home, ENV_CONFIG);
    verify_using_debug_log(opts, test_home.c_str()); /* May as well test this. */
}

/*
 * test_workload_txn --
 *     Test the workload executor with timestamped transactions.
 */
static void
test_workload_txn(void)
{
    model::kv_workload workload;
    workload << model::operation::create_table(k_table1_id, "table1", "S", "S")
             << model::operation::begin_transaction(1) << model::operation::begin_transaction(2)
             << model::operation::insert(k_table1_id, 1, key1, value1)
             << model::operation::insert(k_table1_id, 2, key2, value2)
             << model::operation::commit_transaction(1, 10)
             << model::operation::commit_transaction(2, 20)
             << model::operation::begin_transaction(1)
             << model::operation::insert(k_table1_id, 1, key3, value3)
             << model::operation::insert(k_table1_id, 1, key4, value4)
             << model::operation::remove(k_table1_id, 1, key1)
             << model::operation::commit_transaction(1, 30)
             << model::operation::begin_transaction(1)
             << model::operation::remove(k_table1_id, 1, key3)
             << model::operation::insert(k_table1_id, 1, key4, value6)
             << model::operation::rollback_transaction(1) << model::operation::begin_transaction(1)
             << model::operation::set_commit_timestamp(1, 35)
             << model::operation::insert(k_table1_id, 1, key1, value5)
             << model::operation::set_commit_timestamp(1, 40)
             << model::operation::truncate(k_table1_id, 1, key4, key4)
             << model::operation::commit_transaction(1)
             << model::operation::set_stable_timestamp(35)
             << model::operation::rollback_to_stable();

    /* Run the workload in the model. */
    model::kv_database database;
    workload.run(database);

    /* Verify the contents of the model. */
    model::kv_table_ptr table = database.table("table1");
    testutil_assert(table->get(key1) == value5);
    testutil_assert(table->get(key2) == value2);
    testutil_assert(table->get(key3) == value3);
    testutil_assert(table->get(key4) == value4);

    /* Run the workload in WiredTiger and verify. */
    std::string test_home = std::string(home) + DIR_DELIM_STR + "txn";
    verify_workload(workload, opts, test_home, ENV_CONFIG);
    verify_using_debug_log(opts, test_home.c_str()); /* May as well test this. */
}

/*
 * test_workload_prepared --
 *     Test the workload executor with prepared transactions.
 */
static void
test_workload_prepared(void)
{
    model::kv_workload workload;
    workload << model::operation::create_table(k_table1_id, "table1", "S", "S")
             << model::operation::begin_transaction(1) << model::operation::begin_transaction(2)
             << model::operation::insert(k_table1_id, 1, key1, value1)
             << model::operation::insert(k_table1_id, 2, key2, value2)
             << model::operation::prepare_transaction(1, 10)
             << model::operation::prepare_transaction(2, 15)
             << model::operation::commit_transaction(1, 20, 21)
             << model::operation::commit_transaction(2, 25, 26)
             << model::operation::set_stable_timestamp(24)
             << model::operation::rollback_to_stable();

    /* Run the workload in the model. */
    model::kv_database database;
    workload.run(database);

    /* Verify the contents of the model. */
    model::kv_table_ptr table = database.table("table1");
    testutil_assert(table->get(key1) == value1);
    testutil_assert(table->get(key2) == model::NONE);
    testutil_assert(table->get(key3) == model::NONE);

    /* Run the workload in WiredTiger and verify. */
    std::string test_home = std::string(home) + DIR_DELIM_STR + "prepared";
    verify_workload(workload, opts, test_home, ENV_CONFIG);
    verify_using_debug_log(opts, test_home.c_str()); /* May as well test this. */
}

/*
 * test_workload_restart --
 *     Test the workload executor with database restart.
 */
static void
test_workload_restart(void)
{
    model::kv_workload workload;
    workload << model::operation::create_table(k_table1_id, "table1", "S", "S")
             << model::operation::begin_transaction(1) << model::operation::begin_transaction(2)
             << model::operation::insert(k_table1_id, 1, key1, value1)
             << model::operation::insert(k_table1_id, 2, key2, value2)
             << model::operation::prepare_transaction(1, 10)
             << model::operation::prepare_transaction(2, 15)
             << model::operation::commit_transaction(1, 20, 21)
             << model::operation::commit_transaction(2, 25, 26)
             << model::operation::set_stable_timestamp(22) << model::operation::begin_transaction(1)
             << model::operation::remove(k_table1_id, 1, key1) << model::operation::checkpoint()
             << model::operation::restart() << model::operation::begin_transaction(1)
             << model::operation::insert(k_table1_id, 1, key3, value3)
             << model::operation::prepare_transaction(1, 23)
             << model::operation::commit_transaction(1, 24, 25)
             << model::operation::set_stable_timestamp(25);

    /* Run the workload in the model. */
    model::kv_database database;
    workload.run(database);

    /* Verify the contents of the model. */
    model::kv_table_ptr table = database.table("table1");
    testutil_assert(table->get(key1) == value1);
    testutil_assert(table->get(key2) == model::NONE);
    testutil_assert(table->get(key3) == value3);

    /* Run the workload in WiredTiger and verify. */
    std::string test_home = std::string(home) + DIR_DELIM_STR + "restart";
    verify_workload(workload, opts, test_home, ENV_CONFIG);
    verify_using_debug_log(opts, test_home.c_str()); /* May as well test this. */
}

/*
 * test_workload_crash --
 *     Test the workload executor with database crash.
 */
static void
test_workload_crash(void)
{
    model::kv_workload workload;
    workload << model::operation::create_table(k_table1_id, "table1", "S", "S")
             << model::operation::begin_transaction(1) << model::operation::begin_transaction(2)
             << model::operation::insert(k_table1_id, 1, key1, value1)
             << model::operation::insert(k_table1_id, 2, key2, value2)
             << model::operation::prepare_transaction(1, 10)
             << model::operation::prepare_transaction(2, 15)
             << model::operation::commit_transaction(1, 20, 21)
             << model::operation::commit_transaction(2, 25, 26)
             << model::operation::set_stable_timestamp(22) << model::operation::begin_transaction(1)
             << model::operation::remove(k_table1_id, 1, key1) << model::operation::checkpoint()
             << model::operation::crash() << model::operation::begin_transaction(1)
             << model::operation::insert(k_table1_id, 1, key3, value3)
             << model::operation::prepare_transaction(1, 23)
             << model::operation::commit_transaction(1, 24, 25)
             << model::operation::set_stable_timestamp(25);

    /* Run the workload in the model. */
    model::kv_database database;
    workload.run(database);

    /* Verify the contents of the model. */
    model::kv_table_ptr table = database.table("table1");
    testutil_assert(table->get(key1) == value1);
    testutil_assert(table->get(key2) == model::NONE);
    testutil_assert(table->get(key3) == value3);

    /* Run the workload in WiredTiger and verify. */
    std::string test_home = std::string(home) + DIR_DELIM_STR + "crash";
    verify_workload(workload, opts, test_home, ENV_CONFIG);
    verify_using_debug_log(opts, test_home.c_str()); /* May as well test this. */
}

/*
 * test_workload_generator --
 *     Test the workload generator.
 */
static void
test_workload_generator(void)
{
    std::shared_ptr<model::kv_workload> workload = model::kv_workload_generator::generate();

    /* Run the workload in the model and in WiredTiger, then verify. */
    std::string test_home = std::string(home) + DIR_DELIM_STR + "generator";
    verify_workload(*workload, opts, test_home, ENV_CONFIG);
}

/*
 * test_workload_parse --
 *     Test the workload parser.
 */
static void
test_workload_parse(void)
{
    model::kv_workload workload;

    /* The workload parser currently supports only unsigned numbers for keys and values. */
    workload << model::operation::create_table(k_table1_id, "table1", "Q", "Q")
             << model::operation::begin_transaction(1) << model::operation::begin_transaction(2)
             << model::operation::insert(
                  k_table1_id, 1, model::data_value((uint64_t)1), model::data_value((uint64_t)1))
             << model::operation::insert(
                  k_table1_id, 2, model::data_value((uint64_t)2), model::data_value((uint64_t)2))
             << model::operation::prepare_transaction(1, 10)
             << model::operation::prepare_transaction(2, 15)
             << model::operation::commit_transaction(1, 20, 21)
             << model::operation::rollback_transaction(2)
             << model::operation::set_stable_timestamp(22) << model::operation::begin_transaction(1)
             << model::operation::remove(k_table1_id, 1, model::data_value((uint64_t)1))
             << model::operation::checkpoint() << model::operation::crash()
             << model::operation::begin_transaction(1)
             << model::operation::insert(
                  k_table1_id, 1, model::data_value((uint64_t)3), model::data_value((uint64_t)3))
             << model::operation::truncate(
                  k_table1_id, 2, model::data_value((uint64_t)1), model::data_value((uint64_t)2))
             << model::operation::prepare_transaction(1, 23)
             << model::operation::commit_transaction(1, 24, 25)
             << model::operation::set_stable_timestamp(25) << model::operation::rollback_to_stable()
             << model::operation::restart();

    /* Convert to string, parse, and compare each operation. */
    for (size_t i = 0; i < workload.size(); i++) {
        std::stringstream ss;
        ss << workload[i].operation;
        model::operation::any op = model::operation::parse(ss.str());
        testutil_assert(workload[i].operation == op);
    }

    /* Additional tests for different allowed parsing behaviors. */
    testutil_assert(model::operation::parse("create_table(1, table1, Q, Q)") ==
      model::operation::any(model::operation::create_table(1, "table1", "Q", "Q")));
    testutil_assert(model::operation::parse("create_table(1, \"table1\", \"Q\", \"Q\")") ==
      model::operation::any(model::operation::create_table(1, "table1", "Q", "Q")));
    testutil_assert(model::operation::parse("create_table   (   0x1,table1, \"Q\",  Q      )  ") ==
      model::operation::any(model::operation::create_table(1, "table1", "Q", "Q")));
    testutil_assert(model::operation::parse("create_table(1, \"table\\\" \\\\\", \"Q\", \"\")") ==
      model::operation::any(model::operation::create_table(1, "table\" \\", "Q", "")));
    testutil_assert(model::operation::parse("create_table\t\n(0x1 ,\"table\" \"1\", \"Q\", S )") ==
      model::operation::any(model::operation::create_table(1, "table1", "Q", "S")));

    /* Test optional arguments. */
    testutil_assert(model::operation::parse("checkpoint()") ==
      model::operation::any(model::operation::checkpoint()));
    testutil_assert(model::operation::parse("checkpoint(\"test\")") ==
      model::operation::any(model::operation::checkpoint("test")));
    testutil_assert(model::operation::parse("commit_transaction(1)") ==
      model::operation::any(model::operation::commit_transaction(1)));
    testutil_assert(model::operation::parse("commit_transaction(1, 2)") ==
      model::operation::any(model::operation::commit_transaction(1, 2)));
    testutil_assert(model::operation::parse("commit_transaction(1, 2, 3)") ==
      model::operation::any(model::operation::commit_transaction(1, 2, 3)));
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
        test_workload_basic();
        test_workload_txn();
        test_workload_prepared();
        test_workload_restart();
        test_workload_crash();
        test_workload_generator();
        test_workload_parse();
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
