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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <boost/optional/optional.hpp>

#include "mongo/bson/util/builder.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/hashagg_base.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/util/spilling.h"
#include "mongo/db/exec/sbe/values/row.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/exec/trial_run_tracker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/stage_types.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/temporary_record_store.h"
#include "mongo/platform/atomic_proxy.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/unordered_map.h"

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
public:
    HashAggStage(std::unique_ptr<PlanStage> input,
                 value::SlotVector gbs,
                 AggExprVector aggs,
                 value::SlotVector seekKeysSlots,
                 bool optimizedClose,
                 boost::optional<value::SlotId> collatorSlot,
                 bool allowDiskUse,
                 SlotExprPairVector mergingExprs,
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
    const AggExprVector _aggs;
    const boost::optional<value::SlotId> _collatorSlot;
    const value::SlotVector _seekKeysSlots;
    // When this operator does not expect to be reopened (almost always) then it can close the child
    // early.
    const bool _optimizedClose{true};

    // Expressions used to merge partial aggregates that have been spilled to disk and their
    // corresponding input slots. For example, imagine that this list contains a pair (s12,
    // sum(s12)). This means that the partial aggregate values will be read into slot s12 after
    // being recovered from the spill table and can be merged using the 'sum()' agg function.
    //
    // When disk use is allowed, this vector must have the same length as '_aggs'.
    const SlotExprPairVector _mergingExprs;

    value::SlotAccessorMap _outAccessors;

    // Accessors used to obtain the values of the group by slots when reading the input from the
    // child.
    std::vector<value::SlotAccessor*> _inKeyAccessors;

    // This buffer stores values for '_outKeyRowRecordStore'; values in the '_outKeyRowRecordStore'
    // can be pointers that point to data in this buffer.
    BufBuilder _outKeyRowRSBuffer;
    // Accessors for the key slots provided as output by this stage. The keys can either come from
    // the hash table or recovered from a temporary record store. We use a 'SwitchAccessor' to
    // switch between these two cases.
    std::vector<std::unique_ptr<HashKeyAccessor>> _outHashKeyAccessors;
    // Row of key values to output used when recovering spilled data from the record store.
    value::MaterializedRow _outKeyRowRecordStore{0};
    std::vector<std::unique_ptr<value::MaterializedSingleRowAccessor>> _outRecordStoreKeyAccessors;
    std::vector<std::unique_ptr<value::SwitchAccessor>> _outKeyAccessors;

    // Accessors for the output aggregate results. The aggregates can either come from the hash
    // table or can be computed after merging partial aggregates spilled to a record store. We use a
    // 'SwitchAccessor' to switch between these two cases.
    std::vector<std::unique_ptr<HashAggAccessor>> _outHashAggAccessors;
    // Row of agg values to output used when recovering spilled data from the record store.
    value::MaterializedRow _outAggRowRecordStore{0};
    std::vector<std::unique_ptr<value::MaterializedSingleRowAccessor>> _outRecordStoreAggAccessors;
    std::vector<std::unique_ptr<value::SwitchAccessor>> _outAggAccessors;

    std::vector<value::SlotAccessor*> _seekKeysAccessors;
    value::MaterializedRow _seekKeys;

    // Bytecodes for the aggregate functions. The first code fragment is the aggregator initializer.
    // The second code fragment aggregates incoming rows into the hash table.
    std::vector<std::pair<std::unique_ptr<vm::CodeFragment>, std::unique_ptr<vm::CodeFragment>>>
        _aggCodes;
    // Bytecode for the merging expressions, executed if partial aggregates are spilled to a record
    // store and need to be subsequently combined.
    std::vector<std::unique_ptr<vm::CodeFragment>> _mergingExprCodes;

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
};
}  // namespace sbe
}  // namespace mongo
