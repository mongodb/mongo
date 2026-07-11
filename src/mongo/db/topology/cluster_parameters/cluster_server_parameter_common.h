// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/base/status_with.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/tenant_id.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/modules.h"

#include <set>

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Get all tenants which have non-empty config databases (<tenantId>_config) existing on the given
 * shard.
 */
StatusWith<std::set<boost::optional<TenantId>>> getTenantsWithConfigDbsOnShard(
    OperationContext* opCtx, Shard& shard);

StatusWith<std::set<boost::optional<TenantId>>> getTenantsWithConfigDbsOnShard(
    OperationContext* opCtx,
    RemoteCommandTargeter& targeter,
    std::shared_ptr<executor::TaskExecutor> executor);

}  // namespace mongo
