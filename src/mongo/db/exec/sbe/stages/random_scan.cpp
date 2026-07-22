// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/stages/random_scan.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep

#include <cstring>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace sbe {

RandomScanStage::RandomScanStage(UUID collUuid,
                                 DatabaseName dbName,
                                 boost::optional<value::SlotId> recordSlot,
                                 boost::optional<value::SlotId> recordIdSlot,
                                 boost::optional<value::SlotId> snapshotIdSlot,
                                 boost::optional<value::SlotId> indexIdentSlot,
                                 boost::optional<value::SlotId> indexKeySlot,
                                 boost::optional<value::SlotId> indexKeyPatternSlot,
                                 std::vector<std::string> scanFieldNames,
                                 value::SlotVector scanFieldSlots,
                                 PlanYieldPolicySBE* yieldPolicy,
                                 PlanNodeId nodeId,
                                 // Optional arguments:
                                 bool participateInTrialRunTracking)
    : ScanStageBaseImpl<RandomScanStage>(collUuid,
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
                                         nullptr /* scanOpenCallback */,
                                         false /* forward */,
                                         // Optional arguments:
                                         participateInTrialRunTracking) {}

/**
 * Constructor for clone(). Copies '_state' shared_ptr.
 */
RandomScanStage::RandomScanStage(std::shared_ptr<ScanStageBaseState> state,
                                 PlanYieldPolicySBE* yieldPolicy,
                                 PlanNodeId nodeId,
                                 bool participateInTrialRunTracking)
    : ScanStageBaseImpl<RandomScanStage>(
          std::move(state), yieldPolicy, nodeId, participateInTrialRunTracking) {}

std::unique_ptr<PlanStage> RandomScanStage::clone() const {
    return std::make_unique<RandomScanStage>(
        _state, _yieldPolicy, _commonStats.nodeId, participateInTrialRunTracking());
}

std::unique_ptr<PlanStageStats> RandomScanStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->specific = std::make_unique<ScanStats>(_specificStats);

    if (includeDebugInfo) {
        BSONObjBuilder bob;
        getStatsShared(bob);
        ret->debugInfo = bob.obj();
    }
    return ret;
}

void RandomScanStage::doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                                   DebugPrintInfo& debugPrintInfo) const {
    DebugPrinter::addKeyword(ret, "random");
    debugPrintShared(ret);
}
}  // namespace sbe
}  // namespace mongo
