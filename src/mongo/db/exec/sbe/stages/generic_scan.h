// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace sbe {
class GenericScanStage final : public ScanStageBaseImpl<GenericScanStage> {
    friend class ScanStageBaseImpl<GenericScanStage>;

public:
    GenericScanStage(UUID collUuid,
                     DatabaseName dbName,
                     boost::optional<value::SlotId> recordSlot,
                     boost::optional<value::SlotId> recordIdSlot,
                     boost::optional<value::SlotId> snapshotIdSlot,
                     boost::optional<value::SlotId> indexIdentSlot,
                     boost::optional<value::SlotId> indexKeySlot,
                     boost::optional<value::SlotId> indexKeyPatternSlot,
                     std::vector<std::string> scanFieldNames,
                     value::SlotVector scanFieldSlots,
                     bool forward,
                     PlanYieldPolicySBE* yieldPolicy,
                     PlanNodeId nodeId,
                     ScanOpenCallback scanOpenCallback,
                     // Optional arguments:
                     bool participateInTrialRunTracking = true);
    /**
     * Constructor for clone(). Copies '_state' shared_ptr.
     */
    GenericScanStage(std::shared_ptr<ScanStageBaseState> state,
                     PlanYieldPolicySBE* yieldPolicy,
                     PlanNodeId nodeId,
                     bool participateInTrialRunTracking,
                     std::shared_ptr<ParallelScanChunks> parallelChunks = nullptr,
                     size_t parallelId = 0);

    std::vector<std::unique_ptr<PlanStage>> cloneForParallelExecution(CompileCtx& ctx,
                                                                      size_t dop) const final;
    std::unique_ptr<PlanStage> clone() const final;
    void inline prepare(CompileCtx& ctx) final {
        prepareShared(ctx);
    }
    inline void close() final {
        closeShared();
        _cursor.reset();
    }
    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const final;
    void doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                      DebugPrintInfo& debugPrintInfo) const final;

private:
    inline void scanResetState(bool reOpen) {
        _cursor = _coll->getCollectionPtr()->getCursor(_opCtx, _state->forward);
        if (_parallelChunks) {
            _parallelChunks->reset(_parallelId);
            _needNextChunk = true;
        }
    }
    inline RecordCursor* getActiveCursor() const {
        return _cursor.get();
    }
    void getNextHangFailPoint();
    bool pastEnd() const {
        return false;
    }
    boost::optional<Record> getNextInternal() {
        if (MONGO_likely(!_parallelChunks)) {
            return _cursor->next();
        }
        return getNextInternalParallel();
    }
    boost::optional<Record> getNextInternalParallel();

    std::unique_ptr<SeekableRecordCursor> _cursor;

    // The chunks left to scan, shared with the sibling clones of a parallel scan. Null when this
    // stage is not part of one.
    std::shared_ptr<ParallelScanChunks> _parallelChunks;
    // Identifies this clone among its siblings; selects the span of chunks it consumes first.
    size_t _parallelId = 0;

    // Only for a parallel scan: whether the next getNext() call must claim a new chunk and seek to
    // its lower bound, and the exclusive upper bound of the chunk currently being scanned.
    bool _needNextChunk = false;
    RecordId _excludedHigherBound;
};  // class GenericScanStage
}  // namespace sbe
}  // namespace mongo
