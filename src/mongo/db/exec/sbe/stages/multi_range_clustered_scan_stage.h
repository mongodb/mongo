/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
