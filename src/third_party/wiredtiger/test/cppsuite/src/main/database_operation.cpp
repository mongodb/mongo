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

#include "database_operation.h"

#include "src/common/thread_manager.h"
#include "src/common/constants.h"
#include "src/common/logger.h"
#include "src/common/random_generator.h"
#include "src/main/validator.h"
#include "src/storage/connection_manager.h"

namespace test_harness {
/* Static methods. */
static void
populate_worker(thread_worker *tc)
{
    uint64_t collections_per_thread = tc->collection_count / tc->thread_count;

    for (int64_t i = 0; i < collections_per_thread; ++i) {
        collection &coll = tc->db.get_collection((tc->id * collections_per_thread) + i);
        /*
         * WiredTiger lets you open a cursor on a collection using the same pointer. When a session
         * is closed, WiredTiger APIs close the cursors too.
         */
        scoped_cursor cursor = tc->session.open_scoped_cursor(coll.name);
        uint64_t j = 0;
        while (j < tc->key_count) {
            tc->txn.begin();
            auto key = tc->pad_string(std::to_string(j), tc->key_size);
            auto value = random_generator::instance().generate_pseudo_random_string(tc->value_size);
            if (tc->insert(cursor, coll.id, key, value)) {
                if (tc->txn.commit()) {
                    ++j;
                }
            } else {
                tc->txn.rollback();
            }
        }
    }
    logger::log_msg(LOG_TRACE, "Populate: thread {" + std::to_string(tc->id) + "} finished");
}

void
database_operation::populate(
  database &database, timestamp_manager *tsm, configuration *config, operation_tracker *op_tracker)
{
    int64_t collection_count, key_count, key_size, thread_count, value_size;
    std::vector<thread_worker *> workers;
    std::string collection_name;
    thread_manager tm;

    /* Validate our config. */
    collection_count = config->get_int(COLLECTION_COUNT);
    key_count = config->get_int(KEY_COUNT_PER_COLLECTION);
    value_size = config->get_int(VALUE_SIZE);
    thread_count = config->get_int(THREAD_COUNT);
    testutil_assert(thread_count == 0 || collection_count % thread_count == 0);
    testutil_assert(value_size > 0);
    key_size = config->get_int(KEY_SIZE);
    testutil_assert(key_size > 0);
    /* Keys must be unique. */
    testutil_assert(key_count <= pow(10, key_size));

    logger::log_msg(
      LOG_INFO, "Populate: creating " + std::to_string(collection_count) + " collections.");

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
        thread_worker *tc = new thread_worker(i, thread_type::INSERT, config,
          connection_manager::instance().create_session(), tsm, op_tracker, database);
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
database_operation::checkpoint_operation(thread_worker *tc)
{
    logger::log_msg(
      LOG_INFO, type_string(tc->type) + " thread {" + std::to_string(tc->id) + "} commencing.");

    while (tc->running()) {
        tc->sleep();
        /*
         * This may seem like noise but it can prevent the test being killed by the evergreen
         * timeout.
         */
        logger::log_msg(LOG_INFO,
          type_string(tc->type) + " thread {" + std::to_string(tc->id) + "} taking a checkpoint.");
        testutil_check(tc->session->checkpoint(tc->session.get(), nullptr));
    }
}

void
database_operation::custom_operation(thread_worker *tc)
{
    logger::log_msg(
      LOG_INFO, type_string(tc->type) + " thread {" + std::to_string(tc->id) + "} commencing.");
}

void
database_operation::insert_operation(thread_worker *tc)
{
    logger::log_msg(
      LOG_INFO, type_string(tc->type) + " thread {" + std::to_string(tc->id) + "} commencing.");

    /* Helper struct which stores a pointer to a collection and a cursor associated with it. */
    struct collection_cursor {
        collection_cursor(collection &coll, scoped_cursor &&cursor)
            : coll(coll), cursor(std::move(cursor))
        {
        }
        collection &coll;
        scoped_cursor cursor;
    };

    /* Collection cursor vector. */
    std::vector<collection_cursor> ccv;
    uint64_t collection_count = tc->db.get_collection_count();
    testutil_assert(collection_count != 0);
    uint64_t collections_per_thread = collection_count / tc->thread_count;
    /* Must have unique collections for each thread. */
    testutil_assert(collection_count % tc->thread_count == 0);
    for (int i = tc->id * collections_per_thread;
         i < (tc->id * collections_per_thread) + collections_per_thread && tc->running(); ++i) {
        collection &coll = tc->db.get_collection(i);
        scoped_cursor cursor = tc->session.open_scoped_cursor(coll.name);
        ccv.push_back({coll, std::move(cursor)});
    }

    uint64_t counter = 0;
    while (tc->running()) {
        uint64_t start_key = ccv[counter].coll.get_key_count();
        uint64_t added_count = 0;
        tc->txn.begin();

        /* Collection cursor. */
        auto &cc = ccv[counter];
        while (tc->txn.active() && tc->running()) {
            /* Insert a key value pair, rolling back the transaction if required. */
            auto key = tc->pad_string(std::to_string(start_key + added_count), tc->key_size);
            auto value = random_generator::instance().generate_pseudo_random_string(tc->value_size);
            if (!tc->insert(cc.cursor, cc.coll.id, key, value)) {
                added_count = 0;
                tc->txn.rollback();
            } else {
                added_count++;
                if (tc->txn.can_commit()) {
                    if (tc->txn.commit()) {
                        /*
                         * We need to inform the database model that we've added these keys as some
                         * other thread may rely on the key_count data. Only do so if we
                         * successfully committed.
                         */
                        cc.coll.increase_key_count(added_count);
                    } else {
                        added_count = 0;
                    }
                }
            }

            /* Sleep the duration defined by the op_rate. */
            tc->sleep();
        }
        /* Reset our cursor to avoid pinning content. */
        testutil_check(cc.cursor->reset(cc.cursor.get()));
        counter++;
        if (counter == collections_per_thread)
            counter = 0;
        testutil_assert(counter < collections_per_thread);
    }
    /* Make sure the last transaction is rolled back now the work is finished. */
    tc->txn.try_rollback();
}

void
database_operation::read_operation(thread_worker *tc)
{
    logger::log_msg(
      LOG_INFO, type_string(tc->type) + " thread {" + std::to_string(tc->id) + "} commencing.");

    std::map<uint64_t, scoped_cursor> cursors;
    while (tc->running()) {
        /* Get a collection and find a cached cursor. */
        collection &coll = tc->db.get_random_collection();

        if (cursors.find(coll.id) == cursors.end())
            cursors.emplace(coll.id, std::move(tc->session.open_scoped_cursor(coll.name)));

        /* Do a second lookup now that we know it exists. */
        auto &cursor = cursors[coll.id];

        tc->txn.begin();
        while (tc->txn.active() && tc->running()) {
            auto ret = cursor->next(cursor.get());
            if (ret != 0) {
                if (ret == WT_NOTFOUND) {
                    testutil_check(cursor->reset(cursor.get()));
                } else if (ret == WT_ROLLBACK) {
                    tc->txn.rollback();
                    tc->sleep();
                    continue;
                } else
                    testutil_die(ret, "Unexpected error returned from cursor->next()");
            }
            tc->txn.add_op();
            tc->txn.try_rollback();
            tc->sleep();
        }
        /* Reset our cursor to avoid pinning content. */
        testutil_check(cursor->reset(cursor.get()));
    }
    /* Make sure the last transaction is rolled back now the work is finished. */
    tc->txn.try_rollback();
}

void
database_operation::remove_operation(thread_worker *tc)
{
    logger::log_msg(
      LOG_INFO, type_string(tc->type) + " thread {" + std::to_string(tc->id) + "} commencing.");

    /*
     * We need two types of cursors. One cursor is a random cursor to randomly select a key and the
     * other one is a standard cursor to remove the random key. This is required as the random
     * cursor does not support the remove operation.
     */
    std::map<uint64_t, scoped_cursor> rnd_cursors, cursors;

    /* Loop while the test is running. */
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
            /* Open the two cursors for the chosen collection. */
            scoped_cursor rnd_cursor =
              tc->session.open_scoped_cursor(coll.name, "next_random=true");
            rnd_cursors.emplace(coll.id, std::move(rnd_cursor));
            scoped_cursor cursor = tc->session.open_scoped_cursor(coll.name);
            cursors.emplace(coll.id, std::move(cursor));
        }

        /* Start a transaction if possible. */
        tc->txn.try_begin();

        /* Get the cursor associated with the collection. */
        scoped_cursor &rnd_cursor = rnd_cursors[coll.id];
        scoped_cursor &cursor = cursors[coll.id];

        /* Choose a random key to delete. */
        int ret = rnd_cursor->next(rnd_cursor.get());

        if (ret != 0) {
            /*
             * It is possible not to find anything if the collection is empty. In that case, finish
             * the current transaction as we might be able to see new records after starting a new
             * one.
             */
            if (ret == WT_NOTFOUND) {
                WT_IGNORE_RET_BOOL(tc->txn.commit());
            } else if (ret == WT_ROLLBACK) {
                tc->txn.rollback();
            } else {
                testutil_die(ret, "Unexpected error returned from cursor->next()");
            }
            testutil_check(rnd_cursor->reset(rnd_cursor.get()));
            continue;
        }

        const char *key_str;
        testutil_check(rnd_cursor->get_key(rnd_cursor.get(), &key_str));
        if (!tc->remove(cursor, coll.id, key_str)) {
            tc->txn.rollback();
        }

        /* Reset our cursors to avoid pinning content. */
        testutil_check(cursor->reset(cursor.get()));
        testutil_check(rnd_cursor->reset(rnd_cursor.get()));

        /* Commit the current transaction if we're able to. */
        if (tc->txn.can_commit())
            WT_IGNORE_RET_BOOL(tc->txn.commit());
    }

