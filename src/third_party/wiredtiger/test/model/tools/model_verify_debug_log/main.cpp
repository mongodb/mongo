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

/*
 * Command-line arguments.
 */
extern int __wt_optind, __wt_optwt;
extern char *__wt_optarg;

/*
 * Configuration.
 */
#define ENV_CONFIG "readonly=true,log=(enabled=false)"

/*
 * usage --
 *     Print usage help for the program. (Don't exit.)
 */
static void
usage(const char *progname)
{
    fprintf(stderr, "usage: %s [OPTIONS]\n\n", progname);
    fprintf(stderr, "Options:\n");
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
    const char *debug_log_json, *home, *progname;
    int ch, ret;

    debug_log_json = nullptr;
    home = nullptr;
    progname = argv[0];

    /*
     * Parse the command-line arguments.
     */
    __wt_optwt = 1;
    while ((ch = __wt_getopt(progname, argc, argv, "h:j:?")) != EOF)
        switch (ch) {
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
    ret = wiredtiger_open(home, nullptr /* event handler */, ENV_CONFIG, &conn);
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
     * Verify the database.
     */
    try {
        for (auto &t : tables) {
            std::cout << "Verifying table: " << t << std::endl;
            db.table(t)->verify(conn);
        }
    } catch (std::exception &e) {
        std::cerr << "Verification failed: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
