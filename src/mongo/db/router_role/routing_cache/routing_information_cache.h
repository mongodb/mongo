// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/util/modules.h"

namespace mongo {

/*
 * In-memory only cache for routing information, meant to support the worker threads of the
 * Config Server (the Balancer Subsystem and the PeriodicShardedIndexConsistencyChecker).
 *
 * TODO (SERVER-97261): Delete this file and replace usages with the dedicated CatalogCache for
 * routing information.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] RoutingInformationCache : public CatalogCache {
public:
    RoutingInformationCache(ServiceContext* serviceCtx);

    ~RoutingInformationCache() override = default;

    static void set(ServiceContext* serviceCtx);
    static void setOverride(ServiceContext* serviceCtx, CatalogCache* cacheOverride);

    static CatalogCache* get(ServiceContext* serviceCtx);

    static CatalogCache* get(OperationContext* opCtx);
};

}  // namespace mongo
