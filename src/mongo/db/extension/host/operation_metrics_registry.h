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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/host_connector/handle/host_operation_metrics_handle.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/executable_agg_stage.h"

#include <map>
#include <string>

namespace mongo::extension::host {

/**
 * OperationMetricsRegistry manages a collection of operation metrics for extension-executed
 * aggregation stages during query execution.
 *
 * This registry maintains owned HostOperationMetricsHandle instances, one per unique aggregation
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

    extension::host_connector::HostOperationMetricsHandle* getOrCreateMetrics(
        const std::string& stageName, UnownedExecAggStageHandle execStage);

private:
    std::map<std::string, extension::host_connector::HostOperationMetricsHandle> _metrics;
};

};  // namespace mongo::extension::host
