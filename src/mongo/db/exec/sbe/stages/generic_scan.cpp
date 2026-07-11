// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/stages/generic_scan.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/stages/generic_scan.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep

#include <cstring>

#include <boost/optional/optional.hpp>

namespace mongo {

namespace {
MONGO_FAIL_POINT_DEFINE(hangGenericScanGetNext);
}  // namespace

namespace sbe {
GenericScanStage::GenericScanStage(UUID collUuid,
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
                                   bool participateInTrialRunTracking)
    : ScanStageBaseImpl<GenericScanStage>(collUuid,
                                          dbName,
                                          recordSlot,
                                          recordIdSlot,
                                          snapshotIdSlot,
                                          indexIdentSlot,
                                          indexKeySlot,
                                          indexKeyPatternSlot,
                                          scanFieldNames,
                                          scanFieldSlots,
                                          yieldPolicy,
                                          nodeId,
                                          scanOpenCallback,
                                          forward,
                                          // Optional arguments:
                                          participateInTrialRunTracking) {}

/**
 * Constructor for clone(). Copies '_state' shared_ptr.
 */
GenericScanStage::GenericScanStage(std::shared_ptr<ScanStageBaseState> state,
                                   PlanYieldPolicySBE* yieldPolicy,
                                   PlanNodeId nodeId,
                                   bool participateInTrialRunTracking)
    : ScanStageBaseImpl(std::move(state), yieldPolicy, nodeId, participateInTrialRunTracking) {
}  // ScanStageBaseImpl constructor for clone()

std::unique_ptr<PlanStage> GenericScanStage::clone() const {
    return std::make_unique<GenericScanStage>(
        _state, _yieldPolicy, _commonStats.nodeId, participateInTrialRunTracking());
}

PlanState GenericScanStage::getNext() {
    if (MONGO_unlikely(hangGenericScanGetNext.shouldFail())) {
        hangGenericScanGetNext.pauseWhileSet();
    }
    auto optTimer(getOptTimer(_opCtx));

    handleInterruptAndSlotAccess();
    boost::optional<Record> nextRecord;
    nextRecord = _cursor->next();

    if (!nextRecord) {
        handleEOF(nextRecord);
        return trackPlanState(PlanState::IS_EOF);
    }

    resetRecordId(nextRecord);

    if (_state->recordIdSlot) {
        _recordId = std::move(nextRecord->id);
        _recordIdAccessor.reset(value::TagValueView{value::TypeTags::RecordId,
                                                    value::bitcastFrom<RecordId*>(&_recordId)});
    }

    if (!_scanFieldAccessors.empty()) {
        placeFieldsFromRecordInAccessors(*nextRecord, _state->scanFieldNames, _scanFieldAccessors);
    }

    ++_specificStats.numReads;
    trackRead();
    return trackPlanState(PlanState::ADVANCED);
}

std::unique_ptr<PlanStageStats> GenericScanStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->specific = std::make_unique<ScanStats>(_specificStats);

    if (includeDebugInfo) {
        BSONObjBuilder bob;
        getStatsShared(bob);
        ret->debugInfo = bob.obj();
    }
    return ret;
}

void GenericScanStage::doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                                    DebugPrintInfo& debugPrintInfo) const {
    ret.emplace_back("generic");
    debugPrintShared(ret);
}
}  // namespace sbe
}  // namespace mongo
