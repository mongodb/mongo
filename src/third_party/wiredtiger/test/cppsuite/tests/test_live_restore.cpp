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

/*
 * [live_restore]: live_restore_fs.c
 * This file tests WiredTiger's live restore behavior. If called with the -f flag it will populate a
 * test database and place it in a "backup" folder. Subsequent runs that don't have -f will open
 * WiredTiger in live restore mode using the backup folder as the source. It will then perform
 * random updates to the database, testing that we can perform operations on the database while live
 * restore copies data across from source in parallel.
 */
#include "src/common/constants.h"
#include "src/common/logger.h"
#include "src/common/random_generator.h"
#include "src/common/thread_manager.h"
#include "src/storage/connection_manager.h"
#include "src/storage/scoped_session.h"
#include "src/main/thread_worker.h"
#include "src/util/options_parser.h"

#include <vector>
#include <cstdlib>
#include <iostream>

extern "C" {
#include "wiredtiger.h"
#include "test_util.h"
}

using namespace test_harness;

static const char *file_config = "allocation_size=512B,internal_page_max=512B,leaf_page_max=512B";

class database_model {
public:
    /* Collection names start from zero. */
    void
    add_new_collection(scoped_session &session)
    {
        auto uri = database::build_collection_name(collection_count());
        testutil_check(session->create(
          session.get(), uri.c_str(), (DEFAULT_FRAMEWORK_SCHEMA + file_config).c_str()));
        _collections.emplace_back(uri);
    }

    void
    add_and_open_existing_collection(scoped_session &session, const std::string &uri)
    {
        _collections.emplace_back(uri);
        // TODO: Is it possible to validate that the data we get from a collection is the same as
        // the data we saved to it? To check for bugs in filename logic in the file system?
        testutil_check(
          session->create(session.get(), uri.c_str(), DEFAULT_FRAMEWORK_SCHEMA.c_str()));
        scoped_cursor cursor = session.open_scoped_cursor(uri.c_str());
        WT_IGNORE_RET(cursor->next(cursor.get()));
    }

    std::string &
    get_random_collection()
    {
        return _collections.at(
          random_generator::instance().generate_integer(0ul, collection_count() - 1));
    }

    std::vector<std::string>::const_iterator
    begin() const
    {
        return _collections.cbegin();
    }

    std::vector<std::string>::const_iterator
    end() const
    {
        return _collections.cend();
    }

    size_t
    collection_count()
    {
        return _collections.size();
    }

private:
    std::vector<std::string> _collections;
};

static const int iteration_count_default = 2;
static const int thread_count_default = 4;
static const int op_count_default = 20000;
static const int warmup_insertion_factor = 3;

static database_model db;
static const int key_size = 100;
static const int value_size = 50000;
static const char *SOURCE_PATH = "WT_LIVE_RESTORE_SOURCE";
static const char *HOME_PATH = DEFAULT_DIR;

/* Declarations to avoid the error raised by -Werror=missing-prototypes. */
void do_random_crud(scoped_session &session, bool fresh_start);
void create_collection(scoped_session &session);
void read(scoped_session &session);
void trigger_fs_truncate(scoped_session &session);
void write(scoped_session &session, bool fresh_start);
std::string key_to_str(uint64_t);
std::string generate_key();
std::string generate_value();
void insert(scoped_cursor &cursor, std::string &coll);
void update(scoped_cursor &cursor, std::string &coll);
void remove(scoped_session &session, scoped_cursor &cursor, std::string &coll);
void configure_database(scoped_session &session);

void
read(scoped_session &session)
{
    auto cursor = session.open_scoped_cursor(db.get_random_collection(), "next_random=true");
    auto ret = cursor->next(cursor.get());
    if (ret == WT_NOTFOUND)
        logger::log_msg(LOG_WARN, "Reading in a collection with no keys");
    else if (ret != 0)
        testutil_assert(ret == 0);
}

