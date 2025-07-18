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

#include "mongo/db/operation_context.h"
#include "mongo/db/query/compiler/stats/stats_cache_loader.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool_interface.h"
#include "mongo/util/read_through_cache.h"

#include <memory>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
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

    stdx::mutex _mutex;

    std::unique_ptr<StatsCacheLoader> _statsCacheLoader;
};

}  // namespace mongo::stats
