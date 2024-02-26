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

#include <deque>
#include <memory>
#include "model/driver/kv_workload.h"
#include "model/core.h"
#include "model/data_value.h"

namespace model {

/*
 * kv_workload_sequence_type --
 *     A type of the sequence.
 */
enum class kv_workload_sequence_type {
    none,
    set_stable_timestamp,
    transaction,
};

/*
 * kv_workload_sequence --
 *     A sequence of operations in a workload.
 */
class kv_workload_sequence {

public:
    /*
     * kv_workload_sequence::kv_workload_sequence --
     *     Create a new sequence of operations.
     */
    inline kv_workload_sequence(
      size_t seq_no, kv_workload_sequence_type type = kv_workload_sequence_type::none)
        : _seq_no(seq_no), _type(type)
    {
    }

    /*
     * kv_workload_sequence::seq_no --
     *     Get the sequence's position in the equivalent serial schedule.
     */
    inline size_t
    seq_no() const noexcept
    {
        return _seq_no;
    }

    /*
     * kv_workload_sequence::type --
     *     Get the type of the sequence, if any.
     */
    inline kv_workload_sequence_type
    type() const noexcept
    {
        return _type;
    }

    /*
     * kv_workload_sequence::operator<< --
     *     Add an operation to the sequence.
     */
    inline kv_workload_sequence &
    operator<<(operation::any &&op)
    {
        _operations.push_back(std::move(op));
        return *this;
    }

    /*
     * kv_workload_sequence::size --
     *     Get the length of the sequence.
     */
    inline size_t
    size() const noexcept
    {
        return _operations.size();
    }

    /*
     * kv_workload_sequence::operator[] --
     *     Get an operation in the sequence.
     */
    inline operation::any &
    operator[](size_t index)
    {
        return _operations[index];
    }

    /*
     * kv_workload_sequence::operator[] --
     *     Get an operation in the sequence.
     */
    inline const operation::any &
    operator[](size_t index) const
    {
        return _operations[index];
    }

    /*
     * kv_workload_sequence::operations --
     *     Get the list of operations. Note that the lifetime of this reference is constrained to
     *     the lifetime of this object.
     */
    inline std::deque<operation::any> &
    operations() noexcept
    {
        return _operations;
    }

    /*
     * kv_workload_sequence::operations --
     *     Get the list of operations. Note that the lifetime of this reference is constrained to
     *     the lifetime of this object.
     */
    inline const std::deque<operation::any> &
    operations() const noexcept
    {
        return _operations;
    }

    /*
     * kv_workload_sequence::overlaps_with --
     *     Check whether this sequence overlaps in any key ranges with the other sequence.
     */
    bool overlaps_with(const kv_workload_sequence &other) const;

    /*
     * kv_workload_sequence::overlaps_with --
     *     Check whether this sequence overlaps in any key ranges with the other sequence.
     */
    inline bool
    overlaps_with(std::shared_ptr<kv_workload_sequence> other) const
    {
        return overlaps_with(*other.get());
    }

    /*
     * kv_workload_sequence::dependencies --
     *     Get the list of sequences that must run before this sequence can run. Note that the
     *     lifetime of this reference is constrained to the lifetime of this object.
     */
    inline const std::deque<kv_workload_sequence *> &
    dependencies() const noexcept
    {
        return _dependencies;
    }

    /*
     * kv_workload_sequence::runnable_after_finish --
     *     Get the list of sequences that are unblocked after this sequence completes. Note that the
     *     lifetime of this reference is constrained to the lifetime of this object.
     */
    inline const std::deque<kv_workload_sequence *> &
    unblocks() const noexcept
    {
        return _unblocks;
    }

    /*
     * kv_workload_sequence::must_finish_before --
     *     Declare that the other sequence cannot start until this sequence finishes.
     */
    void must_finish_before(kv_workload_sequence *other);

protected:
    /*
     * kv_workload_sequence::contains_key --
     *     Check whether the sequence contains an operation that touches any key in the range.
     */
    bool contains_key(table_id_t table_id, const data_value &start, const data_value &stop) const;

protected:
    size_t _seq_no;
    kv_workload_sequence_type _type;

    std::deque<operation::any> _operations;

    /* Sequences that must finish before this sequence can start. */
    std::deque<kv_workload_sequence *> _dependencies;

    /* Sequences that may be unblocked after this sequence finishes. */
    std::deque<kv_workload_sequence *> _unblocks;
};

/*
 * kv_workload_sequence_ptr --
 *     Pointer to a sequence.
 */
using kv_workload_sequence_ptr = std::shared_ptr<kv_workload_sequence>;

} /* namespace model */
