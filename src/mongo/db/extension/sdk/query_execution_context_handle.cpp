// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/sdk/query_execution_context_handle.h"

#include <string_view>

namespace mongo::extension::sdk {

ExtensionGenericStatus QueryExecutionContextAPI::checkForInterrupt() const {
    // ExtensionGenericStatus defaults to OK, check_for_interrupt will only update the status if an
    // interrupt was detected.
    ExtensionGenericStatus queryStatus;
    invokeCAndConvertStatusToException(
        [&]() { return _vtable().check_for_interrupt(get(), &queryStatus); });
    return queryStatus;
}

UnownedOperationMetricsHandle QueryExecutionContextAPI::getMetrics(
    MongoExtensionExecAggStage* execStage) const {

    MongoExtensionOperationMetrics* metrics = nullptr;
    invokeCAndConvertStatusToException(
        [&]() { return _vtable().get_metrics(get(), execStage, &metrics); });

    return UnownedOperationMetricsHandle(metrics);
}

int64_t QueryExecutionContextAPI::getDeadlineTimestampMs() const {
    int64_t deadlineTimestampMs{0};
    invokeCAndConvertStatusToException(
        [&]() { return _vtable().get_deadline_timestamp_ms(get(), &deadlineTimestampMs); });
    return deadlineTimestampMs;
}

BSONObj QueryExecutionContextAPI::getHostMetrics(
    const std::vector<std::string_view>& metricNames) const {
    std::vector<MongoExtensionByteView> views;
    views.reserve(metricNames.size());
    for (const auto& name : metricNames) {
        views.push_back(stringViewAsByteView(name));
    }

    MongoExtensionByteBuf* buf = nullptr;
    invokeCAndConvertStatusToException(
        [&]() { return _vtable().get_host_metrics(get(), views.data(), views.size(), &buf); });

    // Take ownership of the returned buffer so that it gets cleaned up, then retrieve an owned
    // BSONObj to return to the caller.
    ExtensionByteBufHandle ownedBuf{buf};
    return bsonObjFromByteView(ownedBuf->getByteView()).getOwned();
}
}  // namespace mongo::extension::sdk
