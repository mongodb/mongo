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

#include "src/common/constants.h"
#include "src/common/logger.h"
#include "src/common/random_generator.h"
#include "src/component/operation_tracker.h"
#include "src/main/test.h"

using namespace test_harness;

/* Defines what data is written to the tracking table for use in custom validation. */
class operation_tracker_cache_resize : public operation_tracker {

public:
    operation_tracker_cache_resize(
      configuration *config, const bool use_compression, timestamp_manager &tsm)
        : operation_tracker(config, use_compression, tsm)
    {
    }

    void
    set_tracking_cursor(WT_SESSION *session, const tracking_operation &operation, const uint64_t &,
      const std::string &, const std::string &value, wt_timestamp_t ts,
      scoped_cursor &op_track_cursor) override final
    {
        uint64_t txn_id = ((WT_SESSION_IMPL *)session)->txn->id;
        /*
         * The cache_size may have been changed between the time we make an insert to the DB and
         * when we write the details to the tracking table, as such we can't take cache_size from
         * the connection. Instead, write the cache size as part of the atomic insert into the DB
         * and when populating the tracking table take it from there.
         */
        uint64_t cache_size = std::stoull(value);

        op_track_cursor->set_key(op_track_cursor.get(), ts, txn_id);
        op_track_cursor->set_value(op_track_cursor.get(), operation, cache_size);
    }
};

/*
 * This test continuously writes transactions larger than 1MB but less than 500MB into the database,
 * while switching the connection cache size between 1MB and 500MB. When transactions are larger
 * than the cache size they are rejected, so only transactions made when cache size is 500MB should
 * be allowed.
 */
class cache_resize : public test {
public:
    cache_resize(const test_args &args) : test(args)
    {
        init_operation_tracker(
          new operation_tracker_cache_resize(_config->get_subconfig(OPERATION_TRACKER),
            _config->get_bool(COMPRESSION_ENABLED), *_timestamp_manager));
    }

    void
    custom_operation(thread_worker *tc) override final
    {
        WT_CONNECTION *conn = connection_manager::instance().get_connection();
        WT_CONNECTION_IMPL *conn_impl = (WT_CONNECTION_IMPL *)conn;
        bool increase_cache = false;
        const std::string small_cache_size = "cache_size=1MB";
        const std::string big_cache_size = "cache_size=500MB";

        while (tc->running()) {
            tc->sleep();

            /* Get the current cache size. */
            uint64_t prev_cache_size = conn_impl->cache_size;

            /* Reconfigure with the new cache size. */
            testutil_check(conn->reconfigure(
              conn, increase_cache ? big_cache_size.c_str() : small_cache_size.c_str()));

            /* Get the new cache size. */
            uint64_t new_cache_size = conn_impl->cache_size;

            logger::log_msg(LOG_TRACE,
              "The cache size was updated from " + std::to_string(prev_cache_size) + " to " +
                std::to_string(new_cache_size));

            /*
             * The collection id and the key are dummy fields which are required by the
             * save_operation API but not needed for this test.
             */
            const uint64_t collection_id = 0;
            const std::string key;
            const std::string value = std::to_string(new_cache_size);

            /* Save the change of cache size in the tracking table. */
            tc->txn.begin();
            int ret = tc->op_tracker->save_operation(tc->session.get(), tracking_operation::CUSTOM,
              collection_id, key, value, tc->tsm->get_next_ts(), tc->op_track_cursor);

            if (ret == 0)
                testutil_assert(tc->txn.commit());
            else {
                /* Due to the cache pressure, it is possible to fail when saving the operation. */
                testutil_assert(ret == WT_ROLLBACK);
                logger::log_msg(LOG_WARN,
                  "The cache size reconfiguration could not be saved in the tracking table, ret: " +
                    std::to_string(ret));
                tc->txn.rollback();
            }
            increase_cache = !increase_cache;
        }
    }

