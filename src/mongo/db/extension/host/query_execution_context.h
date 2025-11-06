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
#pragma once

#include "mongo/db/curop.h"
#include "mongo/db/extension/host_connector/handle/executable_agg_stage.h"
#include "mongo/db/extension/host_connector/query_execution_context_adapter.h"
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

    host_connector::HostOperationMetricsHandle* getMetrics(
        const std::string& stageName,
        const host_connector::UnownedExecAggStageHandle& execStage) const override {
        auto& opDebug = CurOp::get(_ctx->getOperationContext())->debug();
        auto& opDebugMetrics = opDebug.extensionMetrics;
        return opDebugMetrics.getOrCreateMetrics(stageName, execStage);
    }

private:
    const ExpressionContext* _ctx;
};

}  // namespace mongo::extension::host
