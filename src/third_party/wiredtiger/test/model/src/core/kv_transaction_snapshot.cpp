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

#include "model/kv_transaction_snapshot.h"
#include "model/kv_update.h"

namespace model {

/*
 * kv_transaction_snapshot_by_exclusion::contains --
 *     Check whether the given update belongs to the snapshot.
 */
bool
kv_transaction_snapshot_by_exclusion::contains(const kv_update &update) const noexcept
{
    if (update.txn_id() > _exclude_after)
        return false;
    return _exclude_ids.find(update.txn_id()) == _exclude_ids.end();
}

/*
 * kv_transaction_snapshot_wt::contains --
 *     Check whether the given update belongs to the snapshot.
 */
bool
kv_transaction_snapshot_wt::contains(const kv_update &update) const noexcept
{
    /* Compare the base generation numbers to see if we are in the right restart cycle. */
    write_gen_t update_write_gen = update.wt_base_write_gen();
    if (update_write_gen < _write_gen)
        return true;
    if (update_write_gen > _write_gen)
        return false;

    /* ...because only then the transaction IDs are meaningful. */
    txn_id_t txn_id = update.wt_txn_id();
    if (txn_id >= _max_id) /* Max is exclusive. */
        return false;
    if (txn_id < _min_id)
        return true;
    return _exclude_ids.find(txn_id) == _exclude_ids.end();
}

} /* namespace model */
