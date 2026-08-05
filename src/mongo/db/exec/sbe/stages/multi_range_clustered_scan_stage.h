// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/query/record_id_range_list.h"
#include "mongo/db/storage/record_store.h"

#include <memory>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace sbe {

/**
 * Scan stage for a clustered collection scan over multiple disjoint RecordId ranges. The range
 * list is passed as a constructor argument. Between ranges the stage seeks directly to the start
 * of the next range rather than streaming through the gap.
 *
 * Single-range and unbounded clustered scans are handled by ScanStage, not by this stage. The
 * stage builder picks between the two based on the number of ranges in the planner's
 * RecordIdRangeList.
 */
class MultiRangeClusteredScanStage final : public ScanStageBaseImpl<MultiRangeClusteredScanStage> {
    friend class ScanStageBaseImpl<MultiRangeClusteredScanStage>;

public:
    MultiRangeClusteredScanStage(UUID collUuid,
                                 DatabaseName dbName,
                                 boost::optional<value::SlotId> recordSlot,
                                 boost::optional<value::SlotId> recordIdSlot,
                                 boost::optional<value::SlotId> snapshotIdSlot,
                                 boost::optional<value::SlotId> indexIdentSlot,
                                 boost::optional<value::SlotId> indexKeySlot,
                                 boost::optional<value::SlotId> indexKeyPatternSlot,
                                 std::vector<std::string> scanFieldNames,
                                 value::SlotVector scanFieldSlots,
                                 RecordIdRangeList rangeList,
                                 bool forward,
                                 PlanYieldPolicySBE* yieldPolicy,
                                 PlanNodeId nodeId,
                                 ScanOpenCallback scanOpenCallback,
                                 bool participateInTrialRunTracking = true);

    /**
     * Constructor for clone(). Copies '_state' shared_ptr.
     */
    MultiRangeClusteredScanStage(std::shared_ptr<ScanStageBaseState> state,
                                 PlanYieldPolicySBE* yieldPolicy,
                                 PlanNodeId nodeId,
                                 RecordIdRangeList rangeList,
                                 bool participateInTrialRunTracking);

    std::unique_ptr<PlanStage> clone() const final;
    void prepare(CompileCtx& ctx) final;
    void close() final;
    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const final;
    void doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                      DebugPrintInfo& debugPrintInfo) const final;

private:
    inline RecordCursor* getActiveCursor() const {
        return _cursor.get();
    }
    void scanResetState(bool reOpen);
    void getNextHangFailPoint();
    bool pastEnd() const {
        return _havePassedScanEndRecordId;
    }
    boost::optional<Record> getNextInternal();

    std::unique_ptr<SeekableRecordCursor> _cursor;

    // Have we crossed the outer scan end bound?
    bool _havePassedScanEndRecordId = false;

    // Index of the range currently being scanned.
    // Advances 0 → N-1 for forward scans, N-1 → 0 for backward scans.
    // -1ULL if the initial seek has not been performed yet.
    size_t _currentRangeIdx = -1ULL;

    RecordIdRangeList _rangeList;
};  // class MultiRangeClusteredScanStage

}  // namespace sbe
}  // namespace mongo
