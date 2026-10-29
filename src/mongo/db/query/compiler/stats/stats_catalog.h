// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/compiler/stats/ce_histogram.h"
#include "mongo/db/query/compiler/stats/stats_cache.h"
#include "mongo/db/query/compiler/stats/stats_cache_loader.h"
#include "mongo/db/service_context.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>

namespace mongo::stats {
/**
 * This class owns statsCache and manages executor lifetime.
 */
class [[MONGO_MOD_PUBLIC]] StatsCatalog {
public:
    /**
     * Stores the catalog on the specified service context. May only be called once for the lifetime
     * of the service context.
     */
    static void set(ServiceContext* serviceContext, std::unique_ptr<StatsCatalog> catalog);

    static StatsCatalog& get(ServiceContext* serviceContext);
    static StatsCatalog& get(OperationContext* opCtx);

    /**
     * The constructor provides the Service under which the cache needs to be instantiated, and a
     * Thread pool to be used for invoking the blocking 'lookup' calls. The size is the number of
     * entries the underlying LRU cache will hold.
     */
    StatsCatalog(Service* service, std::unique_ptr<StatsCacheLoader> cacheLoader);

    ~StatsCatalog();

    StatusWith<std::shared_ptr<const CEHistogram>> getHistogram(OperationContext* opCtx,
                                                                const NamespaceString& nss,
                                                                const std::string& path);

    Status invalidatePath(const NamespaceString& nss, const std::string& path);

private:
    /**
     * The executor is used by the cache.
     */
    std::shared_ptr<ThreadPool> _executor;
    StatsCache _statsCache;
};

}  // namespace mongo::stats
