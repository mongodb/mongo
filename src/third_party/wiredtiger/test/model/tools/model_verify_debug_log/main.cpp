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
#include <string>
#include <vector>

#include "wiredtiger.h"
extern "C" {
#include "wt_internal.h"
}

#include "model/driver/debug_log_parser.h"
#include "model/kv_database.h"
#include "model/util.h"

/*
 * Command-line arguments.
 */
extern int __wt_optind, __wt_optwt;
extern char *__wt_optarg;

/*
 * Configuration.
 */
#define ENV_CONFIG_BASE "readonly=true,log=(enabled=false)"

/*
 * verify_timestamps --
 *     Verify the global database timestamps. Throw exception on error.
 */
static void
verify_timestamps(model::kv_database &db, WT_CONNECTION *conn)
{
    char buf[64];
    int ret;

    ret = conn->query_timestamp(conn, buf, "get=oldest_timestamp");
    if (ret != 0)
        throw model::wiredtiger_exception(ret);
    model::timestamp_t oldest_timestamp = model::parse_uint64(std::string("0x") + buf);
    if (oldest_timestamp != db.oldest_timestamp())
        throw std::runtime_error("The oldest timestamp does not match: WiredTiger has " +
          std::to_string(oldest_timestamp) + ", but " + std::to_string(db.oldest_timestamp()) +
          " was expected.");

    ret = conn->query_timestamp(conn, buf, "get=stable_timestamp");
    if (ret != 0)
        throw model::wiredtiger_exception(ret);
    model::timestamp_t stable_timestamp = model::parse_uint64(std::string("0x") + buf);
    if (stable_timestamp != db.stable_timestamp())
        throw std::runtime_error("The stable timestamp does not match: WiredTiger has " +
          std::to_string(stable_timestamp) + ", but " + std::to_string(db.stable_timestamp()) +
          " was expected.");
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
    fprintf(stderr, "  -c NAME    specify the checkpoint to verify\n");
    fprintf(stderr, "  -h HOME    specify the database directory\n");
    fprintf(stderr, "  -j PATH    load the debug log from a JSON file\n");
    fprintf(stderr, "  -?         show this message\n");
}

/*
 * main --
 *     The main entry point for the test.
 */
int
main(int argc, char *argv[])
{
    const char *checkpoint, *debug_log_json, *home, *progname;
    int ch, ret;

    checkpoint = nullptr;
    debug_log_json = nullptr;
    home = nullptr;
    progname = argv[0];

    std::string conn_config = ENV_CONFIG_BASE;

    /*
     * Parse the command-line arguments.
     */
    __wt_optwt = 1;
    while ((ch = __wt_getopt(progname, argc, argv, "C:c:h:j:?")) != EOF)
        switch (ch) {
        case 'C':
            conn_config += ",";
            conn_config += __wt_optarg;
            break;
        case 'c':
            checkpoint = __wt_optarg;
            break;
        case 'h':
            home = __wt_optarg;
            break;
        case 'j':
            debug_log_json = __wt_optarg;
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

    /*
     * Open the WiredTiger database to verify.
     */
    WT_CONNECTION *conn;
    ret = wiredtiger_open(home, nullptr /* event handler */, conn_config.c_str(), &conn);
    if (ret != 0) {
        std::cerr << "Cannot open the database: " << wiredtiger_strerror(ret) << std::endl;
        return EXIT_FAILURE;
    }
    model::wiredtiger_connection_guard conn_guard(conn); /* Automatically close on exit. */

    /*
     * Get the list of tables.
     */
    std::vector<std::string> tables;
    try {
        tables = model::wt_list_tables(conn);
    } catch (std::exception &e) {
        std::cerr << "Failed to list the tables: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    /*
     * Load the debug log into the model.
     */
    model::kv_database db;
    try {
        if (debug_log_json == nullptr)
            model::debug_log_parser::from_debug_log(db, conn);
        else {
            std::cout << "Loading: " << debug_log_json << std::endl;
            model::debug_log_parser::from_json(db, debug_log_json);
        }
    } catch (std::exception &e) {
        std::cerr << "Failed to load the debug log: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    /*
     * Get the checkpoint, if applicable.
     */
    model::kv_checkpoint_ptr ckpt;
    try {
        if (checkpoint != nullptr)
            ckpt = db.checkpoint(checkpoint);
    } catch (std::exception &e) {
        std::cerr << "Failed to get the checkpoint: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    /*
     * Verify the global timestamps.
     */
    try {
        verify_timestamps(db, conn);
    } catch (std::exception &e) {
        std::cerr << "Verification failed: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    /*
     * Verify the database.
     */
    try {
        for (auto &t : tables) {
            std::cout << "Verifying table: " << t << std::endl;
            db.table(t)->verify(conn, ckpt);
        }
    } catch (std::exception &e) {
        std::cerr << "Verification failed: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
