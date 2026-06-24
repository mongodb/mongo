/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/otel/metrics/instrumentation/mongodb_build_info_metrics.h"

#include "mongo/db/server_options.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metric_unit.h"
#include "mongo/otel/metrics/metrics_attributes.h"
#include "mongo/otel/metrics/metrics_gauge.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/platform/process_id.h"
#include "mongo/util/version.h"

namespace mongo {

using otel::metrics::AttributeDefinition;
using otel::metrics::MetricNames;
using otel::metrics::MetricsService;
using otel::metrics::MetricUnit;

void installMongoDBBuildInfoMetrics() {
    const auto& vii = VersionInfoInterface::instance();

    const std::string_view name = serverGlobalParams.binaryName;
    const std::string instanceId = ProcessId::getCurrent().toString();
    const std::string_view version = vii.version();
    const std::string_view gitVersion = vii.gitVersion();

    const std::string_view kDesc =
        "`Server` build info (always 1; see label for build information)";

    if (serverGlobalParams.clusterRole.hasExclusively(ClusterRole::RouterServer)) {
        auto& gauge = MetricsService::instance()
                          .createInt64Gauge<std::string_view,
                                            std::string_view,
                                            std::string_view,
                                            std::string_view>(
                              MetricNames::kMongoDBBuildInfo,
                              std::string(kDesc),
                              MetricUnit::kState,
                              AttributeDefinition<std::string_view>{"name", {name}},
                              AttributeDefinition<std::string_view>{"instance_id",
                                                                    {std::string_view(instanceId)}},
                              AttributeDefinition<std::string_view>{"version", {version}},
                              AttributeDefinition<std::string_view>{"git_version", {gitVersion}});
        gauge.set(1, {name, std::string_view(instanceId), version, gitVersion});
    } else {
        const std::string_view storageEngine = storageGlobalParams.engine;
        auto& gauge =
            MetricsService::instance()
                .createInt64Gauge<std::string_view,
                                  std::string_view,
                                  std::string_view,
                                  std::string_view,
                                  std::string_view>(
                    MetricNames::kMongoDBBuildInfo,
                    std::string(kDesc),
                    MetricUnit::kState,
                    AttributeDefinition<std::string_view>{"name", {name}},
                    AttributeDefinition<std::string_view>{"instance_id",
                                                          {std::string_view(instanceId)}},
                    AttributeDefinition<std::string_view>{"version", {version}},
                    AttributeDefinition<std::string_view>{"git_version", {gitVersion}},
                    AttributeDefinition<std::string_view>{"storage_engine", {storageEngine}});
        gauge.set(1, {name, std::string_view(instanceId), version, gitVersion, storageEngine});
    }
}

}  // namespace mongo
