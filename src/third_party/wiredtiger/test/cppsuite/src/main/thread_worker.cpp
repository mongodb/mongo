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

#include "thread_worker.h"

#include <thread>

#include "src/common/constants.h"
#include "src/common/logger.h"
#include "src/common/random_generator.h"
#include "transaction.h"

namespace test_harness {

const std::string
type_string(thread_type type)
{
    switch (type) {
    case thread_type::CHECKPOINT:
        return ("checkpoint");
    case thread_type::CUSTOM:
        return ("custom");
    case thread_type::INSERT:
        return ("insert");
    case thread_type::READ:
        return ("read");
    case thread_type::REMOVE:
        return ("remove");
    case thread_type::UPDATE:
        return ("update");
    }
    testutil_die(EINVAL, "unexpected thread_type: %d", static_cast<int>(type));
}

thread_worker::thread_worker(uint64_t id, thread_type type, configuration *config,
  scoped_session &&created_session, timestamp_manager *timestamp_manager,
  operation_tracker *op_tracker, database &dbase)
    : thread_worker(
        id, type, config, std::move(created_session), timestamp_manager, op_tracker, dbase, nullptr)
{
}

thread_worker::thread_worker(uint64_t id, thread_type type, configuration *config,
  scoped_session &&created_session, timestamp_manager *timestamp_manager,
  operation_tracker *op_tracker, database &dbase, std::shared_ptr<barrier> barrier_ptr)
    : /* These won't exist for certain threads which is why we use optional here. */
      collection_count(config->get_optional_int(COLLECTION_COUNT, 1)),
      key_count(config->get_optional_int(KEY_COUNT_PER_COLLECTION, 1)),
      key_size(config->get_optional_int(KEY_SIZE, 1)),
      value_size(config->get_optional_int(VALUE_SIZE, 1)),
      thread_count(config->get_int(THREAD_COUNT)), type(type), id(id), db(dbase),
      session(std::move(created_session)), tsm(timestamp_manager),
      txn(transaction(config, timestamp_manager, session.get())), op_tracker(op_tracker),
      _sleep_time_ms(config->get_throttle_ms()), _barrier(barrier_ptr)
{
    if (op_tracker->enabled())
        op_track_cursor = session.open_scoped_cursor(op_tracker->get_operation_table_name());

    testutil_assert(key_size > 0 && value_size > 0);
}

void
thread_worker::finish()
{
    _running = false;
}

std::string
thread_worker::pad_string(const std::string &value, uint64_t size)
{
    uint64_t diff = size > value.size() ? size - value.size() : 0;
    std::string s(diff, '0');
    return (s.append(value));
}

bool
thread_worker::update(
  scoped_cursor &cursor, uint64_t collection_id, const std::string &key, const std::string &value)
{
    WT_DECL_RET;

    testutil_assert(op_tracker != nullptr);
    testutil_assert(cursor.get() != nullptr);

    wt_timestamp_t ts = tsm->get_next_ts();
    ret = txn.set_commit_timestamp(ts);
    testutil_assert(ret == 0 || ret == EINVAL);
    if (ret != 0) {
        txn.set_needs_rollback(true);
        return (false);
    }

    cursor->set_key(cursor.get(), key.c_str());
    cursor->set_value(cursor.get(), value.c_str());
    ret = cursor->update(cursor.get());

    if (ret != 0) {
        if (ret == WT_ROLLBACK) {
            txn.set_needs_rollback(true);
            return (false);
        } else
            testutil_die(ret, "unhandled error while trying to update a key");
    }

    ret = op_tracker->save_operation(
      session.get(), tracking_operation::INSERT, collection_id, key, value, ts, op_track_cursor);

    if (ret == 0)
        txn.add_op();
    else if (ret == WT_ROLLBACK)
        txn.set_needs_rollback(true);
    else
        testutil_die(ret, "unhandled error while trying to save an update to the tracking table");
    return (ret == 0);
}

bool
thread_worker::insert(
  scoped_cursor &cursor, uint64_t collection_id, const std::string &key, const std::string &value)
{
    WT_DECL_RET;

    testutil_assert(op_tracker != nullptr);
    testutil_assert(cursor.get() != nullptr);

    wt_timestamp_t ts = tsm->get_next_ts();
    ret = txn.set_commit_timestamp(ts);
    testutil_assert(ret == 0 || ret == EINVAL);
    if (ret != 0) {
        txn.set_needs_rollback(true);
        return (false);
    }

    cursor->set_key(cursor.get(), key.c_str());
    cursor->set_value(cursor.get(), value.c_str());
    ret = cursor->insert(cursor.get());

    if (ret != 0) {
        if (ret == WT_ROLLBACK) {
            txn.set_needs_rollback(true);
            return (false);
        } else
            testutil_die(ret, "unhandled error while trying to insert a key");
    }

    ret = op_tracker->save_operation(
      session.get(), tracking_operation::INSERT, collection_id, key, value, ts, op_track_cursor);

    if (ret == 0)
        txn.add_op();
    else if (ret == WT_ROLLBACK)
        txn.set_needs_rollback(true);
    else
        testutil_die(ret, "unhandled error while trying to save an insert to the tracking table");
    return (ret == 0);
}

bool
thread_worker::remove(scoped_cursor &cursor, uint64_t collection_id, const std::string &key)
{
    WT_DECL_RET;
    testutil_assert(op_tracker != nullptr);
    testutil_assert(cursor.get() != nullptr);

    wt_timestamp_t ts = tsm->get_next_ts();
    ret = txn.set_commit_timestamp(ts);
    testutil_assert(ret == 0 || ret == EINVAL);
    if (ret != 0) {
        txn.set_needs_rollback(true);
        return (false);
    }

    cursor->set_key(cursor.get(), key.c_str());
    ret = cursor->remove(cursor.get());
    if (ret != 0) {
        if (ret == WT_ROLLBACK) {
            txn.set_needs_rollback(true);
            return (false);
        } else
            testutil_die(ret, "unhandled error while trying to remove a key");
    }

    ret = op_tracker->save_operation(
      session.get(), tracking_operation::DELETE_KEY, collection_id, key, "", ts, op_track_cursor);

    if (ret == 0)
        txn.add_op();
    else if (ret == WT_ROLLBACK)
        txn.set_needs_rollback(true);
    else
        testutil_die(ret, "unhandled error while trying to save a remove to the tracking table");
    return (ret == 0);
}

/*
 * Truncate takes in the collection_id to perform truncate on, two optional keys corresponding to
 * the desired start and stop range, and a configuration string. If a start/stop key exists, we open
 * a cursor and position on that key, otherwise we pass in a null cursor to the truncate API to
 * indicate we should truncate all the way to the first and/or last key.
 */
bool
thread_worker::truncate(uint64_t collection_id, std::optional<std::string> start_key,
  std::optional<std::string> stop_key, const std::string &config)
{
    WT_DECL_RET;

    wt_timestamp_t ts = tsm->get_next_ts();
    ret = txn.set_commit_timestamp(ts);
    testutil_assert(ret == 0 || ret == EINVAL);
    if (ret != 0) {
        txn.set_needs_rollback(true);
        return (false);
    }

    const std::string coll_name = db.get_collection(collection_id).name;

    scoped_cursor start_cursor = session.open_scoped_cursor(coll_name);
    scoped_cursor stop_cursor = session.open_scoped_cursor(coll_name);
    if (start_key)
        start_cursor->set_key(start_cursor.get(), start_key.value().c_str());

    if (stop_key)
        stop_cursor->set_key(stop_cursor.get(), stop_key.value().c_str());

    ret = session->truncate(session.get(), (start_key || stop_key) ? nullptr : coll_name.c_str(),
      start_key ? start_cursor.get() : nullptr, stop_key ? stop_cursor.get() : nullptr,
      config.empty() ? nullptr : config.c_str());

    if (ret != 0) {
        if (ret == WT_ROLLBACK) {
            txn.set_needs_rollback(true);
            return (false);
        } else
            testutil_die(ret, "unhandled error while trying to truncate a key range");
    }

    return (ret == 0);
}

void
thread_worker::sleep()
{
    std::this_thread::sleep_for(std::chrono::milliseconds(_sleep_time_ms));
}

void
thread_worker::sync()
{
    _barrier->wait();
}

bool
thread_worker::running() const
{
    return (_running);
}
} // namespace test_harness
