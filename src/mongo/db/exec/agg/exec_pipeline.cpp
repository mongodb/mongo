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

#include "mongo/db/exec/agg/exec_pipeline.h"

#include "mongo/db/query/plan_summary_stats_visitor.h"
#include "mongo/util/assert_util.h"

namespace mongo::exec::agg {

Pipeline::Pipeline(StageContainer&& stages, boost::intrusive_ptr<ExpressionContext> pCtx)
    : _stages(std::move(stages)), expCtx(std::move(pCtx)) {
    tassert(10537101, "Aggregation pipeline missing ExpressionContext", this->expCtx != nullptr);
}

boost::optional<Document> Pipeline::getNext() {
    // TODO SERVER-105493: Remove the following early exit after we prohibit creating empty
    // execution pipelines.
    if (MONGO_unlikely(_stages.empty())) {
        return boost::none;
    }
    auto nextResult = _stages.back()->getNext();
    while (nextResult.isPaused()) {
        nextResult = _stages.back()->getNext();
    }
    if (!nextResult.isEOF()) {
        // We'll get here for both statuses 'GetNextResult::ReturnStatus::kAdvanced' and
        // 'GetNextResult::ReturnStatus::kAdvancedControlDocument'.
        return nextResult.releaseDocument();
    }
    return boost::none;
}

GetNextResult Pipeline::getNextResult() {
    // TODO SERVER-105493: Remove the following assertion after we prohibit creating empty execution
    // pipelines.
    tassert(10394800, "cannon execute an empty aggregation pipeline", _stages.size());
    return _stages.back()->getNext();
}

void Pipeline::accumulatePlanSummaryStats(PlanSummaryStats& planSummaryStats) const {
    auto visitor = PlanSummaryStatsVisitor(planSummaryStats);
    for (auto&& stage : _stages) {
        if (auto specificStats = stage->getSpecificStats()) {
            specificStats->acceptVisitor(&visitor);
        }
    }
}

void Pipeline::detachFromOperationContext() {
    expCtx->setOperationContext(nullptr);

    for (auto&& source : _stages) {
        source->detachFromOperationContext();
    }

    // Check for a null operation context to make sure that all children detached correctly.
    checkValidOperationContext();
}

void Pipeline::reattachToOperationContext(OperationContext* opCtx) {
    expCtx->setOperationContext(opCtx);

    for (auto&& source : _stages) {
        source->reattachToOperationContext(opCtx);
    }

    checkValidOperationContext();
}

bool Pipeline::validateOperationContext(const OperationContext* opCtx) const {
    return std::all_of(_stages.begin(), _stages.end(), [this, opCtx](const auto& stage) {
        // All sources in a pipeline must share its expression context. Subpipelines may have a
        // different expression context, but must point to the same operation context. Let the
        // sources validate this themselves since they don't all have the same subpipelines, etc.
        return stage->getContext() == getContext() && stage->validateOperationContext(opCtx);
    });
}

void Pipeline::checkValidOperationContext() const {
    tassert(7406000,
            str::stream()
                << "All DocumentSources and subpipelines must have the same operation context",
            validateOperationContext(getContext()->getOperationContext()));
}

}  // namespace mongo::exec::agg