    /* Make sure the last operation is rolled back now the work is finished. */
    tc->txn.try_rollback();
}

void
database_operation::update_operation(thread_worker *tc)
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
            scoped_cursor cursor = tc->session.open_scoped_cursor(coll.name);
            cursors.emplace(coll.id, std::move(cursor));
        }

        /* Start a transaction if possible. */
        tc->txn.try_begin();

        /* Get the cursor associated with the collection. */
        scoped_cursor &cursor = cursors[coll.id];

        /* Choose a random key to update. */
        testutil_assert(coll.get_key_count() != 0);
        auto key_id =
          random_generator::instance().generate_integer<uint64_t>(0, coll.get_key_count() - 1);
        auto key = tc->pad_string(std::to_string(key_id), tc->key_size);
        auto value = random_generator::instance().generate_pseudo_random_string(tc->value_size);
        if (!tc->update(cursor, coll.id, key, value)) {
            tc->txn.rollback();
        }

        /* Reset our cursor to avoid pinning content. */
        testutil_check(cursor->reset(cursor.get()));

        /* Commit the current transaction if we're able to. */
        if (tc->txn.can_commit())
            WT_IGNORE_RET_BOOL(tc->txn.commit());
    }

    /* Make sure the last operation is rolled back now the work is finished. */
    tc->txn.try_rollback();
}

void
database_operation::validate(
  const std::string &operation_table_name, const std::string &schema_table_name, database &db)
{
    validator wv;
    wv.validate(operation_table_name, schema_table_name, db);
}
} // namespace test_harness
