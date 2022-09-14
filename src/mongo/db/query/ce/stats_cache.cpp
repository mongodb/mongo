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


#include "mongo/platform/basic.h"

#include "mongo/db/query/ce/stats_cache.h"

#include "mongo/db/query/ce/collection_statistics.h"
#include "mongo/util/read_through_cache.h"

#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {
using namespace mongo::ce;

namespace {

const auto statsCacheDecoration = ServiceContext::declareDecoration<std::unique_ptr<StatsCache>>();

}  // namespace

StatsCache::StatsCache(ServiceContext* service,
                       std::unique_ptr<StatsCacheLoader> cacheLoader,
                       ThreadPoolInterface& threadPool,
                       int size)
    : ReadThroughCache(
          _mutex,
          service,
          threadPool,
          [this](OperationContext* opCtx,
                 const StatsPathString& statsPath,
                 const ValueHandle& stats) { return _lookupStats(opCtx, statsPath, stats); },
          size),
      _statsCacheLoader(std::move(cacheLoader)) {}

StatsCache::LookupResult StatsCache::_lookupStats(OperationContext* opCtx,
                                                  const StatsPathString& statsPath,
                                                  const StatsCacheValueHandle& stats) {

    try {
        invariant(_statsCacheLoader);
        auto newStats = _statsCacheLoader->getStats(opCtx, statsPath).get();
        return LookupResult(std::move(newStats));
    } catch (const DBException& ex) {
        if (ex.code() == ErrorCodes::NamespaceNotFound) {
            return StatsCache::LookupResult(boost::none);
        }
        throw;
    }
}

void StatsCache::set(ServiceContext* serviceContext, std::unique_ptr<StatsCache> cache) {
    auto& statsCache = statsCacheDecoration(serviceContext);
    invariant(!statsCache);

    statsCache = std::move(cache);
}

void StatsCache::clearForTests(ServiceContext* serviceContext) {
    auto& statsCache = statsCacheDecoration(serviceContext);
    invariant(statsCache);

    statsCache.reset();
}

StatsCache& StatsCache::get(ServiceContext* serviceContext) {
    auto& statsCache = statsCacheDecoration(serviceContext);
    invariant(statsCache);

    return *statsCache;
}

StatsCache& StatsCache::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}
}  // namespace mongo
