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

#include <algorithm>

namespace mongo::exec::agg {

Pipeline::Pipeline(StageContainer&& stages, boost::intrusive_ptr<ExpressionContext> pCtx)
    : _stages(std::move(stages)), expCtx(std::move(pCtx)) {
    tassert(10549300, "Cannot create an empty execution pipeline", !_stages.empty());
    tassert(10537101, "Aggregation pipeline missing ExpressionContext", expCtx != nullptr);
    for (const auto& stage : _stages) {
        tassert(10617300, "stage must not be null", stage != nullptr);
    }
}

Pipeline::~Pipeline() {
    if (_disposeInDestructor) {
        // 'dispose()' performs the actual disposal only once, so it is safe to call it again from
        // here.
        dispose();
    }
    tassert(10617100, "expecting the pipeline to be disposed at destruction", _disposed);
}

boost::optional<Document> Pipeline::getNext() {
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

void Pipeline::forceSpill() {
    _stages.back()->forceSpill();
}

std::vector<Value> Pipeline::writeExplainOps(const SerializationOptions& opts) const {
    tassert(10908500,
            "this method should not be called with explain verbosity below 'executionStats'",
            *opts.verbosity >= ExplainOptions::Verbosity::kExecStats);

    std::vector<Value> execArray;
    execArray.reserve(_stages.size());
    for (auto&& stage : _stages) {
        execArray.emplace_back(stage->getExplainOutput(opts));
    }

    return execArray;
}

bool Pipeline::usedDisk() const {
    return std::any_of(
        _stages.begin(), _stages.end(), [](const auto& stage) { return stage->usedDisk(); });
}

void Pipeline::dispose() {
    if (_disposed) {
        // Avoid double-disposal of the same pipeline.
        return;
    }
    try {
        _stages.back()->dispose();
        _disposed = true;
    } catch (...) {
        std::terminate();
    }
}

}  // namespace mongo::exec::agg
