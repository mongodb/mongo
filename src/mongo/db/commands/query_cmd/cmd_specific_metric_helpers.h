// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/counter.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {

/**
 * Allocates and returns a counter metric for the given command name and statistic name/path.
 * 'statPath' may be dotted to nest the metric under a sub-section within the command's section of
 * serverStatus.
 */
Counter64* getSingletonMetricPtrWithinCmd(std::string_view commandName,
                                          std::string_view statPath,
                                          boost::optional<ClusterRole> role = boost::none);
}  // namespace mongo
