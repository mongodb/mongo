// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/initialize_tenant_to_shard_cache.h"

#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"

#include <utility>

namespace mongo {
namespace {
bool initSet = false;
std::function<void(ServiceContext* service)> tenantToShardCacheInitializer =
    [](ServiceContext* service) {
    };
}  // namespace
}  // namespace mongo

void mongo::registerTenantToShardCacheInitializer(
    std::function<void(mongo::ServiceContext* service)> init) {
    invariant(!initSet);
    tenantToShardCacheInitializer = std::move(init);
    initSet = true;
}

void mongo::initializeTenantToShardCache(mongo::ServiceContext* service) {
    tenantToShardCacheInitializer(service);
}
