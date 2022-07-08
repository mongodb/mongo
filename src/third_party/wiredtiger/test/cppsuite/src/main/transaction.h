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

#ifndef TRANSACTION_H
#define TRANSACTION_H

#include <string>

#include "src/main/configuration.h"
#include "src/component/timestamp_manager.h"

extern "C" {
#include "wiredtiger.h"
}

namespace test_harness {

class transaction {
    public:
    transaction(configuration *config, timestamp_manager *timestamp_manager, WT_SESSION *session);

    bool active() const;
    void add_op();
    void begin(const std::string &config = "");
    /* Begin a transaction if we are not currently in one. */
    void try_begin(const std::string &config = "");
    /*
     * Commit a transaction and return true if the commit was successful.
     */
    bool commit(const std::string &config = "");
    /* Rollback a transaction, failure will abort the test. */
    void rollback(const std::string &config = "");
    /* Attempt to rollback the transaction given the requirements are met. */
    void try_rollback(const std::string &config = "");
    /* Set a commit timestamp. */
    int set_commit_timestamp(wt_timestamp_t ts);
    /* Set that the transaction needs to be rolled back. */
    void set_needs_rollback(bool rollback);
    /*
     * Returns true if a transaction can be committed as determined by the op count and the state of
     * the transaction.
     */
    bool can_commit();
    /*
     * Returns true if a transaction can be rolled back as determined by the op count and the state
     * of the transaction.
     */
    bool can_rollback();
    /* Get the number of operations this transaction needs before it can commit */
    int64_t get_target_op_count() const;

    private:
    bool _in_txn = false;
    bool _needs_rollback = false;

    /*
     * _min_op_count and _max_op_count are the minimum and maximum number of operations within one
     * transaction. is the current maximum number of operations that can be executed in the current
     * transaction.
     */
    int64_t _max_op_count = INT64_MAX;
    int64_t _min_op_count = 0;
    /*
     * op_count is the current number of operations that have been executed in the current
     * transaction.
     */
    int64_t _op_count = 0;
    int64_t _target_op_count = 0;

    timestamp_manager *_timestamp_manager = nullptr;
    WT_SESSION *_session = nullptr;
};

} // namespace test_harness

#endif
