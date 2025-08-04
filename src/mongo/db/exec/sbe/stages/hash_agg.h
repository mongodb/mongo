/**
 *    Copyright (C) 2019-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/bson/util/builder.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/hash_agg_accumulator.h"
#include "mongo/db/exec/sbe/stages/hashagg_base.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/util/spilling.h"
#include "mongo/db/exec/sbe/values/row.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/temporary_record_store.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/unordered_map.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/inlined_vector.h>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace sbe {
/**
 * Performs a hash-based aggregation. Appears as the "group" stage in debug output. Groups the input
 * based on the provided vector of group-by slots, 'gbs'. The 'aggs' parameter is a map from
 * 'SlotId' to a pair of expressions, where the first expression is an optional initializer,
 * and the second expression aggregates the incoming rows. This defines a set of output slots whose
 * values will be computed based on the corresponding aggregate expressions. Each distinct grouping
 * will produce a single output, consisting of the values of the group-by keys and the results of
 * the aggregate functions.
 *
 * Since the data must be buffered in a hash table, this is a "binding reflector". This means slots
 * from the 'input' tree are not visible higher in tree. Stages higher in the tree can only see the
 * slots holding the group-by keys as well as those holding the corresponding aggregate values.
 *
 * The optional 'seekKeys', if provided, limit the results returned from the hash table only to
 * those equal to seekKeys.
 *
 * The 'optimizedClose' flag controls whether we can close the child subtree right after building
 * the hash table. If true it means that we do not expect the subtree to be reopened.
 *
 * The optional 'collatorSlot', if provided, changes the definition of string equality used when
 * determining whether two group-by keys are equal. For instance, the plan may require us to do a
 * case-insensitive group on a string field.
 *
 * The 'allowDiskUse' flag controls whether this stage can spill. If false and the memory budget is
 * exhausted, this stage throws a query-fatal error with code
 * 'QueryExceededMemoryLimitNoDiskUseAllowed'. If true, then spilling is possible and the caller
 * must provide a vector of 'mergingExprs'. This is a vector of (slot, expression) pairs which is
 * symmetrical with 'aggs'. The slots are only visible internally and are used to store partial
 * aggregate values that have been recovered from the spill table. Each of the expressions is an agg
 * function which merges the partial aggregate value from this slot into the final aggregate value.
 * In the debug string output, the internal slots used to house the partial aggregates are printed
 * as a list of "spillSlots" and the expressions are printed as a parallel list of "mergingExprs".
 *
 * If 'forcedIncreasedSpilling' is true, then this stage will spill frequently even if the memory
 * limit is not reached. This is intended to be used in test contexts to exercise the otherwise
 * infrequently used spilling logic.
 *
 * Debug string representation:
 *
 *  group [<group by slots>] [slot_1 = expr_1, ..., slot_n = expr_n] [<seek slots>]?
 *      spillSlots[slot_1, ..., slot_n] mergingExprs[expr_1, ..., expr_n] reopen? collatorSlot?
 *  childStage
 */
class HashAggStage final : public HashAggBaseStage<HashAggStage> {
    friend class HashAggBaseStage<HashAggStage>;

public:
    HashAggStage(std::unique_ptr<PlanStage> input,
                 value::SlotVector gbs,
                 std::vector<std::unique_ptr<HashAggAccumulator>> accumulatorList,
                 value::SlotVector seekKeysSlots,
                 bool optimizedClose,
                 boost::optional<value::SlotId> collatorSlot,
                 bool allowDiskUse,
                 PlanYieldPolicy* yieldPolicy,
                 PlanNodeId planNodeId,
                 bool participateInTrialRunTracking = true,
                 bool forceIncreasedSpilling = false);

    std::unique_ptr<PlanStage> clone() const final;

    void prepare(CompileCtx& ctx) final;
    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final;
    void open(bool reOpen) final;
    PlanState getNext() final;
    void close() final;

    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const final;
    const SpecificStats* getSpecificStats() const final;
    HashAggStats* getHashAggStats();
    std::vector<DebugPrinter::Block> debugPrint() const final;
    size_t estimateCompileTimeSize() const final;

protected:
    // After all inputs are processed (which happens in 'open()'), this method iterates over the
    // resulting aggregated groups in memory. Each call updates the '_htIt' iterator, which has the
    // effect of redirecting the '_outRecordStoreKeyAccessors' and '_outRecordStoreAggAccessors' to
    // reference the aggregated from their entry in the '_ht' hash table.
    //
    // Does not read any data that was spilled to disk.
    void setIteratorToNextRecord() {
        if (_htIt == _ht->end()) {
            // First invocation of getNext() after open().
            if (!_seekKeysAccessors.empty()) {
                _htIt = _ht->find(_seekKeys);
            } else {
                _htIt = _ht->begin();
            }
        } else if (!_seekKeysAccessors.empty()) {
            // Subsequent invocation with seek keys. Return only 1 single row (if any).
            _htIt = _ht->end();
        } else {
            ++_htIt;
        }
    }

