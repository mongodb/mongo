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
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/compiler/stats/ce_histogram.h"
#include "mongo/db/query/compiler/stats/stats_cache_loader_mock.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/thread_pool.h"

#include <cstddef>
#include <string>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo::stats {
namespace {


/**
 * Fixture for tests, which do not need to exercise the multi-threading capabilities of the cache
 * and as such do not require control over the creation/destruction of their operation contexts.
 */
class StatsCacheTest : public ServiceContextTest {
protected:
    // Extends StatsCache and automatically provides it with a thread  pool, which will be
    // shutdown and joined before the StatsCache is destroyed (which is part of the  contract of
    // ReadThroughCache)
    class CacheWithThreadPool : public StatsCache {
    public:
        CacheWithThreadPool(ServiceContext* service,
                            std::unique_ptr<StatsCacheLoader> cacheLoaderMock,
                            size_t size)
            : StatsCache(service->getService(ClusterRole::ShardServer),
                         std::move(cacheLoaderMock),
                         _threadPool,
                         size) {
            _threadPool.startup();
        }

    private:
        ThreadPool _threadPool{[] {
            ThreadPool::Options options;
            options.poolName = "StatsCacheTest";
            options.minThreads = 1;
            options.maxThreads = 1;
            return options;
        }()};
    };

    const ServiceContext::UniqueOperationContext _opCtxHolder{makeOperationContext()};
    OperationContext* const _opCtx{_opCtxHolder.get()};
};

TEST(SimpleStatsCacheTest, StandaloneValueHandle) {
    StatsCacheVal statsPtr(CEHistogram::make());
    StatsCache::ValueHandle standaloneHandle(std::move(statsPtr));
    ASSERT(standaloneHandle.isValid());
}

TEST_F(StatsCacheTest, KeyDoesNotExist) {
    Status namespaceNotFoundErrorStatus = {ErrorCodes::NamespaceNotFound,
                                           "The key does not exists"};
    auto cacheLoaderMock = std::make_unique<StatsCacheLoaderMock>();
    auto cache = CacheWithThreadPool(getServiceContext(), std::move(cacheLoaderMock), 1);
    cache.getStatsCacheLoader()->setStatsReturnValueForTest(
        std::move(namespaceNotFoundErrorStatus));
    auto handle = cache.acquire(
        _opCtx,
        std::make_pair(NamespaceString::createNamespaceString_forTest("db", "coll"), "somePath"));
    ASSERT(!handle);
}

/*
TEST_F(StatsCacheTest, LoadStats) {
    auto cacheLoaderMock = std::make_unique<StatsCacheLoaderMock>();
    auto cache = CacheWithThreadPool(getServiceContext(), std::move(cacheLoaderMock), 1);

    auto stats1 = CollectionStatistics(1);
    auto stats2 = CollectionStatistics(2);

    cache.getStatsCacheLoader()->setStatsReturnValueForTest(std::move(stats1));

    auto handle = cache.acquire(_opCtx, NamespaceString::createNamespaceString_forTest("db",
"coll1")); ASSERT(handle.isValid()); ASSERT_EQ(1, handle->getCardinality());

    // Make all requests to StatsCacheLoader to throw an exception to ensre that test returns value
    // from cache.
    Status internalErrorStatus = {ErrorCodes::InternalError,
                                  "Stats cache loader received unexpected request"};
    cache.getStatsCacheLoader()->setStatsReturnValueForTest(std::move(internalErrorStatus));

    handle = cache.acquire(_opCtx, NamespaceString::createNamespaceString_forTest("db", "coll1"));
    ASSERT(handle.isValid());
    ASSERT_EQ(1, handle->getCardinality());

    cache.getStatsCacheLoader()->setStatsReturnValueForTest(std::move(stats2));
    handle = cache.acquire(_opCtx, NamespaceString::createNamespaceString_forTest("db", "coll2"));
    ASSERT(handle.isValid());
    ASSERT_EQ(2, handle->getCardinality());
}
*/

}  // namespace
}  // namespace mongo::stats
