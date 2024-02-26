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
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "wiredtiger.h"
extern "C" {
#include "wt_internal.h"
}

#include "model/driver/kv_workload_generator.h"
#include "model/test/util.h"
#include "model/test/wiredtiger_util.h"
#include "model/kv_database.h"
#include "model/util.h"

/*
 * Command-line arguments.
 */
extern int __wt_optind, __wt_optwt;
extern char *__wt_optarg;

/*
 * Connection configuration.
 */
#define ENV_CONFIG_BASE "create=true,log=(enabled=false)"

/*
 * Table configuration: Use small pages to force WiredTiger to generate deeper trees with less
 * effort than we would have generated otherwise.
 */
#define TABLE_CONFIG_BASE "leaf_page_max=4KB"

/*
 * run_and_verify --
 *     Run and verify the workload.
 */
static void
run_and_verify(std::shared_ptr<model::kv_workload> workload, const std::string &home,
  const std::string &conn_config, const std::string &table_config)
{
    /* Run the workload in the model. */
    model::kv_database database;
    try {
        workload->run(database);

        /* When we load the workload from WiredTiger, that would be after running recovery. */
        database.restart();
    } catch (std::exception &e) {
        throw std::runtime_error(
          "Failed to run the workload in the model: " + std::string(e.what()));
    }

    /* Create the database directory and save the workload. */
    testutil_recreate_dir(home.c_str());
    std::string workload_file = home + DIR_DELIM_STR + "WORKLOAD";
    std::ofstream workload_out;
    workload_out.open(workload_file);
    if (!workload_out.is_open())
        throw std::runtime_error("Failed to create file: " + workload_file);
    workload_out << *workload.get();
    workload_out.close();
    if (!workload_out.good())
        throw std::runtime_error("Failed to close file: " + workload_file);

    /* Run the workload in WiredTiger. */
    try {
        workload->run_in_wiredtiger(home.c_str(), conn_config.c_str(), table_config.c_str());
    } catch (std::exception &e) {
        throw std::runtime_error(
          "Failed to run the workload in WiredTiger: " + std::string(e.what()));
    }

    /* Open the WiredTiger database to verify. */
    WT_CONNECTION *conn;
    int ret =
      wiredtiger_open(home.c_str(), nullptr /* event handler */, conn_config.c_str(), &conn);
    if (ret != 0)
        throw std::runtime_error("Cannot open the database: " +
          std::string(wiredtiger_strerror(ret)) + " (" + std::to_string(ret) + ")");
    model::wiredtiger_connection_guard conn_guard(conn); /* Automatically close at the end. */

    /* Get the list of tables. */
    std::vector<std::string> tables;
    try {
        tables = model::wt_list_tables(conn);
    } catch (std::exception &e) {
        throw std::runtime_error("Failed to list the tables: " + std::string(e.what()));
    }

    /* Verify the database. */
    for (auto &t : tables)
        try {
            database.table(t)->verify(conn);
        } catch (std::exception &e) {
            throw std::runtime_error("Verification failed for table " + t + ": " + e.what());
        }
}

/*
 * update_spec --
 *     Update the workload generator's specification from the given config string. Throw an
 *     exception on error.
 */
static void
update_spec(model::kv_workload_generator_spec &spec, std::string &conn_config,
  std::string &table_config, const char *config)
{
    model::config_map m = model::config_map::from_string(config);
    std::vector<std::string> keys = m.keys();

#define UPDATE_SPEC_START \
    if (k == "")          \
    continue
#define UPDATE_SPEC(what, type) else if (k == #what) spec.what = m.get_##type(#what)

    for (std::string &k : keys) {
        UPDATE_SPEC_START;

        UPDATE_SPEC(min_tables, uint64);
        UPDATE_SPEC(max_tables, uint64);
        UPDATE_SPEC(min_sequences, uint64);
        UPDATE_SPEC(max_sequences, uint64);
        UPDATE_SPEC(max_concurrent_transactions, uint64);
        UPDATE_SPEC(max_value_uint64, uint64);

        UPDATE_SPEC(use_set_commit_timestamp, float);

        UPDATE_SPEC(finish_transaction, float);
        UPDATE_SPEC(insert, float);
        UPDATE_SPEC(remove, float);
        UPDATE_SPEC(set_commit_timestamp, float);
        UPDATE_SPEC(truncate, float);

        UPDATE_SPEC(checkpoint, float);
        UPDATE_SPEC(crash, float);
        UPDATE_SPEC(restart, float);
        UPDATE_SPEC(set_stable_timestamp, float);

        UPDATE_SPEC(prepared_transaction, float);
        UPDATE_SPEC(nonprepared_transaction_rollback, float);
        UPDATE_SPEC(prepared_transaction_rollback_after_prepare, float);
        UPDATE_SPEC(prepared_transaction_rollback_before_prepare, float);

        else if (k == "connection_config") conn_config += "," + m.get_string("connection_config");

        else if (k == "table_config") table_config += "," + m.get_string("table_config");

        else throw std::runtime_error("Invalid configuration key: " + k);
    }

#undef UPDATE_SPEC_START
#undef UPDATE_SPEC
}

/*
 * load_spec --
 *     Load the specification from file. Throw an exception on error.
 */
