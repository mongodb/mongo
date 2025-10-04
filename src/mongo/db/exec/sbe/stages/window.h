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
#include "mongo/db/exec/sbe/util/spilling.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/query/stage_memory_limit_knobs/knobs.h"

#include <bit>

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
        EExpression::Vector initExprs;
        EExpression::Vector addExprs;
        EExpression::Vector removeExprs;
        std::unique_ptr<EExpression> lowBoundExpr;
        std::unique_ptr<EExpression> highBoundExpr;
    };

    WindowStage(std::unique_ptr<PlanStage> input,
                value::SlotVector currSlots,
                value::SlotVector boundTestingSlots,
                size_t partitionSlotCount,
                std::vector<Window> windows,
                boost::optional<value::SlotId> collatorSlot,
                bool allowDiskUse,
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


protected:
    void doSaveState() override;
    void doRestoreState() override;

    void doAttachCollectionAcquisition(const MultipleCollectionAccessor& mca) override {
        return;
    }

private:
    /**
     * Get the last row id either in the buffer or spilled.
     */
    inline size_t getLastRowId();

    /**
     * Get the window frame size of a particular window.
     */
    inline size_t getWindowFrameSize(size_t windowIdx);

    /**
     * Fetch the next row into the window buffer, this call may cause spilling when memory
     * usage exceeds the threshold.
     */
    bool fetchNextRow();

    /**
     * All rows up until the given id (exclusive) will be freed from the window buffer, the spilled
     * rows will not be deleted.
     */
    void freeUntilRow(size_t id);

    /**
     * Free the entire window buffer and the spilled records, reset all relevant states to their
     * initial values.
     */
    void freeRows();

    /**
     * Set the current partition to the row starting at the given id. Updates all window frame
     * ranges and window states.
     */
    void setPartition(int id);

    /**
     * Set the accessors to the row of given id, either when the row is in memory buffer or when the
     * row is spilled.
     */
    void setAccessors(size_t id,
                      const std::vector<std::unique_ptr<value::SwitchAccessor>>& accessors,
                      size_t& bufferedRowIdx,
                      value::MaterializedRow& spilledRow);

    /**
     * Clear the accessors to empty. Use the spilled row to hold the Nothing values.
     */
    void clearAccessors(const std::vector<std::unique_ptr<value::SwitchAccessor>>& accessors,
                        value::MaterializedRow& row);

    /**
     * Set or clear different accessor types.
     */
    void setCurrAccessors(size_t id);
    void setBoundTestingAccessors(size_t id);
    void setFrameFirstAccessors(size_t windowIdx, size_t id);
    void clearFrameFirstAccessors(size_t windowIdx);
    void setFrameLastAccessors(size_t windowIdx, size_t id);
    void clearFrameLastAccessors(size_t windowIdx);

    /**
     * Get the last spilled row id.
     */
    inline size_t getLastSpilledRowId();

    /**
     * Get the estimated total memory size, including both the window buffer and the accumulator
     * states.
     */
    inline size_t getMemoryEstimation();

    /**
     * Read a spilled row of given id into the provided MaterializedRow.
     */
    void readSpilledRow(size_t id, value::MaterializedRow& row);

    /**
     * Spill all the in memory rows inside the window buffer.
     */
    void spill();

    void doForceSpill() final {
        doRestoreState();
        spill();
        doSaveState();
    }

    const value::SlotVector _currSlots;
    const value::SlotVector _boundTestingSlots;
    const size_t _partitionSlotCount;
    const std::vector<Window> _windows;

    const boost::optional<value::SlotId> _collatorSlot;

    using BufferedRowAccessor = value::MaterializedRowAccessor<std::deque<value::MaterializedRow>>;
    using SpilledRowAccessor = value::MaterializedSingleRowAccessor;
    // The in/out accessors for the current document slots. Either pointing to a row in the window
    // buffer by an index, or a recovered spilled row.
    std::vector<value::SlotAccessor*> _inCurrAccessors;
    std::vector<std::unique_ptr<value::SwitchAccessor>> _outCurrAccessors;
    std::vector<std::unique_ptr<BufferedRowAccessor>> _outCurrBufferAccessors;
    size_t _currBufferedRowIdx;
    std::vector<std::unique_ptr<SpilledRowAccessor>> _outCurrSpillAccessors;
    value::MaterializedRow _currSpilledRow;
    // The accessors for document slots under bound testing. Either pointing to a row in the window
    // buffer by an index, or a recovered spilled row.
    std::vector<std::unique_ptr<value::SwitchAccessor>> _boundTestingAccessors;
    std::vector<std::unique_ptr<BufferedRowAccessor>> _boundTestingBufferAccessors;
    size_t _boundTestingBufferedRowIdx;
    std::vector<std::unique_ptr<SpilledRowAccessor>> _boundTestingSpillAccessors;
    value::MaterializedRow _boundTestingSpilledRow;
    // The accessors for the document slots of the first document in window frame. Either pointing
    // to a row in the window buffer by an index, or a recovered spilled row.
    std::vector<std::vector<std::unique_ptr<value::SwitchAccessor>>> _outFrameFirstAccessors;
    std::vector<std::vector<std::unique_ptr<BufferedRowAccessor>>> _outFrameFirstBufferAccessors;
    std::vector<size_t> _frameFirstBufferedRowIdxes;
    std::vector<std::vector<std::unique_ptr<SpilledRowAccessor>>> _outFrameFirstSpillAccessors;
    std::vector<value::MaterializedRow> _frameFirstSpilledRows;
    // The accessors for the document slots of the last document in window frame. Either pointing
    // to a row in the window buffer by an index, or a recovered spilled row.
    std::vector<std::vector<std::unique_ptr<value::SwitchAccessor>>> _outFrameLastAccessors;
    std::vector<std::vector<std::unique_ptr<BufferedRowAccessor>>> _outFrameLastBufferAccessors;
    std::vector<size_t> _frameLastBufferedRowIdxes;
    std::vector<std::vector<std::unique_ptr<SpilledRowAccessor>>> _outFrameLastSpillAccessors;
    std::vector<value::MaterializedRow> _frameLastSpilledRows;
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
    // The last id in the buffered rows.
    size_t _lastRowId{0};
    // The id of the next partition that we have fetched into the buffer.
    boost::optional<size_t> _nextPartitionId{boost::none};
    // Whether the child stage has reached EOF.
    bool _isEOF{false};

    // Whether to allow spilling.
    bool _allowDiskUse;
    // The spilled record storage.
    std::unique_ptr<SpillingStore> _recordStore{nullptr};
    // The temporary data structure to hold record batch before they're spilled.
    static const long _batchSize = 1000;
    std::vector<Record> _records;
    std::vector<SharedBuffer> _recordBuffers;

    /**
     * A memory estimator for the window state. Incrementally calculate the linear regression of the
     * memory samples of different window frame size. The intervals between sample frame sizes are
     * increased in a capped exponential backoff fashion.
     *
     * https://stats.stackexchange.com/questions/23481/are-there-algorithms-for-computing-running-linear-or-logistic-regression-param
     */
    struct WindowStateMemoryEstimator {
        WindowStateMemoryEstimator() {
            reset();
        }

        bool shouldSample(size_t x) {
            if (x == sampleCheckpoint) {
                // This checkpoing increment ensures we sample at two's power plus one (1, 2, 3, 5,
                // 9, ...), becasue some data structures (e.g. std::vector) grows internal memory at
                // that point.
                size_t checkPointIncrement = x <= 2 ? static_cast<size_t>(1) : std::bit_floor(x);
                sampleCheckpoint += std::min(checkPointIncrement, maxSampleCheckpointIncrement);
                return true;
            }
            return false;
        }

        void sample(size_t x, size_t y) {
            n += 1;
            double m = (n - 1) / n;
            double dx = x - meanX;
            double dy = y - meanY;
            varX += (m * dx * dx - varX) / n;
            covXY += (m * dx * dy - covXY) / n;
            meanX += dx / n;
            meanY += dy / n;
        }

        size_t estimate(size_t x) {
            // Variance of x will be zero when less than two samples are taken.
            double a = n <= 1 ? 0 : covXY / varX;
            double b = meanY - a * meanX;
            return a * x + b;
        }

        void reset() {
            sampleCheckpoint = 1;
            meanX = 0;
            meanY = 0;
            varX = 0;
            covXY = 0;
            n = 0;
        }

        const size_t maxSampleCheckpointIncrement =
            internalQuerySlotBasedExecutionWindowStateMemorySamplingAtLeast.load();
        size_t sampleCheckpoint;
        double meanX;
        double meanY;
        double varX;
        double covXY;
        double n;
    };

    /**
     * A memory estimator for the window buffer. Tracks the mean of each record memory sample. The
     * samples are taken in a capped exponential backoff fashion.
     */
    struct WindowBufferMemoryEstimator {
        WindowBufferMemoryEstimator() {
            reset();
        }

        bool shouldSample() {
            sampleCounter++;
            if (sampleCounter == sampleCheckpoint) {
                sampleCounter = 0;
                sampleCheckpoint = std::min(sampleCheckpoint * 2, maxSampleCheckpoint);
                return true;
            }
            return false;
        }

        void sample(size_t y) {
            n += 1;
            mean += (y - mean) / n;
        }

        size_t estimate(size_t x) {
            return x * mean;
        }

        void reset() {
            sampleCounter = 0;
            sampleCheckpoint = 1;
            mean = 0;
            n = 0;
        }

        const size_t maxSampleCheckpoint =
            internalQuerySlotBasedExecutionWindowBufferMemorySamplingAtLeast.load();
        size_t sampleCheckpoint;
        size_t sampleCounter;
        double mean;
        double n;
    };

    // The memory estimator for the window buffer.
    WindowBufferMemoryEstimator _windowBufferMemoryEstimator;
    // The memory size for each window accumulator state.
    std::vector<std::vector<WindowStateMemoryEstimator>> _windowStateMemoryEstimators;
    // Memory threshold before spilling.
    const size_t _memoryThreshold =
        loadMemoryLimit(StageMemoryLimit::DocumentSourceSetWindowFieldsMaxMemoryBytes);

    // The failpoint counter to force spilling, incremented for every window function update,
    // every document.
    long long _failPointSpillCounter{0};

    WindowStats _specificStats;
};

}  // namespace mongo::sbe
