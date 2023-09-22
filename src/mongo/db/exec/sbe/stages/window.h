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
 * consecutive groups. For each document the stage takes a fixed number of slots in `currSlots`,
 * where the first `partitionSlotCount` slots are used to separate partitions.
 *
 * The list of sliding window aggregator definitions are in the 'windows' vector. Each aggregator
 * tracks a set of accumulators that are used to compute its final value. To keep the state of the
 * accumulators, the aggregator has a vector of slots 'windowExprSlots'. The state of each
 * accumulator is updated using an expression triplet {'initExpr', 'addExpr', 'removeExpr'}, all
 * expressions are optional.
 *
 * The sliding window bound is determined by two boolean valued expressions 'lowBoundExpr' and
 * 'highBoundExpr', where both can be optional to indicated unbounded window. If any document is
 * evaluated to true for the current document, then that document is included in the window frame
 * of the current document. This test is performed against a different document, whose slots are
 * saved in `boundTestingSlots`, which is of the same number as `currSlots`.
 *
 * In addition, the caller may provide a list of `frameFirstSlots` and `frameLastSlots` for each
 * window, each slot vector of the same size as the `currSlots`, to represent slots for the first
 * and last documents in the current window frame. Empty slot vector means the value is not
 * required.
 *
 * Debug string representation:
 *
 *  window  [<current slots>]
 *          [frameFirst[<frame first slots>], frameLast[<frame last slots>],
 *              lowBound{<expr>}, highBound{<expr>}]
 *          [<window slot 1> = {init{<expr>}, add{<expr>}, remove{<expr>}},
 *          ...]
 *  childStage
 */
class WindowStage final : public PlanStage {
public:
    struct Window {
        value::SlotVector windowExprSlots;
        value::SlotVector frameFirstSlots;
        value::SlotVector frameLastSlots;
        std::vector<std::unique_ptr<EExpression>> initExprs;
        std::vector<std::unique_ptr<EExpression>> addExprs;
        std::vector<std::unique_ptr<EExpression>> removeExprs;
        std::unique_ptr<EExpression> lowBoundExpr;
        std::unique_ptr<EExpression> highBoundExpr;
    };

    WindowStage(std::unique_ptr<PlanStage> input,
                value::SlotVector currSlots,
                value::SlotVector boundTestingSlots,
                size_t partitionSlotCount,
                std::vector<Window> windows,
                boost::optional<value::SlotId> collatorSlot,
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
    void setCurrAccessors(size_t id);
    void setBoundTestingAccessors(size_t id);
    void setFrameFirstAccessors(size_t windowIdx, size_t firstId);
    void clearFrameFirstAccessors(size_t windowIdx);
    void setFrameLastAccessors(size_t windowIdx, size_t lastId);
    void clearFrameLastAccessors(size_t windowIdx);
    void resetPartition(int start);

    const value::SlotVector _currSlots;
    const value::SlotVector _boundTestingSlots;
    const size_t _partitionSlotCount;
    const std::vector<Window> _windows;

    const boost::optional<value::SlotId> _collatorSlot;

    using BufferedRowAccessor = value::MaterializedRowAccessor<std::deque<value::MaterializedRow>>;
    // The in/out accessors for the current document slots, and the index pointing to that
    // document in the window buffer.
    std::vector<value::SlotAccessor*> _inCurrAccessors;
    std::vector<std::unique_ptr<BufferedRowAccessor>> _outCurrAccessors;
    size_t _currRowIdx;
    // The accessors for document slots under bound testing, and the index pointing to that
    // document in the window buffer.
    std::vector<std::unique_ptr<BufferedRowAccessor>> _boundTestingAccessors;
    size_t _boundTestingRowIdx;
    // The accessors for the document slots of the first document in range, and the index pointing
    // to that document in the window buffer. The accessors are switched with _emptyAccessor to
    // allow empty window frame.
    std::vector<std::vector<std::unique_ptr<value::SwitchAccessor>>> _outFrameFirstAccessors;
    std::vector<std::vector<std::unique_ptr<BufferedRowAccessor>>> _outFrameFirstRowAccessors;
    std::vector<size_t> _frameFirstRowIdxes;
    // The accessors for the document slots of the last document in range, and the index pointing
    // to that document in the window buffer. The accessors are switched with _emptyAccessor to
    // allow empty window frame.
    std::vector<std::vector<std::unique_ptr<value::SwitchAccessor>>> _outFrameLastAccessors;
    std::vector<std::vector<std::unique_ptr<BufferedRowAccessor>>> _outFrameLastRowAccessors;
    std::vector<size_t> _frameLastRowIdxes;
    // An always empty accessor holding Nothing.
    std::unique_ptr<value::OwnedValueAccessor> _emptyAccessor;
    // The out accessors for the window states.
    std::vector<std::vector<std::unique_ptr<value::OwnedValueAccessor>>> _outWindowAccessors;

    value::SlotMap<value::SlotAccessor*> _boundTestingAccessorMap;
    value::SlotMap<value::SlotAccessor*> _outAccessorMap;

    // Accessor for collator. Only set if collatorSlot provided during construction.
    value::SlotAccessor* _collatorAccessor = nullptr;
    CollatorInterface* _collatorView = nullptr;

    bool _compiled{false};
    vm::ByteCode _bytecode;
    std::vector<std::unique_ptr<vm::CodeFragment>> _windowLowBoundCodes;
    std::vector<std::unique_ptr<vm::CodeFragment>> _windowHighBoundCodes;
    std::vector<std::vector<std::unique_ptr<vm::CodeFragment>>> _windowInitCodes;
    std::vector<std::vector<std::unique_ptr<vm::CodeFragment>>> _windowAddCodes;
    std::vector<std::vector<std::unique_ptr<vm::CodeFragment>>> _windowRemoveCodes;

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
    // The id of the start of the current partition.
    size_t _currPartitionId{1};
    // The id of the next partition that we have fetched into the buffer.
    boost::optional<size_t> _nextPartitionId{boost::none};
    // Whether the child stage has reached EOF.
    bool _isEOF{false};

    WindowStats _specificStats;
};

}  // namespace mongo::sbe
