/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/stages/generic_scan.h"

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/client.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/stages/generic_scan.h"
#include "mongo/db/exec/sbe/stages/random_scan.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep
#include "mongo/util/str.h"

#include <cstdint>
#include <cstring>
#include <set>

#include <boost/optional/optional.hpp>

namespace mongo {
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
                                   PlanYieldPolicy* yieldPolicy,
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
                                   PlanYieldPolicy* yieldPolicy,
                                   PlanNodeId nodeId,
                                   bool participateInTrialRunTracking)
    : ScanStageBaseImpl(std::move(state), yieldPolicy, nodeId, participateInTrialRunTracking) {
}  // ScanStageBaseImpl constructor for clone()

std::unique_ptr<PlanStage> GenericScanStage::clone() const {
    return std::make_unique<GenericScanStage>(
        _state, _yieldPolicy, _commonStats.nodeId, participateInTrialRunTracking());
}

PlanState GenericScanStage::getNext() {
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
        _recordIdAccessor.reset(
            false, value::TypeTags::RecordId, value::bitcastFrom<RecordId*>(&_recordId));
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

std::vector<DebugPrinter::Block> GenericScanStage::debugPrint(
    const DebugPrintInfo& debugPrintInfo) const {
    std::vector<DebugPrinter::Block> ret = PlanStage::debugPrint(debugPrintInfo);
    ret.emplace_back("generic");
    debugPrintShared(ret);
    return ret;
}
}  // namespace sbe
}  // namespace mongo
