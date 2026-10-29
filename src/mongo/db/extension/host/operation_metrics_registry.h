// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/executable_agg_stage.h"
#include "mongo/db/extension/shared/handle/operation_metrics_handle.h"
#include "mongo/util/modules.h"

#include <map>
#include <string>

namespace mongo::extension::host {

/**
 * OperationMetricsRegistry manages a collection of operation metrics for extension-executed
 * aggregation stages during query execution.
 *
 * This registry maintains owned OwnedOperationMetricsHandle instances, one per unique aggregation
 * stage name, allowing metrics to be collected incrementally throughout pipeline execution.
 * It provides the following capabilities:
 *
 * - Lazy creation and caching of metrics per stage: metrics are created on first access via
 *   getOrCreateMetrics() and reused for subsequent accesses such that metrics in the same operation
 * are aggregated
 * - Serialization of all collected metrics into a BSON object for inclusion in operation logs
 *   and debug information
 *
 * The registry is typically owned by an OpDebug instance and populated during pipeline execution
 * as extensions execute their stages. After execution completes, the serialized metrics can be
 * included in operation diagnostics and slow query logs.
 */
class OperationMetricsRegistry {
public:
    bool empty() const {
        return _metrics.empty();
    }

    size_t size() const {
        return _metrics.size();
    }

    BSONObj serialize() const;

    UnownedOperationMetricsHandle getOrCreateMetrics(const std::string& stageName,
                                                     UnownedExecAggStageHandle execStage);

private:
    std::map<std::string, extension::OwnedOperationMetricsHandle> _metrics;
};

};  // namespace mongo::extension::host
