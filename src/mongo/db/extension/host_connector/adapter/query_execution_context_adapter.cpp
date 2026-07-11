// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/extension/host_connector/adapter/query_execution_context_adapter.h"

#include "mongo/db/extension/shared/extension_status.h"

namespace mongo::extension::host_connector {

MongoExtensionStatus* QueryExecutionContextAdapter::_extCheckForInterrupt(
    const MongoExtensionQueryExecutionContext* ctx, MongoExtensionStatus* queryStatus) noexcept {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        const auto& execCtx = static_cast<const QueryExecutionContextAdapter*>(ctx)->getCtxImpl();
        Status interrupted = execCtx.checkForInterrupt();
        // Ensure output query status is valid before accessing it.
        if (!interrupted.isOK()) {
            auto statusAPI = StatusAPI(queryStatus);
            statusAPI.setCode(interrupted.code());
            statusAPI.setReason(interrupted.reason());
        }
    });
}

MongoExtensionStatus* QueryExecutionContextAdapter::_extGetMetrics(
    const MongoExtensionQueryExecutionContext* ctx,
    MongoExtensionExecAggStage* execAggStage,
    MongoExtensionOperationMetrics** metrics) noexcept {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        const auto& execCtx = static_cast<const QueryExecutionContextAdapter*>(ctx)->getCtxImpl();

        auto execStageHandle = UnownedExecAggStageHandle(execAggStage);
        const std::string stageName = std::string(execStageHandle->getName());

        *metrics = execCtx.getMetrics(stageName, execStageHandle).get();
    });
}

MongoExtensionStatus* QueryExecutionContextAdapter::_extGetDeadlineTimestampMs(
    const MongoExtensionQueryExecutionContext* ctx, int64_t* deadlineTimestampMs) noexcept {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        const auto& execCtx = static_cast<const QueryExecutionContextAdapter*>(ctx)->getCtxImpl();
        *deadlineTimestampMs = execCtx.getDeadlineTimestampMs();
    });
}

MongoExtensionStatus* QueryExecutionContextAdapter::_extGetHostMetrics(
    const MongoExtensionQueryExecutionContext* ctx,
    const MongoExtensionByteView* metricNames,
    uint64_t numMetricNames,
    MongoExtensionByteBuf** result) noexcept {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        const auto& execCtx = static_cast<const QueryExecutionContextAdapter*>(ctx)->getCtxImpl();

        std::vector<std::string> names;
        names.reserve(numMetricNames);
        for (uint64_t i = 0; i < numMetricNames; ++i) {
            names.emplace_back(byteViewAsStringView(metricNames[i]));
        }

        BSONObj metrics = execCtx.getHostMetrics(names);
        // Allocate a buffer on the heap. Ownership is transferred to the caller.
        *result = new ByteBuf(metrics);
    });
}
}  // namespace mongo::extension::host_connector
