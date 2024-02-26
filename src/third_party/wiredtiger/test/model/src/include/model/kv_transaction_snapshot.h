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

#pragma once

#include <algorithm>
#include <memory>
#include <unordered_set>
#include <vector>

#include "model/core.h"

namespace model {

class kv_update;

/*
 * kv_transaction_snapshot --
 *     A transaction snapshot.
 */
class kv_transaction_snapshot {

public:
    /*
     * kv_transaction_snapshot::contains --
     *     Check whether the given update belongs to the snapshot.
     */
    virtual bool contains(const kv_update &update) const noexcept = 0;

protected:
    /*
     * kv_transaction_snapshot::kv_transaction_snapshot --
     *     Create a new instance of the snapshot.
     */
    inline kv_transaction_snapshot(){};
};

/*
 * kv_transaction_snapshot_by_exclusion --
 *     A transaction snapshot based on which transactions to exclude.
 */
class kv_transaction_snapshot_by_exclusion : public kv_transaction_snapshot {

public:
    /*
     * kv_transaction_snapshot_by_exclusion::kv_transaction_snapshot_by_exclusion --
     *     Create a new instance of the snapshot based on what to exclude.
     */
    inline kv_transaction_snapshot_by_exclusion(
      txn_id_t exclude_after, std::unordered_set<txn_id_t> &&exclude_ids)
        : _exclude_after(exclude_after), _exclude_ids(exclude_ids)
    {
    }

    /*
     * kv_transaction_snapshot_by_exclusion::contains --
     *     Check whether the given update belongs to the snapshot.
     */
    virtual bool contains(const kv_update &update) const noexcept override;

private:
    txn_id_t _exclude_after;
    std::unordered_set<txn_id_t> _exclude_ids;
};

/*
 * kv_transaction_snapshot_wt --
 *     A transaction snapshot that emulates WiredTiger's behavior.
 */
class kv_transaction_snapshot_wt : public kv_transaction_snapshot {

public:
    /*
     * kv_transaction_snapshot_wt::kv_transaction_snapshot_wt --
     *     Create a new instance of the snapshot.
     */
    inline kv_transaction_snapshot_wt(write_gen_t write_gen, txn_id_t snapshot_min,
      txn_id_t snapshot_max, const std::vector<uint64_t> &snapshots)
        : _min_id(snapshot_min), _max_id(snapshot_max), _write_gen(write_gen)
    {
        std::copy(
          snapshots.begin(), snapshots.end(), std::inserter(_exclude_ids, _exclude_ids.begin()));
    }

    /*
     * kv_transaction_snapshot_wt::contains --
     *     Check whether the given update belongs to the snapshot.
     */
    virtual bool contains(const kv_update &update) const noexcept override;

private:
    txn_id_t _min_id, _max_id;
    write_gen_t _write_gen;

    std::unordered_set<txn_id_t> _exclude_ids;
};

/*
 * kv_transaction_snapshot_ptr --
 *     A shared pointer to the transaction snapshot.
 */
using kv_transaction_snapshot_ptr = std::shared_ptr<kv_transaction_snapshot>;

} /* namespace model */
