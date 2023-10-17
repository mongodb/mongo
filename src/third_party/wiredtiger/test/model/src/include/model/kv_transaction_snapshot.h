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

#ifndef MODEL_KV_TRANSACTION_SNAPSHOT_H
#define MODEL_KV_TRANSACTION_SNAPSHOT_H

#include <unordered_set>

#include "model/core.h"

namespace model {

/*
 * kv_transaction_snapshot --
 *     A transaction snapshot.
 */
class kv_transaction_snapshot {

public:
    /*
     * kv_transaction_snapshot::kv_transaction_snapshot --
     *     Create a new instance of the snapshot based on what to exclude.
     */
    inline kv_transaction_snapshot(
      txn_id_t exclude_after, std::unordered_set<txn_id_t> &&exclude_ids)
        : _exclude_after(exclude_after), _exclude_ids(exclude_ids)
    {
    }

    /*
     * kv_transaction_snapshot::contains --
     *     Check whether the given transaction ID belongs to the snapshot.
     */
    inline bool
    contains(txn_id_t id) const noexcept
    {
        if (id > _exclude_after)
            return false;
        return _exclude_ids.find(id) == _exclude_ids.end();
    }

private:
    txn_id_t _exclude_after;
    std::unordered_set<txn_id_t> _exclude_ids;
};

} /* namespace model */
#endif
