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
#include "crud.h"
#include "transaction.h"

namespace test_harness {

const std::string
type_string(thread_type type)
{
    switch (type) {
    case thread_type::BACKGROUND_COMPACT:
        return ("background_compact");
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
      free_space_target_mb(config->get_optional_int(FREE_SPACE_TARGET_MB, 1)),
      key_count(config->get_optional_int(KEY_COUNT_PER_COLLECTION, 1)),
      key_size(config->get_optional_int(KEY_SIZE, 1)),
      value_size(config->get_optional_int(VALUE_SIZE, 1)),
      thread_count(config->get_int(THREAD_COUNT)), type(type), id(id), db(dbase),
      session(std::move(created_session)), tsm(timestamp_manager), op_tracker(op_tracker),
      _sleep_time_ms(std::chrono::milliseconds(config->get_throttle_ms())), _barrier(barrier_ptr)
{
    if (op_tracker->enabled())
        op_track_cursor = session.open_scoped_cursor(op_tracker->get_operation_table_name());

    /* Use optional here as our populate threads don't define this configuration. */
    configuration *ops_config = config->get_optional_subconfig(OPS_PER_TRANSACTION);
    if (ops_config != nullptr) {
        _min_op_count = ops_config->get_optional_int(MIN, 1);
        _max_op_count = ops_config->get_optional_int(MAX, 1);
        delete ops_config;
    }
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
    int ret = 0;

    testutil_assert(op_tracker != nullptr);
    testutil_assert(cursor.get() != nullptr);

    wt_timestamp_t ts = tsm->get_next_ts();
    ret = set_commit_timestamp(ts);
    testutil_assert(ret == 0 || ret == EINVAL);
    if (ret != 0) {
        _txn.set_needs_rollback();
        return (false);
    }

    if (crud::update(cursor, _txn, key, value) == false)
        return (false);

    ret = op_tracker->save_operation(
      session.get(), tracking_operation::INSERT, collection_id, key, value, ts, op_track_cursor);

    if (ret == 0)
        add_op();
    else if (ret == WT_ROLLBACK)
        _txn.set_needs_rollback();
    else
        testutil_die(ret, "unhandled error while trying to save an update to the tracking table");
    return (ret == 0);
}

bool
thread_worker::insert(
  scoped_cursor &cursor, uint64_t collection_id, const std::string &key, const std::string &value)
{
    int ret = 0;

    testutil_assert(op_tracker != nullptr);
    testutil_assert(cursor.get() != nullptr);

    wt_timestamp_t ts = tsm->get_next_ts();
    ret = set_commit_timestamp(ts);
    testutil_assert(ret == 0 || ret == EINVAL);
    if (ret != 0) {
        _txn.set_needs_rollback();
        return (false);
    }

    if (crud::insert(cursor, _txn, key, value) == false)
        return (false);

    ret = op_tracker->save_operation(
      session.get(), tracking_operation::INSERT, collection_id, key, value, ts, op_track_cursor);

    if (ret == 0)
        add_op();
    else if (ret == WT_ROLLBACK)
        _txn.set_needs_rollback();
    else
        testutil_die(ret, "unhandled error while trying to save an insert to the tracking table");
    return (ret == 0);
}

bool
thread_worker::remove(scoped_cursor &cursor, uint64_t collection_id, const std::string &key)
{
    int ret = 0;
    testutil_assert(op_tracker != nullptr);
    testutil_assert(cursor.get() != nullptr);

    wt_timestamp_t ts = tsm->get_next_ts();
    ret = set_commit_timestamp(ts);
    testutil_assert(ret == 0 || ret == EINVAL);
    if (ret != 0) {
        _txn.set_needs_rollback();
        return (false);
    }

    if (crud::remove(cursor, _txn, key) == false)
        return (false);

    ret = op_tracker->save_operation(
      session.get(), tracking_operation::DELETE_KEY, collection_id, key, "", ts, op_track_cursor);

    if (ret == 0)
        add_op();
    else if (ret == WT_ROLLBACK)
        _txn.set_needs_rollback();
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
    int ret = 0;

    wt_timestamp_t ts = tsm->get_next_ts();
    ret = set_commit_timestamp(ts);
    testutil_assert(ret == 0 || ret == EINVAL);
    if (ret != 0) {
        _txn.set_needs_rollback();
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
            _txn.set_needs_rollback();
            return (false);
        } else
            testutil_die(ret, "unhandled error while trying to truncate a key range");
    }

    return (ret == 0);
}

void
thread_worker::sleep()
{
    std::this_thread::sleep_for(_sleep_time_ms);
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

/*
 * E.g. If we have 4 threads with ids from 0 to 3, and 14 collections with ids from 0 to 13,
 * the distribution of collections to each thread will be as below:
 *
 * Collection count: 14         | Thread count: 4               |
 * -----------------------------|-------------------------------|
 * Thread_id                    |   0   |   1   |   2   |   3   |
 * -----------------------------|-------|-------|-------|-------|
 * Assigned collection count    |   4   |   4   |   3   |   3   |
 * -----------------------------|-------|-------|-------|-------|
 * Assigned first collection_id |   0   |   4   |   8   |   11  |
 * -----------------------------|-------|-------|-------|-------|
 * Assigned collection_id range | [0,3] | [4,7] |[8,10] |[11,13]|
 *
 */
uint64_t
thread_worker::get_assigned_first_collection_id() const
{
    uint64_t collection_count = db.get_collection_count();
    return collection_count / thread_count * id + std::min(id, collection_count % thread_count);
}

/*
 * Assign collections evenly among threads, for any remainders, distribute one collection to each
 * thread starting from thread 0. See a detailed example in the header of
 * get_assigned_first_collection_id().
 */
uint64_t
thread_worker::get_assigned_collection_count() const
{
    uint64_t collection_count = db.get_collection_count();
    return collection_count / thread_count + (collection_count % thread_count > id);
}

/*
 * Returns true if a transaction can be committed as determined by the op count and the state of the
 * transaction.
 */
bool
thread_worker::can_commit()
{
    return (!_txn.needs_rollback() && _txn.active() && get_op_count() >= get_target_op_count());
};

/* Get the current number of operations executed. */
int64_t
thread_worker::get_op_count() const
{
    return _op_count;
}

/* Get the number of operations this transaction needs before it can commit */
int64_t
thread_worker::get_target_op_count() const
{
    return _target_op_count;
}

bool
thread_worker::active() const
{
    return _txn.active();
}
void
thread_worker::add_op()
{
    _op_count++;
}

void
thread_worker::begin(const std::string &config)
{
    /* This randomizes the number of operations to be executed in one transaction. */
    _target_op_count = WT_MAX(
      1, random_generator::instance().generate_integer<int64_t>(_min_op_count, _max_op_count));
    _op_count = 0;
    _txn.begin(session, config);
}

/* Begin a transaction if we are not currently in one. */
void
thread_worker::try_begin(const std::string &config)
{
    if (!active())
        begin(config);
}

/*
 * Commit a transaction and return true if the commit was successful.
 */
bool
thread_worker::commit(const std::string &config)
{
    _op_count = 0;
    return _txn.commit(session, config);
}

/* Rollback a transaction, failure will abort the test. */
void
thread_worker::rollback(const std::string &config)
{
    _op_count = 0;
    _txn.rollback(session, config);
}

/* Attempt to rollback the transaction given the requirements are met. */
void
thread_worker::try_rollback(const std::string &config)
{
    if (active())
        rollback(config);
}

/*
 * FIXME: WT-9198 We're concurrently doing a transaction that contains a bunch of operations while
 * moving the stable timestamp. Eat the occasional EINVAL from the transaction's first commit
 * timestamp being earlier than the stable timestamp.
 */
int
thread_worker::set_commit_timestamp(wt_timestamp_t ts)
{
    if (!tsm->enabled())
        return (0);
    const std::string config = COMMIT_TS + "=" + timestamp_manager::decimal_to_hex(ts);
    return session->timestamp_transaction(session.get(), config.c_str());
}

} // namespace test_harness
