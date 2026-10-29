// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/metrics_policy_manager.h"

#include <string>
#include <vector>

namespace mongo {

class OperationContext;

/**
 * Default metrics policy manager. Does not perform metrics filtering.
 */
class MetricsPolicyManagerDefault : public MetricsPolicyManager {
public:
    bool requiresFiltering(OperationContext*, MetricsCategoryEnum, bool) const override;

    const std::vector<std::string>& getAllowlistPaths(MetricsCategoryEnum) const override;

    const PathMatcherNode& getAllowlistMatcher(MetricsCategoryEnum) const override;
};

}  // namespace mongo
