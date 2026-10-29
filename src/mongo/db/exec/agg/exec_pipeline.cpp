// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/exec_pipeline.h"

#include "mongo/db/query/explain_policy.h"
#include "mongo/db/query/plan_summary_stats_visitor.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#include <algorithm>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::exec::agg {

Pipeline::Pipeline(StageContainer&& stages, boost::intrusive_ptr<ExpressionContext> pCtx)
    : _stages(std::move(stages)), _expCtx(std::move(pCtx)) {
    tassert(10549300, "Cannot create an empty execution pipeline", !_stages.empty());
    tassert(10537101, "Aggregation pipeline missing ExpressionContext", _expCtx != nullptr);
    for (const auto& stage : _stages) {
        tassert(10617300, "stage must not be null", stage != nullptr);
    }
}

Pipeline::~Pipeline() {
    try {
        // 'dispose()' performs the actual disposal only once, so it is safe to call it
        // unconditionally from here.
        dispose();
        tassert(10617100, "expecting the pipeline to be disposed at destruction", _disposed);
    } catch (const std::exception& ex) {
        LOGV2_ERROR(12562102,
                    "Caught an unexpected exception while in query execution pipeline destructor",
                    "error"_attr = ex.what());

        // The 'dassert()' will terminate the process in debug mode only. In release mode, we
        // continue.
        dassert(false, "unexpected exception in pipeline destructor");
    }
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
    _expCtx->setOperationContext(nullptr);

    for (auto&& source : _stages) {
        source->detachFromOperationContext();
    }

    // Check for a null operation context to make sure that all children detached correctly.
    checkValidOperationContext();
}

void Pipeline::reattachToOperationContext(OperationContext* opCtx) {
    _expCtx->setOperationContext(opCtx);

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

std::vector<Value> Pipeline::writeExplainOps(const query_shape::SerializationOptions& opts) const {
    tassert(10908500,
            "this method should not be called unless the explain verbosity is 'executionStats'",
            explainPolicyFor(*opts.verbosity).hasExecStats());

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
    } catch (const std::exception& ex) {
        // When we catch an exception during disposal, some resources may have been leaked.
        // Log an error about this and rethrow the error, so the caller can either handle
        // it or crash the server (in case the call came from a destructor).
        LOGV2_ERROR(10532600,
                    "Caught an unexpected exception while disposing query execution pipeline, "
                    "which could have leaked some of the pipeline's resources",
                    "error"_attr = ex.what());
        throw;
    }
}

}  // namespace mongo::exec::agg