static void
load_spec(model::kv_workload_generator_spec &spec, std::string &conn_config,
  std::string &table_config, const char *file)
{
    std::ifstream f(file);
    if (!f.is_open())
        throw std::runtime_error("Cannot open file: " + std::string(file));

    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        update_spec(spec, conn_config, table_config, line.c_str());
    }
    if (!f.eof() && !f.good())
        throw std::runtime_error("Cannot read from file: " + std::string(file));

    f.close();
}

/*
 * usage --
 *     Print usage help for the program. (Don't exit.)
 */
static void
usage(const char *progname)
{
    fprintf(stderr, "usage: %s [OPTIONS]\n\n", progname);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -C CONFIG  specify WiredTiger's connection configuration\n");
    fprintf(stderr, "  -G CONFIG  specify the workload generator's configuration\n");
    fprintf(stderr, "  -h HOME    specify the database directory\n");
    fprintf(stderr, "  -I n       run the test for at least this many iterations\n");
    fprintf(stderr, "  -i FILE    load the generator's configuration from the file\n");
    fprintf(stderr, "  -l N[-M]   specify the workload length as a number of transactions\n");
    fprintf(stderr, "  -M N[-M]   specify the number of tables\n");
    fprintf(stderr, "  -n         do not execute the workload; only print it\n");
    fprintf(stderr, "  -p         preserve the last database directory\n");
    fprintf(stderr, "  -S SEED    specify the random number generator's seed\n");
    fprintf(stderr, "  -T CONFIG  specify WiredTiger's table configuration\n");
    fprintf(stderr, "  -t N       repeat the test for at least this number of seconds\n");
    fprintf(stderr, "  -?         show this message\n");
}

/*
 * main --
 *     The main entry point for the test.
 */
int
main(int argc, char *argv[])
{
    model::kv_workload_generator_spec spec;

    uint64_t base_seed = (uint64_t)time(NULL);
    std::string home = "WT_TEST";
    uint64_t min_iterations = 1;
    uint64_t min_runtime_s = 0;
    bool preserve = false;
    bool print_only = false;
    const char *progname = argv[0];

    std::string conn_config = ENV_CONFIG_BASE;
    std::string table_config = TABLE_CONFIG_BASE;

    /*
     * Parse the command-line arguments.
     */
    try {
        std::pair<uint64_t, uint64_t> p;
        int ch;

        __wt_optwt = 1;
        while ((ch = __wt_getopt(progname, argc, argv, "C:G:h:I:i:l:M:npS:T:t:?")) != EOF)
            switch (ch) {
            case 'C':
                conn_config += ",";
                conn_config += __wt_optarg;
                break;
            case 'G':
                update_spec(spec, conn_config, table_config, __wt_optarg);
                break;
            case 'h':
                home = __wt_optarg;
                break;
            case 'I':
                min_iterations = parse_uint64(__wt_optarg);
                break;
            case 'i':
                load_spec(spec, conn_config, table_config, __wt_optarg);
                break;
            case 'l':
                p = parse_uint64_range(__wt_optarg);
                spec.min_sequences = p.first;
                spec.max_sequences = p.second;
                if (p.first <= 0)
                    throw std::runtime_error("Not enough transactions");
                break;
            case 'M':
                p = parse_uint64_range(__wt_optarg);
                spec.min_tables = p.first;
                spec.max_tables = p.second;
                if (p.first <= 0)
                    throw std::runtime_error("Not enough tables");
                break;
            case 'n':
                print_only = true;
                break;
            case 'p':
                preserve = true;
                break;
            case 'S':
                base_seed = parse_uint64(__wt_optarg);
                break;
            case 'T':
                table_config += ",";
                table_config += __wt_optarg;
                break;
            case 't':
                min_runtime_s = parse_uint64(__wt_optarg);
                break;
            case '?':
                usage(progname);
                return EXIT_SUCCESS;
            default:
                usage(progname);
                return EXIT_FAILURE;
            }
        argc -= __wt_optind;
        if (argc != 0) {
            usage(progname);
            return EXIT_FAILURE;
        }
    } catch (std::exception &e) {
        std::cerr << progname << ": " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    /* Run the test, potentially many times. */
    double start_time = current_time();
    for (uint64_t iteration = 1;; iteration++) {
        uint64_t seed = base_seed + iteration - 1;
        std::cout << "Iteration " << iteration << ", seed 0x" << std::hex << seed << std::dec
                  << std::endl;

        /* Generate the workload. */
        std::shared_ptr<model::kv_workload> workload;
        try {
            workload = model::kv_workload_generator::generate(spec, seed);
        } catch (std::exception &e) {
            std::cerr << "Failed to generate the workload: " << e.what() << std::endl;
            return EXIT_FAILURE;
        }

        /* If we only want to print the workload, then do so. */
        if (print_only) {
            std::cout << *workload.get();
            break;
        }

        /* Clean up the previous database directory, if it exists. */
        testutil_remove(home.c_str());

        /* Run and verify the workload. */
        try {
            run_and_verify(workload, home, conn_config, table_config);
        } catch (std::exception &e) {
            std::cerr << e.what() << std::endl;
            return EXIT_FAILURE;
        }

        /* Check the test exit conditions. */
        double total_time = current_time() - start_time;
        if (total_time >= min_runtime_s && iteration >= min_iterations)
            break;
    }

    /* Clean up the database directory. */
    if (!preserve)
        testutil_remove(home.c_str());

    return EXIT_SUCCESS;
}
