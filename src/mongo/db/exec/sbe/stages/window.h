/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/query/query_knobs_gen.h"

namespace mongo::sbe {

/**
 * Performs a partitioned sliding window aggregation. The input is assumed to be partitioned into
 * consecutive groups, where the values to separate partitions are saved in the 'partitionSlots'.
 * The stage can also forward a list of slots saved in 'forwardSlots'.
 *
 * The list of sliding window aggregator definitions are in the 'windows' vector. Each aggregator
 * has a slot 'windowSlot' to keep the accumulator state, a expression triplet 'initExpr', 'addExpr'
 * and 'removeExpr' to update the accumulator state, where 'initExpr' and 'removeExpr' are optional.
 *
 * The sliding window bound is determined by two boolean valued expressions 'lowBoundExpr' and
 * 'highBoundExpr', where both can be optional to indicated unbounded window. If any document is
 * evaluated to true for the current document, then that document is included in the window frame
 * of the current document. The 'lowBoundSlot', 'highBoundSlot' are used to save the current
 * document value for the lower and higher bound checking, while the 'boundTestingSlot' is for a
 * different document. The 'lowBoundSlot' and 'highBoundSlot' may be the same for different windows.
 *
 * Debug string representation:
 *
 *  window  [<partition slots>] [<forward slots>] [<window slot 1> = lowBound{<expr>},
 *                                                                   highBound{<expr>},
 *                                                                   init{<expr>},
 *                                                                   add{<expr>},
 *                                                                   remove{<expr>},
 *                                                ...]
 *  childStage
 */
class WindowStage final : public PlanStage {
public:
    struct Window {
        value::SlotId windowSlot;
        std::unique_ptr<EExpression> initExpr;
        std::unique_ptr<EExpression> addExpr;
        std::unique_ptr<EExpression> removeExpr;
        boost::optional<value::SlotId> lowBoundSlot;
        boost::optional<value::SlotId> lowBoundTestingSlot;
        std::unique_ptr<EExpression> lowBoundExpr;
        boost::optional<value::SlotId> highBoundSlot;
        boost::optional<value::SlotId> highBoundTestingSlot;
        std::unique_ptr<EExpression> highBoundExpr;
    };

    WindowStage(std::unique_ptr<PlanStage> input,
                value::SlotVector partitionSlots,
                value::SlotVector forwardSlots,
                std::vector<Window> windows,
                PlanNodeId nodeId,
                bool participateInTrialRunTracking = true);

    std::unique_ptr<PlanStage> clone() const final;

    void prepare(CompileCtx& ctx) final;
    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final;
    void open(bool reOpen) final;
    PlanState getNext() final;
    void close() final;

    std::vector<DebugPrinter::Block> debugPrint() const final;
    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const final;
    const SpecificStats* getSpecificStats() const final;
    size_t estimateCompileTimeSize() const final;

private:
    inline size_t getLastRowId();
    bool fetchNextRow();
    void freeUntilRow(size_t id);
    void freeRows();
    void setOutAccessors(size_t id);
    void setBoundTestingAccessor(size_t id);
    void resetWindowRange(int start);

    const value::SlotVector _partitionSlots;
    const value::SlotVector _forwardSlots;
    const std::vector<Window> _windows;
    // List of bound slots for different windows after deduplication.
    value::SlotVector _boundSlots;
    // The index of bound slot for each window within the above vector.
    std::vector<boost::optional<size_t>> _lowBoundSlotIndex;
    std::vector<boost::optional<size_t>> _highBoundSlotIndex;

    using BufferedRowAccessor = value::MaterializedRowAccessor<std::deque<value::MaterializedRow>>;
    std::vector<value::SlotAccessor*> _inPartitionAccessors;
    std::vector<value::SlotAccessor*> _inForwardAccessors;
    std::vector<value::SlotAccessor*> _inBoundAccessors;
    std::vector<std::unique_ptr<BufferedRowAccessor>> _outPartitionAccessors;
    std::vector<std::unique_ptr<BufferedRowAccessor>> _outForwardAccessors;
    std::vector<std::unique_ptr<BufferedRowAccessor>> _outBoundAccessors;
    size_t _outRowIdx;
    std::vector<std::unique_ptr<BufferedRowAccessor>> _lowBoundTestingAccessors;
    std::vector<std::unique_ptr<BufferedRowAccessor>> _highBoundTestingAccessors;
    size_t _boundTestingRowIdx;
    std::vector<std::unique_ptr<value::OwnedValueAccessor>> _outWindowAccessors;
    value::SlotMap<value::SlotAccessor*> _outAccessorMap;
    value::SlotMap<value::SlotAccessor*> _boundTestingAccessorMap;

    bool _compiled{false};
    vm::ByteCode _bytecode;
    std::vector<std::unique_ptr<vm::CodeFragment>> _windowLowBoundCodes;
    std::vector<std::unique_ptr<vm::CodeFragment>> _windowHighBoundCodes;
    std::vector<std::unique_ptr<vm::CodeFragment>> _windowInitCodes;
    std::vector<std::unique_ptr<vm::CodeFragment>> _windowAddCodes;
    std::vector<std::unique_ptr<vm::CodeFragment>> _windowRemoveCodes;

    // The id of the current document, starting from 1.
    // We use 1-based id since we want to use the id for spilling as the key in the
    // temporary record store, and id 0 is reserved
    size_t _currId{0};
    // The id ranges each window function state current holds, inclusive on both ends. Empty ranges
    // are represented as [a+1, a].
    std::vector<std::pair<size_t, size_t>> _windowIdRanges;
    // The buffered slot values.
    std::deque<value::MaterializedRow> _rows;
    // The first id in the buffered rows.
    size_t _firstRowId{1};
    // The id of the next partition that we have fetched into the buffer.
    boost::optional<size_t> _nextPartitionId{boost::none};
    // Whether the child stage has reached EOF.
    bool _isEOF{false};

    WindowStats _specificStats;
};

}  // namespace mongo::sbe
