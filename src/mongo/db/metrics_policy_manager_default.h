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
    bool requiresServerStatusFiltering(OperationContext* opCtx) const override;

    const std::vector<std::string>& getServerStatusAllowlistPaths() const override;

    const PathMatcherNode& getServerStatusAllowlistMatcher() const override;

    bool requiresReplSetGetStatusFiltering(OperationContext* opCtx) const override;

    const std::vector<std::string>& getReplSetGetStatusAllowlistPaths() const override;

    const PathMatcherNode& getReplSetGetStatusAllowlistMatcher() const override;
};

}  // namespace mongo
