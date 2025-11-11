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

#include "mongo/db/extension/host_connector/handle/host_operation_metrics_handle.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/executable_agg_stage.h"
#include "mongo/util/modules.h"

namespace mongo::extension::host_connector {

/**
 * QueryExecutionContextBase defines the abstract interface for query execution context
 * that extensions need to access during pipeline execution.
 *
 * This interface allows extensions to:
 * - Check if the current operation has been interrupted or cancelled
 * - Retrieve operation metrics and debugging information for specific aggregation stages
 *
 * Concrete implementations of this interface provide the bridge between the extension
 * framework and the host process's query execution state, encapsulating access to
 * OperationContext and operation debugging information.
 *
 * Implementations of this interface should be wrapped by QueryExecutionContextAdapter
 * for exposure to the public extension API.
 */
class QueryExecutionContextBase {
public:
    QueryExecutionContextBase() = default;
    virtual ~QueryExecutionContextBase() = default;
    virtual Status checkForInterrupt() const = 0;
    virtual HostOperationMetricsHandle* getMetrics(
        const std::string& stageName, const UnownedExecAggStageHandle& execStage) const = 0;
};

/**
 * QueryExecutionContextAdapter is an adapter to ::MongoExtensionQueryExecutionContext,
 * providing host expression context methods to extensions.
 */
class QueryExecutionContextAdapter final : public ::MongoExtensionQueryExecutionContext {
public:
    QueryExecutionContextAdapter(std::unique_ptr<QueryExecutionContextBase> ctx)
        : ::MongoExtensionQueryExecutionContext{&VTABLE}, _ctx(std::move(ctx)) {}

    const QueryExecutionContextBase& getCtxImpl() const {
        return *_ctx;
    }

private:
    static MongoExtensionStatus* _extCheckForInterrupt(
        const MongoExtensionQueryExecutionContext* ctx, MongoExtensionStatus* queryStatus) noexcept;

    static MongoExtensionStatus* _extGetMetrics(const MongoExtensionQueryExecutionContext* ctx,
                                                const MongoExtensionExecAggStage* execAggStage,
                                                MongoExtensionOperationMetrics** metrics) noexcept;

    static constexpr ::MongoExtensionQueryExecutionContextVTable VTABLE{
        .check_for_interrupt = &_extCheckForInterrupt, .get_metrics = &_extGetMetrics};

    std::unique_ptr<QueryExecutionContextBase> _ctx;
};

}  // namespace mongo::extension::host_connector
