// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/executable_agg_stage.h"
#include "mongo/db/extension/shared/handle/operation_metrics_handle.h"
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
    virtual UnownedOperationMetricsHandle getMetrics(
        const std::string& stageName, const UnownedExecAggStageHandle& execStage) const = 0;
    virtual int64_t getDeadlineTimestampMs() const = 0;
    virtual BSONObj getHostMetrics(const std::vector<std::string>& metricNames) const = 0;
};

/**
 * QueryExecutionContextAdapter is an adapter to ::MongoExtensionQueryExecutionContext,
 * providing host expression context methods to extensions.
 */
class QueryExecutionContextAdapter final : public ::MongoExtensionQueryExecutionContext {
public:
    QueryExecutionContextAdapter(std::unique_ptr<QueryExecutionContextBase> ctx)
        : ::MongoExtensionQueryExecutionContext{&VTABLE}, _ctx(std::move(ctx)) {
        tassert(11417100, "Provided QueryExecutionContextBase is null", _ctx != nullptr);
    }

    QueryExecutionContextAdapter(const QueryExecutionContextAdapter&) = delete;
    QueryExecutionContextAdapter& operator=(const QueryExecutionContextAdapter&) = delete;
    QueryExecutionContextAdapter(QueryExecutionContextAdapter&&) = delete;
    QueryExecutionContextAdapter& operator=(QueryExecutionContextAdapter&&) = delete;

    const QueryExecutionContextBase& getCtxImpl() const {
        return *_ctx;
    }

    static ::MongoExtensionQueryExecutionContextVTable getVTable() {
        return VTABLE;
    }

private:
    static MongoExtensionStatus* _extCheckForInterrupt(
        const MongoExtensionQueryExecutionContext* ctx, MongoExtensionStatus* queryStatus) noexcept;

    static MongoExtensionStatus* _extGetMetrics(const MongoExtensionQueryExecutionContext* ctx,
                                                MongoExtensionExecAggStage* execAggStage,
                                                MongoExtensionOperationMetrics** metrics) noexcept;

    static MongoExtensionStatus* _extGetDeadlineTimestampMs(
        const MongoExtensionQueryExecutionContext* ctx, int64_t* deadlineTimestampMs) noexcept;

    static MongoExtensionStatus* _extGetHostMetrics(const MongoExtensionQueryExecutionContext* ctx,
                                                    const MongoExtensionByteView* metricNames,
                                                    uint64_t numMetricNames,
                                                    MongoExtensionByteBuf** result) noexcept;

    static constexpr ::MongoExtensionQueryExecutionContextVTable VTABLE = {
        .check_for_interrupt = &_extCheckForInterrupt,
        .get_metrics = &_extGetMetrics,
        .get_deadline_timestamp_ms = &_extGetDeadlineTimestampMs,
        .get_host_metrics = &_extGetHostMetrics};

    std::unique_ptr<QueryExecutionContextBase> _ctx;
};

}  // namespace mongo::extension::host_connector
