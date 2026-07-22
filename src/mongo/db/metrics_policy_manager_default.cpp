// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/metrics_policy_manager_default.h"

#include "mongo/db/service_context.h"

namespace mongo {
namespace {

ServiceContext::ConstructorActionRegisterer registerMetricsPolicyManager{
    "MetricsPolicyManagerDefault", [](ServiceContext* service) {
        MetricsPolicyManager::set(service, std::make_unique<MetricsPolicyManagerDefault>());
    }};

}  // namespace

bool MetricsPolicyManagerDefault::requiresServerStatusFiltering(OperationContext* opCtx) const {
    return false;
}

const std::vector<std::string>& MetricsPolicyManagerDefault::getServerStatusAllowlistPaths() const {
    _throwNoAllowlistError();
}

const PathMatcherNode& MetricsPolicyManagerDefault::getServerStatusAllowlistMatcher() const {
    _throwNoAllowlistError();
}

}  // namespace mongo
