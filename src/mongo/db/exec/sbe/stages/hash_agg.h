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

#include <unordered_map>

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/stdx/unordered_map.h"

namespace mongo {
namespace sbe {
/**
 * Performs a hash-based aggregation. Appears as the "group" stage in debug output. Groups the input
 * based on the provided vector of group-by slots, 'gbs'. The 'aggs' parameter is a map from
 * 'SlotId' to expression. This defines a set of output slots whose values will be computed based on
 * the corresponding aggregate expressions. Each distinct grouping will produce a single output,
 * consisting of the values of the group-by keys and the results of the aggregate functions.
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
 * Debug string representation:
 *
 *  group [<group by slots>] [slot_1 = expr_1, ..., slot_n = expr_n] [<seek slots>]? reopen?
 * collatorSlot? childStage
 */
class HashAggStage final : public PlanStage {
public:
    HashAggStage(std::unique_ptr<PlanStage> input,
                 value::SlotVector gbs,
                 value::SlotMap<std::unique_ptr<EExpression>> aggs,
                 value::SlotVector seekKeysSlots,
                 bool optimizedClose,
                 boost::optional<value::SlotId> collatorSlot,
                 PlanNodeId planNodeId);

    std::unique_ptr<PlanStage> clone() const final;

    void prepare(CompileCtx& ctx) final;
    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final;
    void open(bool reOpen) final;
    PlanState getNext() final;
    void close() final;

    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const final;
    const SpecificStats* getSpecificStats() const final;
    std::vector<DebugPrinter::Block> debugPrint() const final;
    size_t estimateCompileTimeSize() const final;

private:
    using TableType = stdx::unordered_map<value::MaterializedRow,
                                          value::MaterializedRow,
                                          value::MaterializedRowHasher,
                                          value::MaterializedRowEq>;

    using HashKeyAccessor = value::MaterializedRowKeyAccessor<TableType::iterator>;
    using HashAggAccessor = value::MaterializedRowValueAccessor<TableType::iterator>;

    const value::SlotVector _gbs;
    const value::SlotMap<std::unique_ptr<EExpression>> _aggs;
    const boost::optional<value::SlotId> _collatorSlot;
    const value::SlotVector _seekKeysSlots;
    // When this operator does not expect to be reopened (almost always) then it can close the child
    // early.
    const bool _optimizedClose{true};
    // Memory tracking variables.
    const long long _approxMemoryUseInBytesBeforeSpill =
        internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.load();
    const int _memoryUseSampleRate = internalQuerySBEAggMemoryUseSampleRate.load();
    // Used in collaboration with memoryUseSampleRatePercentage to determine whether we should
    // re-approximate memory usage.
    PseudoRandom _pseudoRandom = PseudoRandom(Date_t::now().asInt64());

    value::SlotAccessorMap _outAccessors;
    std::vector<value::SlotAccessor*> _inKeyAccessors;
    std::vector<std::unique_ptr<HashKeyAccessor>> _outKeyAccessors;

    std::vector<value::SlotAccessor*> _seekKeysAccessors;
    value::MaterializedRow _seekKeys;

    std::vector<std::unique_ptr<HashAggAccessor>> _outAggAccessors;
    std::vector<std::unique_ptr<vm::CodeFragment>> _aggCodes;

    // Only set if collator slot provided on construction.
    value::SlotAccessor* _collatorAccessor = nullptr;

    boost::optional<TableType> _ht;
    TableType::iterator _htIt;

    vm::ByteCode _bytecode;

    bool _compiled{false};
    bool _childOpened{false};
};
}  // namespace sbe
}  // namespace mongo