    // The stage has spilled to disk. Results will be returned from there.
    void switchToDisk() {
        tassert(10300300,
                "_recordStore should have been initialised before switching to reading from disk",
                _recordStore);

        // Establish a cursor, positioned at the beginning of the record store.
        _rsCursor = _recordStore->getCursor(_opCtx);

        // Callers will be obtaining the results from the spill table, so set the
        // 'SwitchAccessors' so that they refer to the rows recovered from the record store
        // under the hood.
        for (auto&& accessor : _outKeyAccessors) {
            accessor->setIndex(1);
        }
        for (auto&& accessor : _outAggAccessors) {
            accessor->setIndex(1);
        }
    }

private:
    /**
     * Given a 'record' from the record store and a 'collator', decodes it into a pair of
     * materialized rows (one for the group-by key and another one for the agg value).
     * Both the group-by key and the agg value are read from the data part of the record.
     */
    HashAggBaseStage::SpilledRow deserializeSpilledRecordWithCollation(
        const Record& record, const CollatorInterface& collator);

    PlanState getNextSpilled();

    const value::SlotVector _gbs;

    /**
     * Each entry of this list has an "out slot," which binds the accumulator state during
     * accumulation and the accumulated result after finalization; a "spill slot," which binds the
     * recovered state of a spilled accumulator when merging partial aggregates from disk; and an
     * executable implementation of the accumulator's initialize, accumulate, merge, and finalize
     * steps.
     */
    std::vector<std::unique_ptr<HashAggAccumulator>> _accumulatorList;

    const boost::optional<value::SlotId> _collatorSlot;
    const value::SlotVector _seekKeysSlots;
    // When this operator does not expect to be reopened (almost always) then it can close the child
    // early.
    const bool _optimizedClose{true};

    value::SlotAccessorMap _outAccessors;

    // Accessors used to obtain the values of the group by slots when reading the input from the
    // child.
    std::vector<value::SlotAccessor*> _inKeyAccessors;

    // This buffer stores values for '_outKeyRowRecordStore'; values in the '_outKeyRowRecordStore'
    // can be pointers that point to data in this buffer.
    BufBuilder _outKeyRowRSBuffer;

    // Each accessor in '_outHashKeyAccessors' references one of the keys for a hash agg result
    // produced by a 'getNext()' call and is bound by this stage to the corresponding slot in the
    // '_gbs' array. These accessors are "switch" accessors that reference different sources
    // depending on whether results were spilled to disk.
    //
    // If no spilling was necessary, the '_outKeyAccessors' are switched to
    // '_outRecordStoreKeyAccesors' that in turn reference the keys from the current entry in the
    // '_ht' hash table accroding to the '_htIt' iterator.
    //
    // If results were spilled to disk, the '_outKeyAccessors' are switched to
    // '_outRecordStoreKeyAccessors' that hold the result of recovering partial aggregates from disk
    // and merging them.
    std::vector<std::unique_ptr<HashKeyAccessor>> _outHashKeyAccessors;
    // Row of key values to output used when recovering spilled data from the record store.
    value::MaterializedRow _outKeyRowRecordStore{0};
    std::vector<std::unique_ptr<value::MaterializedSingleRowAccessor>> _outRecordStoreKeyAccessors;
    std::vector<std::unique_ptr<value::SwitchAccessor>> _outKeyAccessors;


    // Each accessor in 'outHashAggAccessors' references one of the accumulated values for a hash
    // agg result produced by a 'getNext()' call and is bound by this stage to the "out stage" from
    // the corresponding '_accumulatorList' entry. These accessors are "switch" accessors that read
    // either '_ht' table entries or merged-from-disk results using the same mechanism as the above
    // '_outHashKeyAccessors' list.
    std::vector<std::unique_ptr<HashAggAccessor>> _outHashAggAccessors;
    // Row of agg values to output used when recovering spilled data from the record store.
    value::MaterializedRow _outAggRowRecordStore{0};
    std::vector<std::unique_ptr<value::MaterializedSingleRowAccessor>> _outRecordStoreAggAccessors;
    std::vector<std::unique_ptr<value::SwitchAccessor>> _outAggAccessors;

    std::vector<value::SlotAccessor*> _seekKeysAccessors;
    value::MaterializedRow _seekKeys;

    // Function object which can be used to check whether two materialized rows of key values are
    // equal. This comparison is collation-aware if the query has a non-simple collation.
    value::MaterializedRowEq _keyEq;

    vm::ByteCode _bytecode;

    bool _compiled{false};
    bool _childOpened{false};

    // Partial aggregates that have been spilled are read into '_spilledAggRow' and read using
    // '_spilledAggsAccessors' so that they can be merged to compute the final aggregate value.
    value::MaterializedRow _spilledAggRow{0};
    std::vector<std::unique_ptr<value::MaterializedSingleRowAccessor>> _spilledAggsAccessors;
    value::SlotAccessorMap _spilledAggsAccessorMap;

    // Buffer to hold data for the deserialized key values from '_stashedNextRow'.
    BufBuilder _stashedKeyBuffer;
    // Place to stash the next keys and values during the streaming phase. The record store cursor
    // doesn't offer a "peek" API, so we need to hold onto the next row between getNext() calls when
    // the key value advances.
    SpilledRow _stashedNextRow;

    HashAggStats _specificStats;
};  // class HashAggStage
}  // namespace sbe
}  // namespace mongo
