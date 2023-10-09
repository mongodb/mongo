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
#include "src/main/test.h"
#include "src/main/validator.h"

namespace test_harness {

/* This test produces a workload that encourages the background compaction server to do work by:
 *      1. Performing truncations that remove 20% of keys on randomly selected tables.
 *      2. Providing a "maintenance window" which allows compact to continue running while all other
 *         operations are paused. The period of the maintenance window is set by the custom
 *         operations op_rate.
 *      3. Performing inserts to ensure the files continue to grow.
 */
class background_compact : public test {
    volatile bool maintenance_window = false;

public:
    background_compact(const test_args &args) : test(args)
    {
        init_operation_tracker();
    }

    /* Custom operation to simulate toggling maintenance windows in a workload. */
    void
    custom_operation(thread_worker *tw) override final
    {
        const std::string log_prefix =
          type_string(tw->type) + " thread {" + std::to_string(tw->id) + "}: ";
        logger::log_msg(LOG_INFO, log_prefix + "commencing.");

        while (tw->running()) {
            /* Sleep first to let threads do some work. */
            tw->sleep();

            maintenance_window = !maintenance_window;
            std::string state = maintenance_window ? "On" : "Off";
            logger::log_msg(LOG_TRACE, log_prefix + " toggle maintenance window " + state);
        }
    }

