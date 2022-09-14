/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/ce/collection_statistics.h"
#include "mongo/db/query/ce/stats_cache_loader.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/read_through_cache.h"

namespace mongo {

using namespace mongo::ce;

using StatsCacheType = ReadThroughCache<StatsPathString, StatsCacheVal>;
using StatsCacheValueHandle = StatsCacheType::ValueHandle;

/**
 * Collectoin statistics read through cache. It reads from the persitent storage but never wrties to
 * it. Stored on the service context.
 */
class StatsCache : public StatsCacheType {
public:
    /**
     * Stores the cache on the specified service context. May only be called once for the lifetime
     * of the service context.
     */
    static void set(ServiceContext* serviceContext, std::unique_ptr<StatsCache> cache);
    static void clearForTests(ServiceContext* serviceContext);

    static StatsCache& get(ServiceContext* serviceContext);
    static StatsCache& get(OperationContext* opCtx);

    /**
     * The constructor provides the Service context under which this cache has been instantiated,
     * and a Thread pool to be used for invoking the blocking 'lookup' calls. The size is the number
     * of entries the underlying LRU cache will hold.
     */
    StatsCache(ServiceContext* service,
               std::unique_ptr<StatsCacheLoader> cacheLoader,
               ThreadPoolInterface& threadPool,
               int size);

    /**
     *  Returns statsCacheLoader currently used for testing only.
     */
    StatsCacheLoader* getStatsCacheLoader() {
        invariant(_statsCacheLoader);

        return _statsCacheLoader.get();
    }

private:
    /**
     * Reads collection stats from the underlying storage if its not found in the in memory cache.
     */
    LookupResult _lookupStats(OperationContext* opCtx,
                              const StatsPathString& statsPath,
                              const ValueHandle& stats);

    Mutex _mutex = MONGO_MAKE_LATCH("StatsCache::_mutex");

    std::unique_ptr<StatsCacheLoader> _statsCacheLoader;
};

}  // namespace mongo