    void
    insert_operation(thread_worker *tc) override final
    {
        const uint64_t collection_count = tc->db.get_collection_count();
        testutil_assert(collection_count > 0);
        collection &coll = tc->db.get_collection(collection_count - 1);
        scoped_cursor cursor = tc->session.open_scoped_cursor(coll.name);

        while (tc->running()) {
            tc->sleep();

            /* Insert the current cache size value using a random key. */
            const std::string key =
              random_generator::instance().generate_pseudo_random_string(tc->key_size);
            const uint64_t cache_size =
              ((WT_CONNECTION_IMPL *)connection_manager::instance().get_connection())->cache_size;
            const std::string value = std::to_string(cache_size);

            tc->txn.try_begin();
            if (!tc->insert(cursor, coll.id, key, value)) {
                tc->txn.rollback();
            } else if (tc->txn.can_commit()) {
                /*
                 * The transaction can fit in the current cache size and is ready to be committed.
                 * This means the tracking table will contain a new record to represent this
                 * transaction which will be used during the validation stage.
                 */
                testutil_assert(tc->txn.commit());
            }
        }

        /* Make sure the last transaction is rolled back now the work is finished. */
        tc->txn.try_rollback();
    }

    void
    validate(
      const std::string &operation_table_name, const std::string &, database &) override final
    {
        bool first_record = true;
        int ret;
        uint64_t cache_size, num_records = 0, prev_txn_id;
        const uint64_t cache_size_500mb = 500000000;

        /* FIXME-WT-9339. */
        (void)cache_size;
        (void)cache_size_500mb;

        /* Open a cursor on the tracking table to read it. */
        scoped_session session = connection_manager::instance().create_session();
        scoped_cursor cursor = session.open_scoped_cursor(operation_table_name);

        /*
         * Parse the tracking table. Each operation is tracked and each transaction is made of
         * multiple operations, hence we expect multiple records for each transaction. We only need
         * to verify that the cache size was big enough when the transaction was committed, which
         * means at the last operation.
         */
        while ((ret = cursor->next(cursor.get())) == 0) {

            uint64_t tracked_ts, tracked_txn_id;
            int tracked_op_type;
            uint64_t tracked_cache_size;

            testutil_check(cursor->get_key(cursor.get(), &tracked_ts, &tracked_txn_id));
            testutil_check(cursor->get_value(cursor.get(), &tracked_op_type, &tracked_cache_size));

            logger::log_msg(LOG_TRACE,
              "Timestamp: " + std::to_string(tracked_ts) +
                ", transaction id: " + std::to_string(tracked_txn_id) +
                ", cache size: " + std::to_string(tracked_cache_size));

            tracking_operation op_type = static_cast<tracking_operation>(tracked_op_type);
            /* There are only two types of operation tracked. */
            testutil_assert(
              op_type == tracking_operation::CUSTOM || op_type == tracking_operation::INSERT);

            /*
             * There is nothing to do if we are reading a record that indicates a cache size change.
             */
            if (op_type == tracking_operation::CUSTOM)
                continue;

            if (first_record) {
                first_record = false;
            } else if (prev_txn_id != tracked_txn_id) {
                /*
                 * We have moved to a new transaction, make sure the cache was big enough when the
                 * previous transaction was committed.
                 *
                 * FIXME-WT-9339 - Somehow we have some transactions that go through while the cache
                 * is very low. Enable the check when this is no longer the case.
                 *
                 * testutil_assert(cache_size > cache_size_500mb);
                 */
            }
            prev_txn_id = tracked_txn_id;
            /*
             * FIXME-WT-9339 - Save the last cache size seen by the transaction.
             *
             * cache_size = tracked_cache_size;
             */
            ++num_records;
        }
        /* All records have been parsed, the last one still needs the be checked. */
        testutil_assert(ret == WT_NOTFOUND);
        testutil_assert(num_records > 0);
        /*
         * FIXME-WT-9339 - Somehow we have some transactions that go through while the cache is very
         * low. Enable the check when this is no longer the case.
         *
         * testutil_assert(cache_size > cache_size_500mb);
         */
    }
};
