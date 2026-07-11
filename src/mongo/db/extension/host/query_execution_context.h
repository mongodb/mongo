// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/curop.h"
#include "mongo/db/extension/host_connector/adapter/query_execution_context_adapter.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

namespace mongo::extension::host {

/**
 * QueryExecutionContext provides concrete host implementation of the query execution context
 * interface for extensions running within the host process.
 *
 * The context wraps an ExpressionContext, which holds references to the active OperationContext
 * and other query state needed during pipeline execution. It delegates interrupt checks to the
 * underlying OperationContext and exposes operation metrics through OpDebug's extension metrics
 * registry.
 *
 * This class is intended for use by the extension host connector framework and should not be
 * instantiated directly by extension code.
 */
class QueryExecutionContext : public host_connector::QueryExecutionContextBase {
public:
    QueryExecutionContext(const ExpressionContext* ctx) : _ctx(ctx) {}

    Status checkForInterrupt() const override {
        return _ctx->getOperationContext()->checkForInterruptNoAssert();
    }

    UnownedOperationMetricsHandle getMetrics(
        const std::string& stageName, const UnownedExecAggStageHandle& execStage) const override {
        auto& opDebug = CurOp::get(_ctx->getOperationContext())->debug();
        auto& opDebugMetrics = opDebug.extensionMetrics;
        return opDebugMetrics.getOrCreateMetrics(stageName, execStage);
    }

    int64_t getDeadlineTimestampMs() const override {
        return _ctx->getOperationContext()->getDeadline().asInt64();
    }

    BSONObj getHostMetrics(const std::vector<std::string>& metricNames) const override {
        auto* opCtx = _ctx->getOperationContext();
        auto* curOp = CurOp::get(opCtx);
        const auto& opDebug = curOp->debug();

        // OpDebug::appendStaged() returns a lambda that accepts an OpDebug object, and returns a
        // snapshot of the requested metrics.
        StringSet requestedFields(metricNames.begin(), metricNames.end());
        auto appendFn =
            OpDebug::appendStaged(opCtx, std::move(requestedFields), /*needWholeDocument=*/false);
        return appendFn(OpDebug::AppendArgs(opCtx, opDebug, *curOp));
    }

private:
    const ExpressionContext* _ctx;
};

}  // namespace mongo::extension::host