    void
    remove_operation(thread_worker *tw) override final
    {
        const std::string log_prefix =
          type_string(tw->type) + " thread {" + std::to_string(tw->id) + "}: ";
        logger::log_msg(LOG_INFO, log_prefix + "commencing.");

        std::map<uint64_t, scoped_cursor> rnd_cursors, stat_cursors;

        /* Loop while the test is running. */
        while (tw->running()) {
            /* Make sure we're not doing any work during the maintenance window. */
            if (maintenance_window) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }

            /*
             * Sleep the period defined by the op_rate in the configuration. Do this at the start of
             * the loop as it could be skipped by a subsequent continue call.
             */
            tw->sleep();

            /* Choose a random collection to truncate. */
            collection &coll = tw->db.get_random_collection();

            /* Look for an existing random cursor in our cursor cache. */
            if (rnd_cursors.find(coll.id) == rnd_cursors.end()) {
                logger::log_msg(
                  LOG_TRACE, log_prefix + "Creating cursors for collection: " + coll.name);
                /* Open the two cursors for the chosen collection. */
                scoped_cursor rnd_cursor =
                  tw->session.open_scoped_cursor(coll.name, "next_random=true");
                rnd_cursors.emplace(coll.id, std::move(rnd_cursor));
                std::string stat_uri = STATISTICS_URI + coll.name;
                scoped_cursor stat_cursor = tw->session.open_scoped_cursor(stat_uri);
                stat_cursors.emplace(coll.id, std::move(stat_cursor));
            }

            /* Get the cursors associated with the collection. */
            scoped_cursor &stat_cursor = stat_cursors[coll.id];
            scoped_cursor &rnd_cursor = rnd_cursors[coll.id];

            /* Get the file statistics so we know how much to truncate. */
            int64_t entries, bytes_avail_reuse, file_size;
            metrics_monitor::get_stat(stat_cursor, WT_STAT_DSRC_BTREE_ENTRIES, &entries);
            metrics_monitor::get_stat(
              stat_cursor, WT_STAT_DSRC_BLOCK_REUSE_BYTES, &bytes_avail_reuse);
            metrics_monitor::get_stat(stat_cursor, WT_STAT_DSRC_BLOCK_SIZE, &file_size);

            /* Don't truncate if we already have enough free space for compact to do work. */
            const int64_t pct_free_space_threshold = 10;
            int64_t pct_free_space = (bytes_avail_reuse * 100 / file_size);
            if (pct_free_space > pct_free_space_threshold) {
                logger::log_msg(LOG_INFO,
                  log_prefix + "Skip truncating collection {" + coll.name +
                    "}, enough free space available (" + std::to_string(pct_free_space) + "%)");
                testutil_check(stat_cursor->reset(stat_cursor.get()));
                continue;
            }

            /*
             * Truncate a range of keys between 0 and 100 until we've truncated a total of 20% of
             * the entries in the table.
             */
            int64_t n_keys_to_truncate = (entries / 100) * 20;
            int64_t keys_truncated = 0, retries = 0;
            const uint64_t MAX_RETRIES = 100;
            while (tw->running() && keys_truncated < n_keys_to_truncate && retries < MAX_RETRIES) {
                /* Start a transaction if possible. */
                tw->txn.try_begin();

                /* Choose a random key to delete. */
                int ret = rnd_cursor->next(rnd_cursor.get());

                if (ret != 0) {
                    /*
                     * It is possible not to find anything if the collection is empty. In that case,
                     * finish the current transaction as we might be able to see new records after
                     * starting a new one.
                     */
                    if (ret == WT_NOTFOUND)
                        WT_IGNORE_RET_BOOL(tw->txn.commit());
                    else if (ret == WT_ROLLBACK)
                        tw->txn.rollback();
                    else
                        testutil_die(ret, "Unexpected error returned from cursor->next()");

                    testutil_check(rnd_cursor->reset(rnd_cursor.get()));
                    break;
                }

                const char *key_str;
                testutil_check(rnd_cursor->get_key(rnd_cursor.get(), &key_str));

                const std::string first_key(key_str);
                uint64_t truncate_range =
                  random_generator::instance().generate_integer<uint64_t>(0, 100);
                const std::string end_key = tw->pad_string(
                  std::to_string(std::stoi(first_key) + truncate_range), first_key.size());

                /*
                 * If we generate an invalid range or our truncate fails rollback the transaction.
                 */
                if (end_key == first_key || !tw->truncate(coll.id, first_key, end_key, "")) {
                    tw->txn.rollback();
                    if (end_key == first_key)
                        logger::log_msg(
                          LOG_TRACE, log_prefix + "truncate failed because of an invalid range");
                    else
                        logger::log_msg(LOG_TRACE, log_prefix + "truncate call failed");
                    retries++;
                    continue;
                }

                if (tw->txn.commit()) {
                    logger::log_msg(LOG_TRACE,
                      log_prefix + " committed truncation of " + std::to_string(truncate_range) +
                        " records.");
                    keys_truncated += truncate_range;
                } else {
                    logger::log_msg(LOG_TRACE,
                      log_prefix + "failed to commit truncation of " +
                        std::to_string(truncate_range) + " records.");
                    retries++;
                }

                /* Reset our cursor to avoid pinning content. */
                testutil_check(rnd_cursor->reset(rnd_cursor.get()));
            }

            logger::log_msg(LOG_TRACE,
              log_prefix + "truncated " + std::to_string(keys_truncated) + " keys out of " +
                std::to_string(n_keys_to_truncate));

            /*
             * Take a checkpoint here so we can read the correct statistics next time we hit this
             * file.
             */
            testutil_check(tw->session->checkpoint(tw->session.get(), nullptr));
        }

        /* Make sure the last operation is rolled back now the work is finished. */
        tw->txn.try_rollback();
    }

