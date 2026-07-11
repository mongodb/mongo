// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands/query_cmd/cmd_specific_metric_helpers.h"

#include "mongo/db/commands/server_status/server_status_metric.h"

#include <string_view>

#include <fmt/format.h>

namespace mongo {
Counter64* getSingletonMetricPtrWithinCmd(std::string_view commandName,
                                          std::string_view statPath,
                                          boost::optional<ClusterRole> role) {
    return &*MetricBuilder<Counter64>{fmt::format("commands.{}.{}", commandName, statPath)}.setRole(
        role.value_or(ClusterRole::None));
}

}  // namespace mongo