// Truncate from a random key to the end of the file and then call compact. This should
// result in the backing file on disk being fs_truncated.
void
trigger_fs_truncate(scoped_session &session)
{
    // Truncate from a random key all the way to the end of the collection and then call compact

    int ret;
    const std::string coll_name = db.get_random_collection();
    scoped_cursor rnd_cursor = session.open_scoped_cursor(coll_name, "next_random=true");
    ret = rnd_cursor->next(rnd_cursor.get());

    testutil_check_error_ok(ret, WT_NOTFOUND);
    if (ret == WT_NOTFOUND)
        // We've tried to truncate an empty collection. Nothing to do.
        return;

    testutil_check(session->truncate(session.get(), NULL, rnd_cursor.get(), nullptr, nullptr));
    testutil_check(session->compact(session.get(), coll_name.c_str(), nullptr));
}

std::string
generate_key()
{
    return random_generator::instance().generate_random_string(key_size);
}

std::string
generate_value()
{
    return random_generator::instance().generate_pseudo_random_string(value_size);
}

void
insert(scoped_cursor &cursor, std::string &coll)
{
    std::string key = generate_key();
    std::string value = generate_value();
    cursor->set_key(cursor.get(), key.c_str());
    cursor->set_value(cursor.get(), value.c_str());
    testutil_check(cursor->insert(cursor.get()));
}

void
update(scoped_cursor &cursor, std::string &coll)
{
    std::string key = generate_key();
    std::string value = generate_value();
    cursor->set_key(cursor.get(), key.c_str());
    cursor->set_value(cursor.get(), value.c_str());
    testutil_check(cursor->update(cursor.get()));
}

void
remove(scoped_session &session, scoped_cursor &cursor, std::string &coll)
{
    auto ran_cursor = session.open_scoped_cursor(coll, "next_random=true");
    auto ret = cursor->next(ran_cursor.get());
    if (ret == WT_NOTFOUND) {
        // no keys in collection.
        logger::log_msg(LOG_WARN, "Removing in a collection with no keys");
        return;
    }
    testutil_assert(ret == 0);
    const char *tmp_key;
    testutil_check(ran_cursor->get_key(ran_cursor.get(), &tmp_key));
    cursor->set_key(cursor.get(), tmp_key);
    testutil_check(cursor->remove(cursor.get()));
}

void
write(scoped_session &session, bool fresh_start)
{
    /* Force insertions for a duration on a fresh start. */
    auto ran = fresh_start ? 1 : random_generator::instance().generate_integer(0, 2);
    auto &coll = db.get_random_collection();
    auto cursor = session.open_scoped_cursor(coll);
    if (ran == 0) {
        // Update.
        update(cursor, coll);
        return;
    }
    if (ran == 1) {
        // Insert.
        insert(cursor, coll);
        return;
    }
    if (ran == 2) {
        update(cursor, coll);
        // TODO:
        // remove(cursor, coll);
        return;
    }

    // Unreachable.
    testutil_assert(false);
}

void
create_collection(scoped_session &session)
{
    db.add_new_collection(session);
}

static void
do_random_crud(scoped_session &session, const int64_t collection_count, const int64_t op_count,
  const bool fresh_start)
{
    bool file_created = fresh_start == false;

    /* Insert random data. */
    std::string key, value;
    int64_t warmup_insertions = op_count / warmup_insertion_factor;
    for (int i = 0; i < op_count; i++) {
        auto ran = random_generator::instance().generate_integer(0, 10000);
        if (ran <= 1 || !file_created) {
            if (static_cast<size_t>(collection_count) == db.collection_count())
                continue;
            // Create a new file, if none exist force this path.
            create_collection(session);
            file_created = true;
            continue;
        }

        if (i == 0) {
            for (int j = 0; j < warmup_insertions; j++)
                write(session, true);
            i = warmup_insertions;
        }

        if (ran < 3) {
            // 0.01% Checkpoint.
            testutil_check(session->checkpoint(session.get(), NULL));
            logger::log_msg(LOG_INFO, "Taking checkpoint");
        } else if (ran < 9000) {
            // 90% Write.
            write(session, false);
        } else if (ran <= 9980) {
            // 10% Read.
            read(session);
        } else if (ran <= 10000) {
            // 0.2% fs_truncate
            trigger_fs_truncate(session);
        } else {
            logger::log_msg(LOG_ERROR,
              "do_random_crud RNG (" + std::to_string(ran) + ") didn't find an operation to run");
            testutil_assert(false);
        }
    }
}