    void
    insert_operation(thread_worker *tw) override final
    {
        logger::log_msg(
          LOG_INFO, type_string(tw->type) + " thread {" + std::to_string(tw->id) + "} commencing.");

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
        uint64_t collection_count = tw->db.get_collection_count();
        testutil_assert(collection_count != 0);
        uint64_t collections_per_thread = collection_count / tw->thread_count;
        /* Must have unique collections for each thread. */
        testutil_assert(collection_count % tw->thread_count == 0);
        for (int i = tw->id * collections_per_thread;
             i < (tw->id * collections_per_thread) + collections_per_thread && tw->running(); ++i) {
            collection &coll = tw->db.get_collection(i);
            scoped_cursor cursor = tw->session.open_scoped_cursor(coll.name);
            ccv.push_back({coll, std::move(cursor)});
        }

        uint64_t counter = 0;
        while (tw->running()) {
            if (maintenance_window) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }

            uint64_t start_key = ccv[counter].coll.get_key_count();
            uint64_t added_count = 0;
            tw->txn.begin();

            /* Collection cursor. */
            auto &cc = ccv[counter];
            while (tw->txn.active() && tw->running()) {
                /* Insert a key value pair, rolling back the transaction if required. */
                auto key = tw->pad_string(std::to_string(start_key + added_count), tw->key_size);
                auto value =
                  random_generator::instance().generate_pseudo_random_string(tw->value_size);
                if (!tw->insert(cc.cursor, cc.coll.id, key, value)) {
                    added_count = 0;
                    tw->txn.rollback();
                } else {
                    added_count++;
                    if (tw->txn.can_commit()) {
                        if (tw->txn.commit())
                            /*
                             * We need to inform the database model that we've added these keys as
                             * some other thread may rely on the key_count data. Only do so if we
                             * successfully committed.
                             */
                            cc.coll.increase_key_count(added_count);
                        else
                            added_count = 0;
                    }
                }

                /* Sleep the duration defined by the op_rate. */
                tw->sleep();
            }
            /* Reset our cursor to avoid pinning content. */
            testutil_check(cc.cursor->reset(cc.cursor.get()));
            counter++;
            if (counter == collections_per_thread)
                counter = 0;
            testutil_assert(counter < collections_per_thread);
        }
        /* Make sure the last transaction is rolled back now the work is finished. */
        tw->txn.try_rollback();
    }

    void
    background_compact_operation(thread_worker *tw) override final
    {
        logger::log_msg(
          LOG_INFO, type_string(tw->type) + " thread {" + std::to_string(tw->id) + "} commencing.");

        bool enabled = false;

        while (tw->running()) {
            enabled = !enabled;

            std::string compact_cfg = enabled ?
              "background=true,free_space_target=" + std::to_string(tw->free_space_target_mb) +
                "MB" :
              "background=false";

            /* We never expect the background compaction to fail when being enabled/disabled. */
            testutil_check(tw->session->compact(tw->session.get(), nullptr, compact_cfg.c_str()));

            tw->sleep();
        }
    }

    void
    validate(bool, const std::string &, const std::string &, database &) override final
    {
        logger::log_msg(LOG_INFO, "Starting validation.");
        scoped_session session = connection_manager::instance().create_session();

        /* Check the background compact statistics. */
        int64_t bytes_recovered, bytes_rewritten_ema, bytes_written, files_tracked, skipped,
          success;
        scoped_cursor conn_stat_cursor = session.open_scoped_cursor(STATISTICS_URI);

        metrics_monitor::get_stat(
          conn_stat_cursor, WT_STAT_CONN_BACKGROUND_COMPACT_BYTES_RECOVERED, &bytes_recovered);
        testutil_assert(bytes_recovered > 0);

        metrics_monitor::get_stat(
          conn_stat_cursor, WT_STAT_CONN_BACKGROUND_COMPACT_EMA, &bytes_rewritten_ema);
        testutil_assert(bytes_rewritten_ema > 0);

        metrics_monitor::get_stat(
          conn_stat_cursor, WT_STAT_CONN_BLOCK_BYTE_WRITE_COMPACT, &bytes_written);
        testutil_assert(bytes_written > 0);

        metrics_monitor::get_stat(
          conn_stat_cursor, WT_STAT_CONN_BACKGROUND_COMPACT_FILES_TRACKED, &files_tracked);
        testutil_assert(files_tracked > 0);

        metrics_monitor::get_stat(
          conn_stat_cursor, WT_STAT_CONN_BACKGROUND_COMPACT_SKIPPED, &skipped);
        testutil_assert(skipped > 0);

        metrics_monitor::get_stat(
          conn_stat_cursor, WT_STAT_CONN_BACKGROUND_COMPACT_SUCCESS, &success);
        testutil_assert(success > 0);

        logger::log_msg(LOG_INFO, "Validation successful.");
    }
};

} // namespace test_harness
