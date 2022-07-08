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

#include "transaction.h"

#include "src/common/constants.h"
#include "src/common/logger.h"
#include "src/common/random_generator.h"

namespace test_harness {

transaction::transaction(
  configuration *config, timestamp_manager *timestamp_manager, WT_SESSION *session)
    : _timestamp_manager(timestamp_manager), _session(session)
{
    /* Use optional here as our populate threads don't define this configuration. */
    configuration *transaction_config = config->get_optional_subconfig(OPS_PER_TRANSACTION);
    if (transaction_config != nullptr) {
        _min_op_count = transaction_config->get_optional_int(MIN, 1);
        _max_op_count = transaction_config->get_optional_int(MAX, 1);
        delete transaction_config;
    }
}

bool
transaction::active() const
{
    return (_in_txn);
}

void
transaction::add_op()
{
    _op_count++;
}

void
transaction::begin(const std::string &config)
{
    testutil_assert(!_in_txn);
    testutil_check(
      _session->begin_transaction(_session, config.empty() ? nullptr : config.c_str()));
    /* This randomizes the number of operations to be executed in one transaction. */
    _target_op_count =
      random_generator::instance().generate_integer<int64_t>(_min_op_count, _max_op_count);
    _op_count = 0;
    _in_txn = true;
    _needs_rollback = false;
}

void
transaction::try_begin(const std::string &config)
{
    if (!_in_txn)
        begin(config);
}

/*
 * It's possible to receive rollback in commit, when this happens the API will rollback the
 * transaction internally.
 */
bool
transaction::commit(const std::string &config)
{
    WT_DECL_RET;
    testutil_assert(_in_txn && !_needs_rollback);

    ret = _session->commit_transaction(_session, config.empty() ? nullptr : config.c_str());
    /*
     * FIXME-WT-9198 Now we are accepting the error code EINVAL because of possible invalid
     * timestamps as we know it can happen due to the nature of the framework. The framework may set
     * the stable/oldest timestamps to a more recent date than the commit timestamp of the
     * transaction which makes the transaction invalid. We only need to check against the stable
     * timestamp as, by definition, the oldest timestamp is older than the stable one.
     */
    testutil_assert(ret == 0 || ret == EINVAL || ret == WT_ROLLBACK);

    if (ret != 0)
        logger::log_msg(LOG_WARN,
          "Failed to commit transaction in commit, received error code: " + std::to_string(ret));
    _op_count = 0;
    _in_txn = false;
    return (ret == 0);
}

void
transaction::rollback(const std::string &config)
{
    testutil_assert(_in_txn);
    testutil_check(
      _session->rollback_transaction(_session, config.empty() ? nullptr : config.c_str()));
    _needs_rollback = false;
    _op_count = 0;
    _in_txn = false;
}

void
transaction::try_rollback(const std::string &config)
{
    if (can_rollback())
        rollback(config);
}

int64_t
transaction::get_target_op_count() const
{
    return _target_op_count;
}

/*
 * FIXME: WT-9198 We're concurrently doing a transaction that contains a bunch of operations while
 * moving the stable timestamp. Eat the occasional EINVAL from the transaction's first commit
 * timestamp being earlier than the stable timestamp.
 */
int
transaction::set_commit_timestamp(wt_timestamp_t ts)
{
    /* We don't want to set zero timestamps on transactions if we're not using timestamps. */
    if (!_timestamp_manager->enabled())
        return 0;
    const std::string config = COMMIT_TS + "=" + timestamp_manager::decimal_to_hex(ts);
    return _session->timestamp_transaction(_session, config.c_str());
}

void
transaction::set_needs_rollback(bool rollback)
{
    _needs_rollback = rollback;
}

bool
transaction::can_commit()
{
    return (!_needs_rollback && can_rollback());
}

bool
transaction::can_rollback()
{
    return (_in_txn && _op_count >= _target_op_count);
}
} // namespace test_harness
