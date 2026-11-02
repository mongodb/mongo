// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/stages/hash_agg_accumulator.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/row.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include <absl/container/inlined_vector.h>
#include <boost/optional/optional.hpp>

namespace mongo::sbe {
/**
 * Performs a streaming aggregation, clustering incoming rows on cluster key and grouping on group
 * keys within each cluster. Incoming rows with equal clustering keys must be contiguous. For
 * example, they may be sorted on the clustering key or the clustering keys may be unique. Within a
 * clustering key, multiple group-by keys may appear. Each distinct grouping will produce a single
 * output, consisting of the values of the group-by keys and the results of the aggregate functions.
 *
 * The 'keySlots' parameter defines the group-by slots. The 'clusterKeySlots' parameter defines
 * the cluster key slots. If the rows for a specific group-by slot are already contiguous, the
 * clustering key may reuse the group-by slot for the clustering key. However if for a specific key
 * slot values may appear interleaved, the clustering slot should apply some transformation such
 * that the same clustering keys are contiguous.
 *
 * The 'collatorSlot', if provided, changes the definition of string equality used when determining
 * whether two adjacent group-by keys or cluster keys are equal. This must match the collator of the
 * input.
 *
 * The 'accumulators' parameter is a vector defining an initializer expression, an accumulator
 * expression, and an output slot id.
 *
 * Slots from the input stage are not visible in the output. When a group is finalized, the input
 * slots hold values from the following group (or are EOF).
 *
 * Appears as the "streaming_group" stage in debug output.
 */
class StreamingAggStage final : public PlanStage {
public:
    StreamingAggStage(std::unique_ptr<PlanStage> input,
                      value::SlotVector keySlots,
                      value::SlotVector clusterKeySlots,
                      boost::optional<value::SlotId> collatorSlot,
                      std::vector<std::unique_ptr<HashAggAccumulator>> accumulators,
                      PlanNodeId planNodeId,
                      bool participateInTrialRunTracking = true);

    std::unique_ptr<PlanStage> clone() const final;
    void prepare(CompileCtx& ctx) final;
    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final;
    void open(bool reOpen) final;
    PlanState getNext() final;
    void close() final;

    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const final;
    const SpecificStats* getSpecificStats() const final;
    void doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                      DebugPrintInfo& debugPrintInfo) const final;
    size_t estimateCompileTimeSize() const final;

protected:
    bool shouldOptimizeSaveState(size_t) const final {
        return true;
    }

private:
    static constexpr size_t kMaxClusteredGroupKeys = 4;
    static constexpr size_t kMaxGroupsPerClusterKey = 1 << kMaxClusteredGroupKeys;

    void readInGroupKey();
    void readInClusterKey();
    void storeClusterKey();
    // Initializes a group entry for _inGroupKey, returns true if this is a new group.
    bool startGroup();
    void startNewGroupAt(size_t idx);

    /**
     * Reads the next row, and runs all necessary logic to prepare to accumulate. Handles closing
     * out the last cluster if the group crosses a cluster boundary, and initializing the group's
     * state if this is the first time that group has been seen. Guarantees that _curGroupIdx points
     * to a valid group entry that may be written to by the accumulators.
     *
     * Returns true if the cluster key changed and new rows are ready to be emitted.
     */
    bool prepareGroup();

    /* Accumulates the current row into the group at _curGroupIdx. */
    void accumulate();
    /* Closes the last cluster and emits one group */
    void endCluster();
    bool emitNext();

    const value::SlotVector _keySlots;
    const value::SlotVector _clusterKeySlots;
    const boost::optional<value::SlotId> _collatorSlot;
    // True when every group keys map 1-1 to cluster keys, so each cluster contains a single group.
    bool _noClustering;

    // _clusterKey is the owned clustered key of the last row accumulated. _inClusterKey and
    // _inGroupKey are scratch views of the current row's clustered and group-by keys. _outKey backs
    // the output key accessors and holds owned copies of the emitted group's key.
    value::MaterializedRow _clusterKey;
    value::MaterializedRow _inClusterKey;
    value::MaterializedRow _inGroupKey;
    value::MaterializedRow _outKey;
    value::MaterializedRowEq _keyEq;
    std::vector<value::SlotAccessor*> _inKeyAccessors;
    std::vector<value::SlotAccessor*> _clusteredInKeyAccessors;
    std::vector<value::MaterializedSingleRowAccessor> _outKeyAccessors;

    const std::vector<std::unique_ptr<HashAggAccumulator>> _accumulatorList;

    // Inline vectors holding the keys and agg results currently being accumulated or emitted. The
    // vectors are grow-only, the entries up to _numLiveGroups are valid and the rest are available
    // for future allocations. Groups in the current cluster are written from the start of the
    // vector. After a cluster is emitted, the first group of the next cluster is stored at index 0.

    // Layout when reading:  [    ...groups being accumulated...     | groups to be reused ]
    // Layout when emitting: [ groups emitted | groups to be emitted | groups to be reused ]
    //                                        ^ _emitIdx             ^ _numLiveGroups
    using GroupRows = absl::InlinedVector<value::MaterializedRow, kMaxGroupsPerClusterKey>;
    GroupRows _groupKeys;
    GroupRows _groupAggs;
    // End of range of valid groups in _groupKeys/_groupAggs
    size_t _numLiveGroups = 0;
    // If set, groups [_emitIdx, _numLiveGroups) are ready to be emitted. kNoGroup otherwise.
    static constexpr size_t kNoGroup = std::numeric_limits<size_t>::max();
    size_t _emitIdx = kNoGroup;

    // Index of the group currently being accumulated in _groupKeys/_groupAggs.
    // The accumulators read and write to this group through _curAggAccessors.
    size_t _curGroupIdx = kNoGroup;
    std::vector<value::MaterializedRowAccessor<GroupRows>> _curAggAccessors;

    // Values for the first group of the next cluster, stashed while emitting the previous cluster
    value::MaterializedRow _nextKey;
    value::MaterializedRow _nextAgg;

    std::vector<value::OwnedValueAccessor> _outAggAccessors;

    value::SlotAccessor* _collatorAccessor = nullptr;
    value::SlotAccessorMap _outAccessors;

    std::shared_ptr<vm::ByteCode> _bytecode;

    bool _compiled = false;
    bool _isEOF = false;
};
}  // namespace mongo::sbe
