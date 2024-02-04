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

extern "C" {
#include "wt_internal.h"
}

#include "model/driver/kv_workload_sequence.h"
#include "model/util.h"

namespace model {

/*
 * kv_workload_sequence::overlaps_with --
 *     Check whether this sequence overlaps in any key ranges with the other sequence.
 */
bool
kv_workload_sequence::overlaps_with(const kv_workload_sequence &other) const
{
    for (auto &op : _operations) {
        if (std::holds_alternative<operation::insert>(op)) {
            table_id_t t = std::get<operation::insert>(op).table_id;
            const data_value &k = std::get<operation::insert>(op).key;
            if (other.contains_key(t, k, k))
                return true;
        }
        if (std::holds_alternative<operation::remove>(op)) {
            table_id_t t = std::get<operation::remove>(op).table_id;
            const data_value &k = std::get<operation::remove>(op).key;
            if (other.contains_key(t, k, k))
                return true;
        }
        if (std::holds_alternative<operation::truncate>(op)) {
            table_id_t t = std::get<operation::truncate>(op).table_id;
            const data_value &a = std::get<operation::truncate>(op).start;
            const data_value &b = std::get<operation::truncate>(op).stop;
            if (other.contains_key(t, a, b))
                return true;
        }
    }
    return false;
}

/*
 * kv_workload_sequence::contains_key --
 *     Check whether the sequence contains an operation that touches any key in the range.
 */
bool
kv_workload_sequence::contains_key(
  table_id_t table_id, const data_value &start, const data_value &stop) const
{
    for (auto &op : _operations) {
        if (std::holds_alternative<operation::insert>(op)) {
            if (std::get<operation::insert>(op).table_id != table_id)
                continue;
            const data_value &k = std::get<operation::insert>(op).key;
            if (start <= k && k <= stop)
                return true;
        }
        if (std::holds_alternative<operation::remove>(op)) {
            if (std::get<operation::remove>(op).table_id != table_id)
                continue;
            const data_value &k = std::get<operation::remove>(op).key;
            if (start <= k && k <= stop)
                return true;
        }
        if (std::holds_alternative<operation::truncate>(op)) {
            if (std::get<operation::truncate>(op).table_id != table_id)
                continue;
            const data_value &a = std::get<operation::truncate>(op).start;
            const data_value &b = std::get<operation::truncate>(op).stop;
            if (start <= a && a <= stop)
                return true;
            if (start <= b && b <= stop)
                return true;
            if (a <= start && stop <= b)
                return true;
        }
    }
    return false;
}

/*
 * kv_workload_sequence::must_finish_before --
 *     Declare that the other sequence cannot start until this sequence finishes.
 */
void
kv_workload_sequence::must_finish_before(kv_workload_sequence *other)
{
    other->_dependencies.push_back(this);
    _unblocks.push_back(other);
}

} /* namespace model */
