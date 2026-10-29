// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/query/compiler/stats/stats_cache_loader.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool_interface.h"
#include "mongo/util/modules.h"
#include "mongo/util/read_through_cache.h"

#include <memory>
#include <mutex>

#include <boost/smart_ptr.hpp>

namespace mongo::stats {
using StatsCacheType = ReadThroughCache<StatsPathString, StatsCacheVal>;
using StatsCacheValueHandle = StatsCacheType::ValueHandle;

/**
 * Collectoin statistics read through cache. It reads from the persitent storage but never wrties to
 * it.
 */
class StatsCache : public StatsCacheType {
public:
    /**
     * The constructor provides the Service context under which this cache has been instantiated,
     * and a Thread pool to be used for invoking the blocking 'lookup' calls. The size is the number
     * of entries the underlying LRU cache will hold.
     */
    StatsCache(Service* service,
               std::unique_ptr<StatsCacheLoader> cacheLoader,
               ThreadPoolInterface& threadPool,
               int size);

    /**
     *  Returns statsCacheLoader currently used for testing only.
     */
    StatsCacheLoader* getStatsCacheLoader() {
        tassert(11051900, "Expecting stats cache loader to be provided", _statsCacheLoader);

        return _statsCacheLoader.get();
    }

private:
    /**
     * Reads collection stats from the underlying storage if its not found in the in memory cache.
     */
    LookupResult _lookupStats(OperationContext* opCtx,
                              const StatsPathString& statsPath,
                              const ValueHandle& stats);

    std::mutex _mutex;

    std::unique_ptr<StatsCacheLoader> _statsCacheLoader;
};

}  // namespace mongo::stats
