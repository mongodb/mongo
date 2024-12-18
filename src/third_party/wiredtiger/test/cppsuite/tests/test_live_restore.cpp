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

#include <vector>

extern "C" {
#include "wiredtiger.h"
#include "test_util.h"
}

using namespace test_harness;

class database_model {
public:
    /* Collection names start from zero. */
    void
    add_new_collection(scoped_session &session)
    {
        auto uri = database::build_collection_name(collection_count());
        testutil_check(
          session->create(session.get(), uri.c_str(), DEFAULT_FRAMEWORK_SCHEMA.c_str()));
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

static const int crud_ops = 20000;
static const int warmup_insertions = crud_ops / 3;
static database_model db;
static const int key_size = 10;
static const int value_size = 10;
static const char *SOURCE_DIR = "WT_LIVE_RESTORE_SOURCE";

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
    return random_generator::instance().generate_random_string(value_size);
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

void
do_random_crud(scoped_session &session, bool fresh_start)
{
    bool file_created = fresh_start == false;

    /* Insert random data. */
    std::string key, value;
    for (int i = 0; i < crud_ops; i++) {
        auto ran = random_generator::instance().generate_integer(0, 10000);
        if (ran <= 1 || !file_created) {
            // Create a new file, if none exist force this path.s
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
        } else if (ran < 5000) {
            // 50% Write.
            write(session, false);
        } else if (ran <= 9980) {
            // 49.8% Read.
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

static void
get_stat(WT_CURSOR *cursor, int stat_field, int64_t *valuep)
{
    const char *desc, *pvalue;

    cursor->set_key(cursor, stat_field);
    error_check(cursor->search(cursor));
    error_check(cursor->get_value(cursor, &desc, &pvalue, valuep));
}

int
main(int argc, char *argv[])
{
    /* Set the program name for error messages. */
    const std::string progname = testutil_set_progname(argv);
    /* Set the tracing level for the logger component. */
    logger::trace_level = LOG_TRACE;

    /* Create a connection, set the cache size and specify the home directory. */
    /* FIXME-WT-13825: Set max_threads to non zero once extent list concurrency is implemented. */
    // TODO: Make verbosity level configurable at runtime.
    const std::string conn_config = CONNECTION_CREATE +
      ",live_restore=(enabled=true,threads_max=0,path=\"" + SOURCE_DIR +
      "\"),cache_size=1GB,verbose=[fileops:2],statistics=(all),statistics_log=(json,on_close,wait="
      "1)";

    logger::log_msg(LOG_TRACE, "arg count: " + std::to_string(argc));
    bool fresh_start = false;
    bool background_debug = false;
    if (argc > 1 && argv[1][1] == 'f') {
        fresh_start = true;
        logger::log_msg(LOG_WARN, "Started in -f mode will clean up existing directories");
        // Live restore expects the source directory to exist.
        testutil_recreate_dir(SOURCE_DIR);
        testutil_remove("WT_TEST");
    } else if (argc > 1 && argv[1][1] == 'b') {
        /*
         * Operate in background thread debug mode, in this case we turn off CRUD operations and
         * just wait until the relevant statistic is set then exit.
         *
         * Right now background_debug and fresh_start are mutually exclusive.
         */
        background_debug = true;
    }
    // We will recreate this directory every time, on exit the contents in it will be moved to
    // WT_LIVE_RESTORE_SOURCE/.
    testutil_recreate_dir("WT_TEST");

    /* Create connection. */
    if (fresh_start)
        connection_manager::instance().create(conn_config, DEFAULT_DIR);
    else
        connection_manager::instance().reopen(conn_config, DEFAULT_DIR);

    auto crud_session = connection_manager::instance().create_session();

    if (!fresh_start && !background_debug)
        configure_database(crud_session);

    if (background_debug) {
        // Loop until the state stat is complete!
        int64_t state = 0;
        while (state != WT_LIVE_RESTORE_COMPLETE) {
            auto stat_cursor = crud_session.open_scoped_cursor("statistics:");
            get_stat(stat_cursor.get(), WT_STAT_CONN_LIVE_RESTORE_STATE, &state);
            __wt_sleep(1, 0);
        }
    } else
        do_random_crud(crud_session, fresh_start);

    // We need to close the session here because the connection close will close it out for us if we
    // don't. Then we'll crash because we'll double close a WT session.
    crud_session.close_session();
    connection_manager::instance().close();
    testutil_remove(SOURCE_DIR);
    testutil_copy("WT_TEST", SOURCE_DIR);
    return (0);
}
