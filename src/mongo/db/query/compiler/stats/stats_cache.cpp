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

#include "mongo/db/query/compiler/stats/stats_cache.h"

#include "mongo/base/error_codes.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/read_through_cache.h"

#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

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

}  // namespace mongo::stats
