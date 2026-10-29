// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/host/operation_metrics_registry.h"

namespace mongo::extension::host {
BSONObj OperationMetricsRegistry::serialize() const {
    BSONObjBuilder builder;
    for (const auto& [stageName, extensionMetrics] : _metrics) {
        builder.append(stageName, extensionMetrics->serialize());
    }
    return builder.obj();
}

UnownedOperationMetricsHandle OperationMetricsRegistry::getOrCreateMetrics(
    const std::string& stageName, UnownedExecAggStageHandle execStage) {
    auto it = _metrics.find(std::string(stageName));
    if (it == _metrics.end()) {
        auto metricsHandle = execStage->createMetrics();
        auto [newIt, inserted] = _metrics.emplace(stageName, std::move(metricsHandle));
        it = newIt;
    }

    return UnownedOperationMetricsHandle(it->second.get());
}

};  // namespace mongo::extension::host
