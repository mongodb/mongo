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

#include <cmath>
#include <map>

#include "../connection_manager.h"
#include "../thread_manager.h"
#include "../util/api_const.h"
#include "database_operation.h"
#include "random_generator.h"
#include "workload_tracking.h"

namespace test_harness {
/* Static methods. */
static void
populate_worker(thread_context *tc)
{
    uint64_t collections_per_thread = tc->collection_count / tc->thread_count;
    for (int64_t i = 0; i < collections_per_thread; ++i) {
        collection &coll = tc->db.get_collection((tc->id * collections_per_thread) + i);
        /*
         * WiredTiger lets you open a cursor on a collection using the same pointer. When a session
         * is closed, WiredTiger APIs close the cursors too.
         */
        scoped_cursor cursor = tc->session.open_scoped_cursor(coll.name.c_str());
        for (uint64_t i = 0; i < tc->key_count; ++i) {
            /* Start a txn. */
            tc->transaction.begin();
            if (!tc->insert(cursor, coll.id, i))
                testutil_die(-1, "Got a rollback in populate, this is currently not handled.");
            tc->transaction.commit();
        }
    }
    logger::log_msg(LOG_TRACE, "Populate: thread {" + std::to_string(tc->id) + "} finished");
}

/* database_operation class implementation. */
void
database_operation::populate(
  database &database, timestamp_manager *tsm, configuration *config, workload_tracking *tracking)
{
    int64_t collection_count, key_count, key_size, thread_count, value_size;
    std::vector<thread_context *> workers;
    std::string collection_name;
    thread_manager tm;

    /* Validate our config. */
    collection_count = config->get_int(COLLECTION_COUNT);
    key_count = config->get_int(KEY_COUNT_PER_COLLECTION);
    value_size = config->get_int(VALUE_SIZE);
    thread_count = config->get_int(THREAD_COUNT);
    testutil_assert(collection_count % thread_count == 0);
    testutil_assert(value_size > 0);
    key_size = config->get_int(KEY_SIZE);
    testutil_assert(key_size > 0);
    /* Keys must be unique. */
    testutil_assert(key_count <= pow(10, key_size));

    /* Create n collections as per the configuration. */
    for (int64_t i = 0; i < collection_count; ++i)
        /*
         * The database model will call into the API and create the collection, with its own
         * session.
         */
        database.add_collection(key_count);

    logger::log_msg(
      LOG_INFO, "Populate: " + std::to_string(collection_count) + " collections created.");

    /*
     * Spawn thread_count threads to populate the database, theoretically we should be IO bound
     * here.
     */
    for (int64_t i = 0; i < thread_count; ++i) {
        thread_context *tc = new thread_context(i, thread_type::INSERT, config,
          connection_manager::instance().create_session(), tsm, tracking, database);
        workers.push_back(tc);
        tm.add_thread(populate_worker, tc);
    }

    /* Wait for our populate threads to finish and then join them. */
    logger::log_msg(LOG_INFO, "Populate: waiting for threads to complete.");
    tm.join();

    /* Cleanup our workers. */
    for (auto &it : workers) {
        delete it;
        it = nullptr;
    }
    logger::log_msg(LOG_INFO, "Populate: finished.");
}

void
database_operation::insert_operation(thread_context *tc)
{
    /* Helper struct which stores a pointer to a collection and a cursor associated with it. */
    struct collection_cursor {
        collection_cursor(collection &coll, scoped_cursor &&cursor)
            : coll(coll), cursor(std::move(cursor))
        {
        }
        collection &coll;
        scoped_cursor cursor;
    };
    logger::log_msg(
      LOG_INFO, type_string(tc->type) + " thread {" + std::to_string(tc->id) + "} commencing.");

    /* Collection cursor vector. */
    std::vector<collection_cursor> ccv;
    uint64_t collection_count = tc->db.get_collection_count();
    uint64_t collections_per_thread = collection_count / tc->thread_count;
    /* Must have unique collections for each thread. */
    testutil_assert(collection_count % tc->thread_count == 0);
    for (int i = tc->id * collections_per_thread;
         i < (tc->id * collections_per_thread) + collections_per_thread && tc->running(); ++i) {
        collection &coll = tc->db.get_collection(i);
        scoped_cursor cursor = tc->session.open_scoped_cursor(coll.name.c_str());
        ccv.push_back({coll, std::move(cursor)});
    }

    uint64_t counter = 0;
    while (tc->running()) {
        uint64_t start_key = ccv[counter].coll.get_key_count();
        uint64_t added_count = 0;
        bool committed = true;
        tc->transaction.begin();

        /* Collection cursor. */
        auto &cc = ccv[counter];
        while (tc->transaction.active() && tc->running()) {
            /* Insert a key value pair. */
            if (!tc->insert(cc.cursor, cc.coll.id, start_key + added_count)) {
                committed = false;
                break;
            }
            added_count++;
            tc->transaction.try_commit();
            /* Sleep the duration defined by the op_rate. */
            tc->sleep();
        }
        /* Reset our cursor to avoid pinning content. */
        testutil_check(cc.cursor->reset(cc.cursor.get()));

        /*
         * We need to inform the database model that we've added these keys as some other thread may
         * rely on the key_count data. Only do so if we successfully committed.
         */
        if (committed)
            cc.coll.increase_key_count(added_count);
        counter++;
        if (counter == collections_per_thread)
            counter = 0;
    }
    /* Make sure the last transaction is rolled back now the work is finished. */
    if (tc->transaction.active())
        tc->transaction.rollback();
}

void
database_operation::read_operation(thread_context *tc)
{
    logger::log_msg(
      LOG_INFO, type_string(tc->type) + " thread {" + std::to_string(tc->id) + "} commencing.");
    WT_CURSOR *cursor = nullptr;
    WT_DECL_RET;
    std::map<uint64_t, scoped_cursor> cursors;
    while (tc->running()) {
        /* Get a collection and find a cached cursor. */
        collection &coll = tc->db.get_random_collection();
        const auto &it = cursors.find(coll.id);
        if (it == cursors.end()) {
            auto sc = tc->session.open_scoped_cursor(coll.name.c_str());
            cursor = sc.get();
            cursors.emplace(coll.id, std::move(sc));
        } else
            cursor = it->second.get();

        tc->transaction.begin();
        while (tc->transaction.active() && tc->running()) {
            ret = cursor->next(cursor);
            /* If we get not found here we walked off the end. */
            if (ret == WT_NOTFOUND) {
                testutil_check(cursor->reset(cursor));
            } else if (ret != 0)
                testutil_die(ret, "cursor->next() failed");
            tc->transaction.add_op();
            tc->transaction.try_rollback();
            tc->sleep();
        }
        /* Reset our cursor to avoid pinning content. */
        testutil_check(cursor->reset(cursor));
    }
    /* Make sure the last transaction is rolled back now the work is finished. */
    if (tc->transaction.active())
        tc->transaction.rollback();
}

void
database_operation::update_operation(thread_context *tc)
{
    logger::log_msg(
      LOG_INFO, type_string(tc->type) + " thread {" + std::to_string(tc->id) + "} commencing.");
    /* Cursor map. */
    std::map<uint64_t, scoped_cursor> cursors;

    /*
     * Loop while the test is running.
     */
    while (tc->running()) {
        /*
         * Sleep the period defined by the op_rate in the configuration. Do this at the start of the
         * loop as it could be skipped by a subsequent continue call.
         */
        tc->sleep();

        /* Choose a random collection to update. */
        collection &coll = tc->db.get_random_collection();

        /* Look for existing cursors in our cursor cache. */
        if (cursors.find(coll.id) == cursors.end()) {
            logger::log_msg(LOG_TRACE,
              "Thread {" + std::to_string(tc->id) +
                "} Creating cursor for collection: " + coll.name);
            /* Open a cursor for the chosen collection. */
            scoped_cursor cursor = tc->session.open_scoped_cursor(coll.name.c_str());
            cursors.emplace(coll.id, std::move(cursor));
        }

        /* Start a transaction if possible. */
        tc->transaction.try_begin();

        /* Get the cursor associated with the collection. */
        scoped_cursor &cursor = cursors[coll.id];

        /* Choose a random key to update. */
        uint64_t key_id =
          random_generator::instance().generate_integer<uint64_t>(0, coll.get_key_count() - 1);
        bool successful_update = tc->update(cursor, coll.id, tc->key_to_string(key_id));

        /* Reset our cursor to avoid pinning content. */
        testutil_check(cursor->reset(cursor.get()));

        /* We received a rollback in update. */
        if (!successful_update)
            continue;

        /* Commit the current transaction if we're able to. */
        tc->transaction.try_commit();
    }

    /* Make sure the last operation is committed now the work is finished. */
    if (tc->transaction.active())
        tc->transaction.commit();
}
} // namespace test_harness
