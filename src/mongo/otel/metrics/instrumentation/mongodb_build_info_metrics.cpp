// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