static void
get_stat(WT_CURSOR *cursor, int stat_field, int64_t *valuep)
{
    const char *desc, *pvalue;

    cursor->set_key(cursor, stat_field);
    error_check(cursor->search(cursor));
    error_check(cursor->get_value(cursor, &desc, &pvalue, valuep));
}

static void
run_restore(const std::string &home, const std::string &source, const int64_t thread_count,
  const int64_t collection_count, const int64_t op_count, const bool background_thread_mode,
  const int64_t verbose_level, const bool first)
{
    /* Create a connection, set the cache size and specify the home directory. */
    const std::string verbose_string =
      verbose_level == 0 ? "" : "verbose=[fileops:" + std::to_string(verbose_level) + "]";
    /*
     * FIXME-WT-13888 - The "fill_holes_on_close" configuration can be removed once proper work
     * queuing is implemented. The current implementation skips the turtle and metadata file which
     * breaks things.
     */
    const std::string conn_config = CONNECTION_CREATE +
      ",live_restore=(enabled=true,debug=(fill_holes_on_close=true),threads_max=" +
      std::to_string(thread_count) + ",path=\"" + source + "\"),cache_size=5GB," + verbose_string +
      ",statistics=(all),statistics_log=(json,on_close,wait=1)";

    /* Create connection. */
    if (first)
        connection_manager::instance().create(conn_config, home);
    else
        connection_manager::instance().reopen(conn_config, home);

    auto crud_session = connection_manager::instance().create_session();
    if (!background_thread_mode)
        do_random_crud(crud_session, collection_count, op_count, first);

    // Loop until the state stat is complete!
    if (thread_count > 0 || (background_thread_mode && !first)) {
        logger::log_msg(LOG_INFO, "Waiting for background data transfer to complete...");
        while (true) {
            auto stat_cursor = crud_session.open_scoped_cursor("statistics:");
            int64_t state;
            get_stat(stat_cursor.get(), WT_STAT_CONN_LIVE_RESTORE_STATE, &state);
            if (state == WT_LIVE_RESTORE_COMPLETE)
                break;
            __wt_sleep(1, 0);
        }
        logger::log_msg(LOG_INFO, "Done!");
    }

    // We need to close the session here because the connection close will close it out for us if we
    // don't. Then we'll crash because we'll double close a WT session.
    crud_session.close_session();
    connection_manager::instance().close();
}

static void
usage()
{
    std::cout << "Usage: ./test_live_restore [OPTIONS]" << std::endl;
    std::cout << "DESCRIPTION" << std::endl;
    std::cout << "\t The live restore test simulates performing a live restore on a database. If "
                 "no options are specified it will run with a default configuration."
              << std::endl;
    std::cout << "OPTIONS" << std::endl;
    std::cout << "\t-b Background thread debug mode, initialize the database then loop for "
                 "iteration count. Each iteration will wait for the background thread to finish "
                 "transferring data before terminating. No additional CRUD operations will take "
                 "place during background thread debug mode. "
              << std::endl;
    std::cout << "\t-c The maximum number of collections to run the test with, if unset "
                 "collections are created at random."
              << std::endl;
    std::cout << "\t-H Specifies the database home directory." << std::endl;
    std::cout << "\t-h Output a usage message and exit." << std::endl;
    std::cout << "\t-i The number of iterations to run the test program. Note: A value of 1 with "
                 "no source directory specified will simply populate a database i.e. no live "
                 "restore will take place. The default iteration count is 2."
              << std::endl;
    std::cout << "\t-l Log level, this controls the level of logging that this test will run with. "
                 "This is distinct from the verbose level option as that is a WiredTiger "
                 "configuration. Default is LOG_ERROR (0). The other levels are LOG_WARN (1), "
                 "LOG_INFO (2) and LOG_TRACE (3)."
              << std::endl;
    std::cout << "\t-o The number of crud operations to apply while live restoring." << std::endl;
    std::cout << "\t-t Thread count for the background thread. A value of 0 is legal in which case "
                 "data files will not be transferred in the background."
              << std::endl;
    std::cout << "\t-o Verbose level, this setting will set WT_VERB_FILE_OPS with whatever level "
                 "is provided. The default is off."
              << std::endl;
}

