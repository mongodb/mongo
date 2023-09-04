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

#include "mongo/db/exec/sbe/stages/search_cursor.h"
#include "mongo/base/string_data.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/util/str.h"

namespace mongo::sbe {
SearchCursorStage::SearchCursorStage(boost::optional<value::SlotId> idSlot,
                                     boost::optional<value::SlotId> resultSlot,
                                     std::vector<std::string> metadataNames,
                                     value::SlotVector metadataSlots,
                                     std::vector<std::string> fieldNames,
                                     value::SlotVector fieldSlots,
                                     boost::optional<value::SlotId> searchMetaSlot,
                                     value::SlotId cursorIdSlot,
                                     value::SlotId firstBatchSlot,
                                     PlanYieldPolicy* yieldPolicy,
                                     PlanNodeId planNodeId,
                                     bool participateInTrialRunTracking)
    : PlanStage("search_cursor"_sd, yieldPolicy, planNodeId, participateInTrialRunTracking),
      _idSlot(std::move(idSlot)),
      _resultSlot(std::move(resultSlot)),
      _metadataNames(std::move(metadataNames)),
      _metadataSlots(std::move(metadataSlots)),
      _fieldNames(std::move(fieldNames)),
      _fieldSlots(std::move(fieldSlots)),
      _searchMetaSlot(std::move(searchMetaSlot)),
      _cursorIdSlot(std::move(cursorIdSlot)),
      _firstBatchSlot(std::move(firstBatchSlot)) {}

std::unique_ptr<PlanStage> SearchCursorStage::clone() const {
    return std::make_unique<SearchCursorStage>(_idSlot,
                                               _resultSlot,
                                               _metadataNames.getUnderlyingVector(),
                                               _metadataSlots,
                                               _fieldNames.getUnderlyingVector(),
                                               _fieldSlots,
                                               _searchMetaSlot,
                                               _cursorIdSlot,
                                               _firstBatchSlot,
                                               _yieldPolicy,
                                               _commonStats.nodeId,
                                               _participateInTrialRunTracking);
}

void SearchCursorStage::prepare(CompileCtx& ctx) {}

value::SlotAccessor* SearchCursorStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    return ctx.getAccessor(slot);
}
void SearchCursorStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    _commonStats.opens++;
}

PlanState SearchCursorStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    return trackPlanState(PlanState::ADVANCED);
}

void SearchCursorStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();
}

std::unique_ptr<PlanStageStats> SearchCursorStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    return ret;
}

const SpecificStats* SearchCursorStage::getSpecificStats() const {
    return nullptr;
}

std::vector<DebugPrinter::Block> SearchCursorStage::debugPrint() const {
    auto ret = PlanStage::debugPrint();
    return ret;
}

size_t SearchCursorStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_children);
    size += size_estimator::estimate(_metadataSlots);
    size += size_estimator::estimate(_fieldSlots);
    return size;
}

}  // namespace mongo::sbe
