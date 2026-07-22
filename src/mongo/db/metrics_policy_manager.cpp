// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/metrics_policy_manager.h"

#include "mongo/db/operation_context.h"

namespace mongo {
namespace {

const auto getMetricsPolicyManager =
    ServiceContext::declareDecoration<std::unique_ptr<MetricsPolicyManager>>();

}  // namespace

MetricsPolicyManager& MetricsPolicyManager::get(ServiceContext* svcCtx) {
    auto* ptr = getMetricsPolicyManager(svcCtx).get();
    invariant(ptr, "MetricsPolicyManager must be set during ServiceContext initialization");
    return *ptr;
}

MetricsPolicyManager& MetricsPolicyManager::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

void MetricsPolicyManager::set(ServiceContext* svcCtx,
                               std::unique_ptr<MetricsPolicyManager>&& manager) {
    getMetricsPolicyManager(svcCtx) = std::move(manager);
}

void MetricsPolicyManager::_throwNoAllowlistError() {
    uasserted(ErrorCodes::IllegalOperation, "No allowlist since metrics should not be filtered");
}

}  // namespace mongo