#include <iostream>
int
main(int argc, char *argv[])
{
    /* Set the program name for error messages. */
    const std::string progname = testutil_set_progname(argv);

    // Parse the options. Starting with the help message.
    if (option_exists("-h", argc, argv)) {
        usage();
        return EXIT_FAILURE;
    };

    auto log_level_str = value_for_opt("-l", argc, argv);
    // Set the tracing level for the logger component.
    logger::trace_level = log_level_str.empty() ? LOG_ERROR : atoi(log_level_str.c_str());

    // Get the iteration count if it exists.
    auto it_count_str = value_for_opt("-i", argc, argv);
    int64_t it_count = it_count_str.empty() ? iteration_count_default : atoi(it_count_str.c_str());
    logger::log_msg(LOG_INFO, "Iteration count: " + std::to_string(it_count));

    // Get the thread count if it exists.
    auto thread_count_str = value_for_opt("-t", argc, argv);
    int64_t thread_count =
      thread_count_str.empty() ? thread_count_default : atoi(thread_count_str.c_str());
    logger::log_msg(LOG_INFO, "Background thread count: " + std::to_string(thread_count));

    // Get the collection count if it exists.
    auto coll_count_str = value_for_opt("-c", argc, argv);
    int64_t coll_count = INT64_MAX;
    if (!coll_count_str.empty()) {
        coll_count = atoi(coll_count_str.c_str());
        logger::log_msg(LOG_INFO, "Collection count: " + std::to_string(coll_count));
    }

    // Get the op_count if it exists.
    auto op_count_str = value_for_opt("-o", argc, argv);
    int64_t op_count = op_count_str.empty() ? op_count_default : atoi(op_count_str.c_str());
    logger::log_msg(LOG_INFO, "Op count: " + std::to_string(op_count));

    // Get the verbose_level if it exists.
    auto verbose_str = value_for_opt("-v", argc, argv);
    int64_t verbose_level = verbose_str.empty() ? 0 : atoi(verbose_str.c_str());
    logger::log_msg(LOG_INFO, "Verbose level: " + std::to_string(verbose_level));

    // Background thread debug mode option.
    bool background_thread_debug_mode = option_exists("-b", argc, argv);
    if (background_thread_debug_mode && thread_count == 0) {
        logger::log_msg(
          LOG_ERROR, "Cannot run in background thread debug mode with zero background threads.");
        return EXIT_FAILURE;
    }

    // Home path option.
    std::string home_path = value_for_opt("-H", argc, argv);
    if (home_path.empty())
        home_path = HOME_PATH;
    logger::log_msg(LOG_INFO, "Home path: " + home_path);

    // Delete any existing source dir.
    testutil_recreate_dir(SOURCE_PATH);
    logger::log_msg(LOG_INFO, "Source path: " + std::string(SOURCE_PATH));

    // Recreate the home directory on startup every time.
    testutil_recreate_dir(home_path.c_str());

    bool first = true;
    /* When setting up the database we don't want to wait for the background threads to complete. */
    bool background_thread_mode_enabled = (!first && background_thread_debug_mode);
    for (int i = 0; i < it_count; i++) {
        logger::log_msg(LOG_INFO, "!!!! Beginning iteration: " + std::to_string(i) + " !!!!");
        run_restore(home_path, SOURCE_PATH, thread_count, coll_count, op_count,
          background_thread_mode_enabled, verbose_level, first);
        testutil_remove(SOURCE_PATH);
        testutil_move(HOME_PATH, SOURCE_PATH);
        testutil_recreate_dir(HOME_PATH);
        first = false;
        background_thread_mode_enabled = background_thread_debug_mode;
    }

    return (0);
}
