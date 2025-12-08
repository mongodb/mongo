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
#include "src/storage/scoped_session.h"
extern "C" {
#include "test_util.h"
}

namespace test_harness {
bool
transaction::active() const
{
    return (_in_txn);
}

void
transaction::begin(scoped_session &session, const std::string &config)
{
    testutil_assert(!_in_txn);
    testutil_check(
      session->begin_transaction(session.get(), config.empty() ? nullptr : config.c_str()));
    _in_txn = true;
    _needs_rollback = false;
}

void
transaction::try_begin(scoped_session &session, const std::string &config)
{
    if (!_in_txn)
        begin(session, config);
}

/*
 * It's possible to receive rollback in commit, when this happens the API will rollback the
 * transaction internally.
 */
bool
transaction::commit(scoped_session &session, const std::string &config)
{
    int ret = 0;
    testutil_assert(_in_txn && !_needs_rollback);

    ret = session->commit_transaction(session.get(), config.empty() ? nullptr : config.c_str());
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
    _in_txn = false;
    return (ret == 0);
}

void
transaction::rollback(scoped_session &session, const std::string &config)
{
    testutil_assert(_in_txn);
    testutil_check(
      session->rollback_transaction(session.get(), config.empty() ? nullptr : config.c_str()));
    _needs_rollback = false;
    _in_txn = false;
}

void
transaction::try_rollback(scoped_session &session, const std::string &config)
{
    if (_in_txn)
        rollback(session, config);
}

void
transaction::set_needs_rollback()
{
    _needs_rollback = true;
}

bool
transaction::needs_rollback()
{
    return _needs_rollback;
}
} // namespace test_harness
