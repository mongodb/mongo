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
 * This file tests WiredTiger's live restore behavior. It will populate a test database and place it
 * in a "backup" folder. Subsequent runs will open WiredTiger in live restore mode using the backup
 * folder as the source. It will then perform random updates to the database, testing that we can
 * perform operations on the database while live restore copies data across from source in parallel.
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
#include <signal.h>

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
    add_new_collection(scoped_session &session, bool subdirectory)
    {
        auto uri = subdirectory ? std::string("table:") + SUB_DIR + DIR_DELIM + SUB_DIR +
            DIR_DELIM + "collection_" + std::to_string(collection_count()) :
                                  database::build_collection_name(collection_count());
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
void create_collection(scoped_session &session, bool subdirectory = false);
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
void reopen_conn(scoped_session &session, const std::string &conn_config, const std::string &home);

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
create_collection(scoped_session &session, bool subdirectory)
{
    db.add_new_collection(session, subdirectory);
}

void
reopen_conn(scoped_session &session, const std::string &conn_config, const std::string &home)
{
    session.close_session();
    connection_manager::instance().close();
    logger::log_msg(LOG_INFO, "Reopening connection");
    connection_manager::instance().reopen(conn_config, home);
}

static void
do_random_crud(scoped_session &session, const int64_t collection_count, const int64_t op_count,
  const bool fresh_start, const std::string &conn_config, const std::string &home,
  const bool allow_reopen = true, bool subdirectory = false)
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
            create_collection(session, subdirectory);
            file_created = true;
            continue;
        }

        if (i == 0) {
            for (int j = 0; j < warmup_insertions; j++)
                write(session, true);
            i = warmup_insertions;
        }

        if (ran < 3) {
            logger::log_msg(LOG_INFO, "Taking checkpoint");
            // 0.01% Checkpoint.
            testutil_check(session->checkpoint(session.get(), NULL));
            logger::log_msg(LOG_INFO, "Taking checkpoint");
        } else if (ran < 15 && !fresh_start && allow_reopen) {
            logger::log_msg(LOG_INFO, "Commencing connection reopen");
            reopen_conn(session, conn_config, home);
            session = std::move(connection_manager::instance().create_session());
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

/* Setup the initial set of collections in the source path. */
void
configure_database(scoped_session &session)
{
    scoped_cursor cursor = session.open_scoped_cursor("metadata:", "");
    while (cursor->next(cursor.get()) != WT_NOTFOUND) {
        const char *key_str = nullptr;
        testutil_check(cursor->get_key(cursor.get(), &key_str));
        auto metadata_str = std::string(key_str);
        auto pos = metadata_str.find("table:");
        if (pos != std::string::npos) {
            testutil_assert(pos == 0);
            db.add_and_open_existing_collection(session, metadata_str);
            logger::log_msg(LOG_TRACE, "Adding collection: " + metadata_str);
        }
    }
}

// Take a backup of the provided database and then delete the original db. This backup will be used
// in the next loop as a source directory.
static void
take_backup_and_delete_original(const std::string &home, const std::string &backup_dir)
{
    testutil_recreate_dir(backup_dir.c_str());
    const std::string conn_config = "log=(enabled=true,path=journal)";
    connection_manager::instance().reopen(conn_config, home);

    {
        scoped_session backup_session = connection_manager::instance().create_session();
        scoped_cursor backup_cursor = backup_session.open_scoped_cursor("backup:", "");

        while (backup_cursor->next(backup_cursor.get()) == 0) {
            char *file_c_str = nullptr;
            testutil_check(backup_cursor->get_key(backup_cursor.get(), &file_c_str));
            std::string file = std::string(file_c_str);

            // If the file is a log file prepend the hard-coded journal folder path
            if (strncmp(file.c_str(), "WiredTigerLog", 13) == 0) {
                if (!testutil_exists(backup_dir.c_str(), "journal")) {
                    testutil_mkdir((backup_dir + "/" + std::string("journal")).c_str());
                }
                file = "journal/" + file;
            }

            // Create a nested subdirectory to simulate directory per db usage if it does not exist.
            if (WT_PREFIX_MATCH(file_c_str, SUB_DIR)) {
                if (!testutil_exists(backup_dir.c_str(), SUB_DIR)) {
                    auto path = backup_dir + std::string(DIR_DELIM_STR) + SUB_DIR;
                    testutil_mkdir(path.c_str());
                    path += std::string(DIR_DELIM_STR) + SUB_DIR;
                    testutil_mkdir(path.c_str());
                }
            }

            std::string dest_file = home + "/" + file;
            std::string source_file = backup_dir + "/" + file;
            testutil_copy(dest_file.c_str(), source_file.c_str());
        }
    }

    connection_manager::instance().close();
    testutil_remove(home.c_str());
}

static void
run_restore(const std::string &home, const std::string &source, const int64_t thread_count,
  const int64_t collection_count, const int64_t op_count, const int64_t verbose_level,
  const bool die, const bool recovery, bool subdirectory)
{
    /* Create a connection, set the cache size and specify the home directory. */
    const std::string verbose_string = verbose_level == 0 ?
      "" :
      "verbose=[recovery:1,recovery_progress:1,live_restore_progress,live_restore:" +
        std::to_string(verbose_level) + "]";
    const std::string conn_config = CONNECTION_CREATE +
      ",live_restore=(enabled=true,read_size=2MB,threads_max=" + std::to_string(thread_count) +
      ",path=\"" + source + "\"),cache_size=5GB," + verbose_string +
      ",statistics=(all),statistics_log=(json,on_close,wait=1),log=(enabled=true,path=journal)";

    /* Create connection. */
    if (recovery)
        connection_manager::instance().reopen(conn_config, home);
    else
        /*
         * We don't want connection_manager::create() to create the nested subdirectory structure,
         * leave subdirectory as default false and let live restore handle this.
         */
        connection_manager::instance().create(conn_config, home, true);

    auto crud_session = connection_manager::instance().create_session();
    if (recovery)
        configure_database(crud_session);
    // Run 90% of random crud here and do the rest when live restore completes.
    do_random_crud(crud_session, collection_count, (int64_t)(op_count * 0.9), false, conn_config,
      home, true, subdirectory);
    if (die)
        raise(SIGKILL);

    // Loop until the state stat is complete!
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

    /*
     * Once background migration has finished there's no reason for us to access the backing source
     * files any more. Verify this by deleting the backing directory. Any accesses to the deleted
     * files will trigger a crash.
     */
    testutil_remove(SOURCE_PATH);
    logger::log_msg(LOG_INFO, "Run random crud after live restore completion");
    // We've deleted the source folder, so reopening the connection will fail. Disable reopens and
    // do the remaining 10% of random crud operations.
    do_random_crud(crud_session, collection_count, (int64_t)(op_count * 0.1), false, conn_config,
      home, false, subdirectory);

    // We need to close the session here because the connection close will close it out for us if we
    // don't. Then we'll crash because we'll double close a WT session.
    crud_session.close_session();
    connection_manager::instance().close();
}

// Populate an initial database to be live restored. This will be used for the first live restore
// and after that we can use the restored database from the prior iterations as the source.
static void
create_db(const std::string &home, const int64_t thread_count, const int64_t collection_count,
  const int64_t op_count, const int64_t verbose_level, bool subdirectory)
{
    const std::string conn_config = CONNECTION_CREATE +
      ",cache_size=5GB,statistics=(all),statistics_log=(json,on_close,wait=1),log=(enabled=true,"
      "path=journal)";

    // Open the connection and create the log folder. In future runs this is copied by live restore.
    connection_manager::instance().create(conn_config, home, true, subdirectory);

    auto crud_session = connection_manager::instance().create_session();
    do_random_crud(
      crud_session, collection_count, op_count, true, conn_config, home, true, subdirectory);

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
    std::cout << "\t-c The maximum number of collections to run the test with, if unset "
                 "collections are created at random."
              << std::endl;
    std::cout << "\t-D Simulate MongoDB directory per db and directory for indexes configurations "
                 "by creating a subdirectory for the table to populate."
              << std::endl;
    std::cout << "\t-d Die randomly while applying crud operation." << std::endl;
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
    std::cout << "\t-r Start from existing data files and run recovery." << std::endl;
    std::cout << "\t-t Thread count for the background thread. A value greater than 0 must be "
                 "specified."
              << std::endl;
    std::cout << "\t-v Verbose level, this setting will set WT_VERB_FILE_OPS with whatever level "
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
    if (thread_count == 0) {
        logger::log_msg(LOG_ERROR, "Background thread count can't have a value of 0.");
        return EXIT_FAILURE;
    }

    // Get the collection count if it exists.
    auto coll_count_str = value_for_opt("-c", argc, argv);
    int64_t coll_count = INT64_MAX;
    if (!coll_count_str.empty()) {
        coll_count = atoi(coll_count_str.c_str());
        logger::log_msg(LOG_INFO, "Collection count: " + std::to_string(coll_count));
    }

    // Get the subdirectory config.
    auto subdirectory = option_exists("-D", argc, argv);
    logger::log_msg(LOG_INFO, "Subdirectory enabled: " + std::string(subdirectory ? "Y" : "N"));

    // Get the death mode config.
    auto death_mode = option_exists("-d", argc, argv);
    logger::log_msg(LOG_INFO, "Death mode: " + std::string(death_mode ? "Y" : "N"));

    // Get the recovery config.
    auto recovery = option_exists("-r", argc, argv);
    logger::log_msg(LOG_INFO, "Recovery: " + std::string(recovery ? "Y" : "N"));
    if (recovery && it_count > 1) {
        logger::log_msg(LOG_ERROR, "Recovery is only possible for 1 iteration at the moment.");
        return EXIT_FAILURE;
    }

    // Get the op_count if it exists.
    auto op_count_str = value_for_opt("-o", argc, argv);
    int64_t op_count = op_count_str.empty() ? op_count_default : atoi(op_count_str.c_str());
    logger::log_msg(LOG_INFO, "Op count: " + std::to_string(op_count));

    // Get the verbose_level if it exists.
    auto verbose_str = value_for_opt("-v", argc, argv);
    int64_t verbose_level = verbose_str.empty() ? 0 : atoi(verbose_str.c_str());
    logger::log_msg(LOG_INFO, "Verbose level: " + std::to_string(verbose_level));

    // Home path option.
    std::string home_path = value_for_opt("-H", argc, argv);
    if (home_path.empty())
        home_path = HOME_PATH;
    logger::log_msg(LOG_INFO, "Home path: " + home_path);

    // Assuming this run is following a -d "death" run then no folder manipulation is required
    // as the home and source path remain the same.
    if (!recovery) {
        // Delete any existing source dir and home path.
        logger::log_msg(LOG_INFO, "Source path: " + std::string(SOURCE_PATH));
        testutil_remove(SOURCE_PATH);
        testutil_remove(home_path.c_str());

        // We need to create a database to restore from initially.
        create_db(home_path, thread_count, coll_count, op_count, verbose_level, subdirectory);
        take_backup_and_delete_original(home_path, std::string(SOURCE_PATH));
    }

    /* When setting up the database we don't want to wait for the background threads to complete. */
    int death_it = -1;
    if (death_mode && it_count > 1) {
        death_it = random_generator::instance().generate_integer(1, (int)it_count - 1);
        logger::log_msg(LOG_INFO, "Dying on iteration " + std::to_string(death_it));
    }
    for (int i = 0; i < it_count; i++) {
        logger::log_msg(LOG_INFO, "!!!! Beginning iteration: " + std::to_string(i) + " !!!!");
        run_restore(home_path, SOURCE_PATH, thread_count, coll_count, op_count, verbose_level,
          i == death_it, recovery, subdirectory);
        take_backup_and_delete_original(home_path, std::string(SOURCE_PATH));
    }

    return (0);
}
