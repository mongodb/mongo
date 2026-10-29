// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/stats/stats_cache.h"

#include "mongo/base/error_codes.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/read_through_cache.h"

#include <utility>

#include <boost/none.hpp>


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::stats {

StatsCache::StatsCache(Service* service,
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
        tassert(11051901, "Expecting stats cache loader to be provided", _statsCacheLoader);
        auto newStats = _statsCacheLoader->getStats(opCtx, statsPath).get();
        return LookupResult(std::move(newStats));
    } catch (const DBException& ex) {
        if (ex.code() == ErrorCodes::NamespaceNotFound) {
            return StatsCache::LookupResult(boost::none);
        }
        throw;
    }
}

}  // namespace mongo::stats
