/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/smart_ptr.hpp>
#include <cstddef>
#include <fmt/format.h>
#include <list>
#include <string>

#include <boost/optional/optional.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/duration.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/read_through_cache.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

// The structure for testing is intentionally made movable, but non-copyable
struct CachedValue {
    CachedValue(int counter) : counter(counter) {}
    CachedValue(CachedValue&&) = default;
    CachedValue& operator=(CachedValue&&) = default;

    int counter;
};

class Cache : public ReadThroughCache<std::string, CachedValue> {
public:
    Cache(Service* service, ThreadPoolInterface& threadPool, size_t size, LookupFn lookupFn)
        : ReadThroughCache(
              _mutex,
              service,
              threadPool,
              [this, lookupFn = std::move(lookupFn)](
                  OperationContext* opCtx, const std::string& key, const ValueHandle& cachedValue) {
                  ++countLookups;
                  return lookupFn(opCtx, key, cachedValue);
              },
              size) {}

    int countLookups{0};

private:
    stdx::mutex _mutex;
};

class CausallyConsistentCache : public ReadThroughCache<std::string, CachedValue, Timestamp> {
public:
    CausallyConsistentCache(Service* service,
                            ThreadPoolInterface& threadPool,
                            size_t size,
                            LookupFn lookupFn)
        : ReadThroughCache(
              _mutex,
              service,
              threadPool,
              [this, lookupFn = std::move(lookupFn)](OperationContext* opCtx,
                                                     const std::string& key,
                                                     const ValueHandle& cachedValue,
                                                     Timestamp timeInStore) {
                  ++countLookups;
                  return lookupFn(opCtx, key, cachedValue, timeInStore);
              },
              size) {}

    int countLookups{0};

private:
    stdx::mutex _mutex;
};

class CausallyConsistentCacheWithLookupArgs
    : public ReadThroughCache<std::string, CachedValue, Timestamp, std::string, int> {
public:
    CausallyConsistentCacheWithLookupArgs(Service* service,
                                          ThreadPoolInterface& threadPool,
                                          size_t size,
                                          LookupFn lookupFn)
        : ReadThroughCache(
              _mutex,
              service,
              threadPool,
              [this, lookupFn = std::move(lookupFn)](OperationContext* opCtx,
                                                     const std::string& key,
                                                     const ValueHandle& cachedValue,
                                                     Timestamp timeInStore,
                                                     const std::string& name,
                                                     int val) {
                  ++countLookups;
                  return lookupFn(opCtx, key, cachedValue, timeInStore, name, val);
              },
              size) {}

    int countLookups{0};

private:
    stdx::mutex _mutex;
};

/**
 * Fixture for tests, which do not need to exercise the multi-threading capabilities of the cache
 * and as such do not require control over the creation/destruction of their operation contexts.
 */
class ReadThroughCacheTest : public ServiceContextTest {
protected:
    // Extends any of Cache/CausallyConsistentCache and automatically provides it with a thread
    // pool, which will be shutdown and joined before the Cache is destroyed (which is part of the
    // contract of ReadThroughCache)
    template <class T>
    class CacheWithThreadPool : public T {
    public:
        CacheWithThreadPool(Service* service, size_t size, typename T::LookupFn lookupFn)
            : T(service, _threadPool, size, std::move(lookupFn)) {
            _threadPool.startup();
        }

    private:
        ThreadPool _threadPool{[] {
            ThreadPool::Options options;
            options.poolName = "ReadThroughCacheTest";
            options.minThreads = 1;
            options.maxThreads = 1;
            return options;
        }()};
    };

    const ServiceContext::UniqueOperationContext _opCtxHolder{makeOperationContext()};
    OperationContext* const _opCtx{_opCtxHolder.get()};
};

TEST(ReadThroughCacheTest, StandaloneValueHandle) {
    Cache::ValueHandle standaloneHandle(CachedValue(100));
    ASSERT(standaloneHandle.isValid());
    ASSERT_EQ(100, standaloneHandle->counter);
}

TEST_F(ReadThroughCacheTest, FetchInvalidateAndRefetch) {
    auto fnTest = [&](auto cache) {
        for (int i = 1; i <= 3; i++) {
            auto value = cache.acquire(_opCtx, "TestKey");
            ASSERT(value);
            ASSERT_EQ(100 * i, value->counter);
            ASSERT_EQ(i, cache.countLookups);

            ASSERT(cache.acquire(_opCtx, "TestKey"));
            ASSERT_EQ(i, cache.countLookups);

            cache.invalidateKey("TestKey");
        }
    };

    fnTest(CacheWithThreadPool<Cache>(
        getService(),
        1,
        [&, nextValue = 0](
            OperationContext*, const std::string& key, const Cache::ValueHandle&) mutable {
            ASSERT_EQ("TestKey", key);
            return Cache::LookupResult(CachedValue(100 * ++nextValue));
        }));

    fnTest(CacheWithThreadPool<CausallyConsistentCache>(
        getService(),
        1,
        [&, nextValue = 0](OperationContext*,
                           const std::string& key,
                           const CausallyConsistentCache::ValueHandle&,
                           const Timestamp& timeInStore) mutable {
            ASSERT_EQ("TestKey", key);
            ++nextValue;
            return CausallyConsistentCache::LookupResult(CachedValue(100 * nextValue),
                                                         Timestamp(nextValue));
        }));
}

TEST_F(ReadThroughCacheTest, FetchInvalidateKeyAndRefetch) {
    auto fnTest = [&](auto cache) {
        for (int i = 1; i <= 3; i++) {
            auto value = cache.acquire(_opCtx, "TestKey");
            ASSERT(value);
            ASSERT_EQ(100 * i, value->counter);
            ASSERT_EQ(i, cache.countLookups);

            ASSERT(cache.acquire(_opCtx, "TestKey"));
            ASSERT_EQ(i, cache.countLookups);

            cache.invalidateKeyIf([](const std::string& key) { return key == "TestKey"; });
        }
    };

    fnTest(CacheWithThreadPool<Cache>(
        getService(),
        1,
        [&, nextValue = 0](
            OperationContext*, const std::string& key, const Cache::ValueHandle&) mutable {
            ASSERT_EQ("TestKey", key);
            return Cache::LookupResult(CachedValue(100 * ++nextValue));
        }));

    fnTest(CacheWithThreadPool<CausallyConsistentCache>(
        getService(),
        1,
        [&, nextValue = 0](OperationContext*,
                           const std::string& key,
                           const CausallyConsistentCache::ValueHandle&,
                           const Timestamp& timeInStore) mutable {
            ASSERT_EQ("TestKey", key);
            ++nextValue;
            return CausallyConsistentCache::LookupResult(CachedValue(100 * nextValue),
                                                         Timestamp(nextValue));
        }));
}

TEST_F(ReadThroughCacheTest, FetchInvalidateValueAndRefetch) {
    auto fnTest = [&](auto cache) {
        for (int i = 1; i <= 3; i++) {
            auto value = cache.acquire(_opCtx, "TestKey");
            ASSERT(value);
            ASSERT_EQ(100 * i, value->counter);
            ASSERT_EQ(i, cache.countLookups);

            ASSERT(cache.acquire(_opCtx, "TestKey"));
            ASSERT_EQ(i, cache.countLookups);

            cache.invalidateLatestCachedValueIf_IgnoreInProgress(
                [i](const std::string&, const CachedValue& value) {
                    return value.counter == 100 * i;
                });
        }
    };

    fnTest(CacheWithThreadPool<Cache>(
        getService(),
        1,
        [&, nextValue = 0](
            OperationContext*, const std::string& key, const Cache::ValueHandle&) mutable {
            ASSERT_EQ("TestKey", key);
            return Cache::LookupResult(CachedValue(100 * ++nextValue));
        }));

    fnTest(CacheWithThreadPool<CausallyConsistentCache>(
        getService(),
        1,
        [&, nextValue = 0](OperationContext*,
                           const std::string& key,
                           const CausallyConsistentCache::ValueHandle&,
                           const Timestamp& timeInStore) mutable {
            ASSERT_EQ("TestKey", key);
            ++nextValue;
            return CausallyConsistentCache::LookupResult(CachedValue(100 * nextValue),
                                                         Timestamp(nextValue));
        }));
}

TEST_F(ReadThroughCacheTest, FailedLookup) {
    auto fnTest = [&](auto cache) {
        ASSERT_THROWS_CODE(
            cache.acquire(_opCtx, "TestKey"), DBException, ErrorCodes::InternalError);
    };

    fnTest(CacheWithThreadPool<Cache>(
        getService(),
        1,
        [&](OperationContext*, const std::string& key, const Cache::ValueHandle&)
            -> Cache::LookupResult { uasserted(ErrorCodes::InternalError, "Test error"); }));

    fnTest(CacheWithThreadPool<CausallyConsistentCache>(
        getService(),
        1,
        [&](OperationContext*,
            const std::string& key,
            const CausallyConsistentCache::ValueHandle&,
            const Timestamp& timeInStore) -> CausallyConsistentCache::LookupResult {
            uasserted(ErrorCodes::InternalError, "Test error");
        }));
}

TEST_F(ReadThroughCacheTest, InvalidateCacheSizeZeroReissuesLookup) {
    auto fnTest = [&](auto cache) {
        auto value = cache.acquire(_opCtx, "TestKey");
        ASSERT(value);
        ASSERT_EQ(1000, value->counter);
        ASSERT_EQ(1, cache.countLookups);

        // Because 'value' above is held alive, the cache will not perform lookup until it is
        // destroyed
        ASSERT_EQ(1000, cache.acquire(_opCtx, "TestKey")->counter);
        ASSERT_EQ(1, cache.countLookups);

        cache.invalidateKey("TestKey");
        auto valueAfterInvalidate = cache.acquire(_opCtx, "TestKey");
        ASSERT(!value.isValid());
        ASSERT(valueAfterInvalidate);
        ASSERT_EQ(2000, valueAfterInvalidate->counter);
        ASSERT_EQ(2, cache.countLookups);
    };

    fnTest(CacheWithThreadPool<Cache>(
        getService(),
        0,
        [&, nextValue = 0](
            OperationContext*, const std::string& key, const Cache::ValueHandle&) mutable {
            ASSERT_EQ("TestKey", key);
            return Cache::LookupResult(CachedValue(1000 * ++nextValue));
        }));

    fnTest(CacheWithThreadPool<CausallyConsistentCache>(
        getService(),
        0,
        [&, nextValue = 0](OperationContext*,
                           const std::string& key,
                           const CausallyConsistentCache::ValueHandle&,
                           const Timestamp& timeInStore) mutable {
            ASSERT_EQ("TestKey", key);
            ++nextValue;
            return CausallyConsistentCache::LookupResult(CachedValue(1000 * nextValue),
                                                         Timestamp(nextValue));
        }));
}

TEST_F(ReadThroughCacheTest, KeyDoesNotExist) {
    auto fnTest = [&](auto cache) {
        ASSERT(!cache.acquire(_opCtx, "TestKey"));
    };

    fnTest(CacheWithThreadPool<Cache>(
        getService(), 1, [&](OperationContext*, const std::string& key, const Cache::ValueHandle&) {
            ASSERT_EQ("TestKey", key);
            return Cache::LookupResult(boost::none);
        }));

    fnTest(CacheWithThreadPool<CausallyConsistentCache>(
        getService(),
        1,
        [&](OperationContext*,
            const std::string& key,
            const CausallyConsistentCache::ValueHandle&,
            const Timestamp& timeInStore) {
            ASSERT_EQ("TestKey", key);
            return CausallyConsistentCache::LookupResult(boost::none, Timestamp(10));
        }));
}

TEST_F(ReadThroughCacheTest, CausalConsistency) {
    boost::optional<CausallyConsistentCache::LookupResult> nextToReturn;
    CacheWithThreadPool<CausallyConsistentCache> cache(
        getService(),
        1,
        [&](OperationContext*,
            const std::string& key,
            const CausallyConsistentCache::ValueHandle&,
            const Timestamp& timeInStore) {
            ASSERT_EQ("TestKey", key);
            return CausallyConsistentCache::LookupResult(std::move(*nextToReturn));
        });

    nextToReturn.emplace(CachedValue(10), Timestamp(10));
    ASSERT_EQ(10, cache.acquire(_opCtx, "TestKey", CacheCausalConsistency::kLatestCached)->counter);
    ASSERT_EQ(10, cache.acquire(_opCtx, "TestKey", CacheCausalConsistency::kLatestKnown)->counter);

    nextToReturn.emplace(CachedValue(20), Timestamp(20));
    ASSERT(cache.advanceTimeInStore("TestKey", Timestamp(20)));
    ASSERT_EQ(10, cache.acquire(_opCtx, "TestKey", CacheCausalConsistency::kLatestCached)->counter);
    ASSERT(!cache.acquire(_opCtx, "TestKey", CacheCausalConsistency::kLatestCached).isValid());
    ASSERT_EQ(20, cache.acquire(_opCtx, "TestKey", CacheCausalConsistency::kLatestKnown)->counter);
    ASSERT(cache.acquire(_opCtx, "TestKey", CacheCausalConsistency::kLatestKnown).isValid());
}

TEST_F(ReadThroughCacheTest, CausalConsistencyWithLookupArgs) {
    boost::optional<CausallyConsistentCacheWithLookupArgs::LookupResult> nextToReturn;

    CacheWithThreadPool<CausallyConsistentCacheWithLookupArgs> cache(
        getService(),
        1,
        [&](OperationContext*,
            const std::string& key,
            const CausallyConsistentCacheWithLookupArgs::ValueHandle&,
            const Timestamp& timeInStore,
            const std::string& name,
            int val) {
            ASSERT_EQ("TestKey", key);
            ASSERT_EQ("hello", name);
            ASSERT_GTE(val, 0);
            return CausallyConsistentCacheWithLookupArgs::LookupResult(std::move(*nextToReturn));
        });

    constexpr auto kName = "hello";

    nextToReturn.emplace(CachedValue(10), Timestamp(10));
    ASSERT_EQ(10,
              cache.acquire(_opCtx, "TestKey", CacheCausalConsistency::kLatestCached, kName, 16)
                  ->counter);
    ASSERT_EQ(
        10,
        cache.acquire(_opCtx, "TestKey", CacheCausalConsistency::kLatestKnown, kName, 12)->counter);

    nextToReturn.emplace(CachedValue(20), Timestamp(20));
    ASSERT(cache.advanceTimeInStore("TestKey", Timestamp(20)));
    ASSERT_EQ(
        10,
        cache.acquire(_opCtx, "TestKey", CacheCausalConsistency::kLatestCached, kName, 0)->counter);
    ASSERT(!cache.acquire(_opCtx, "TestKey", CacheCausalConsistency::kLatestCached, kName, 17)
                .isValid());
    ASSERT_EQ(
        20,
        cache.acquire(_opCtx, "TestKey", CacheCausalConsistency::kLatestKnown, kName, 39)->counter);
    ASSERT(
        cache.acquire(_opCtx, "TestKey", CacheCausalConsistency::kLatestKnown, kName, 2).isValid());
}

/**
 * Fixture for tests, which need to control the creation/destruction of their operation contexts.
 */
class ReadThroughCacheAsyncTest : public unittest::Test,
                                  public ScopedGlobalServiceContextForTest {};

using Barrier = unittest::Barrier;

TEST_F(ReadThroughCacheAsyncTest, SuccessfulInProgressLookupForNotCausallyConsistentCache) {
    ThreadPool threadPool{ThreadPool::Options()};
    threadPool.startup();

    Cache cache(getService(),
                threadPool,
                1,
                [&](OperationContext*, const std::string& key, const Cache::ValueHandle&) {
                    return Cache::LookupResult(CachedValue(500));
                });

    // Join threads before destroying cache. This ensure the internal asynchronous processing tasks
    // are completed before the cache resources are released.
    ON_BLOCK_EXIT([&] {
        threadPool.shutdown();
        threadPool.join();
    });

    Cache::InProgressLookup inProgress(
        cache, "TestKey", Cache::ValueHandle(), CacheNotCausallyConsistent());
    auto future = inProgress.addWaiter(WithLock::withoutLock());
    ASSERT(!future.isReady());

    auto res = inProgress.asyncLookupRound().get();
    ASSERT(inProgress.valid(WithLock::withoutLock()));
    ASSERT(res.v);
    ASSERT_EQ(500, res.v->counter);
    auto promisesToSet = inProgress.getPromisesLessThanOrEqualToTime(WithLock::withoutLock(),
                                                                     CacheNotCausallyConsistent());
    ASSERT_EQ(1U, promisesToSet.size());
    promisesToSet.front()->emplaceValue(std::move(*res.v));

    ASSERT(future.isReady());
    ASSERT_EQ(500, future.get()->counter);
}

TEST_F(ReadThroughCacheAsyncTest, FailedInProgressLookupForNotCausallyConsistentCache) {
    ThreadPool threadPool{ThreadPool::Options()};
    threadPool.startup();

    Cache cache(getService(),
                threadPool,
                1,
                [&](OperationContext*, const std::string& key, const Cache::ValueHandle&)
                    -> Cache::LookupResult { uasserted(ErrorCodes::InternalError, "Test error"); });

    // Join threads before destroying cache. This ensure the internal asynchronous processing tasks
    // are completed before the cache resources are released.
    ON_BLOCK_EXIT([&] {
        threadPool.shutdown();
        threadPool.join();
    });

    Cache::InProgressLookup inProgress(
        cache, "TestKey", Cache::ValueHandle(), CacheNotCausallyConsistent());
    auto future = inProgress.addWaiter(WithLock::withoutLock());
    ASSERT(!future.isReady());

    auto asyncLookupResult = inProgress.asyncLookupRound().getNoThrow();
    ASSERT_THROWS_CODE(inProgress.asyncLookupRound().get(), DBException, ErrorCodes::InternalError);
    ASSERT(inProgress.valid(WithLock::withoutLock()));
    auto promisesToSet = inProgress.getAllPromisesOnError(WithLock::withoutLock());
    ASSERT_EQ(1U, promisesToSet.size());
    promisesToSet.front()->setError(asyncLookupResult.getStatus());

    ASSERT(future.isReady());
    ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::InternalError);
}

TEST_F(ReadThroughCacheAsyncTest, AcquireObservesOperationContextDeadline) {
    ThreadPool threadPool{ThreadPool::Options()};
    threadPool.startup();

    Barrier lookupStartedBarrier(2);
    Barrier completeLookupBarrier(2);
    Cache cache(getService(),
                threadPool,
                1,
                [&](OperationContext*, const std::string& key, const Cache::ValueHandle&) {
                    lookupStartedBarrier.countDownAndWait();
                    completeLookupBarrier.countDownAndWait();
                    return Cache::LookupResult(CachedValue(5));
                });

    // Join threads before destroying cache. This ensure the internal asynchronous processing tasks
    // are completed before the cache resources are released.
    ON_BLOCK_EXIT([&] {
        threadPool.shutdown();
        threadPool.join();
    });

    {
        ThreadClient tc(getService());
        const ServiceContext::UniqueOperationContext opCtxHolder{tc->makeOperationContext()};
        OperationContext* const opCtx{opCtxHolder.get()};

        opCtx->setDeadlineAfterNowBy(Milliseconds{5}, ErrorCodes::ExceededTimeLimit);
        ASSERT_THROWS_CODE(
            cache.acquire(opCtx, "TestKey"), DBException, ErrorCodes::ExceededTimeLimit);

        lookupStartedBarrier.countDownAndWait();
    }

    completeLookupBarrier.countDownAndWait();

    {
        ThreadClient tc(getService());
        const ServiceContext::UniqueOperationContext opCtxHolder{tc->makeOperationContext()};
        OperationContext* const opCtx{opCtxHolder.get()};

        auto value = cache.acquire(opCtx, "TestKey");
        ASSERT(value);
        ASSERT_EQ(5, value->counter);
    }

    {
        ThreadClient tc(getService());
        const ServiceContext::UniqueOperationContext opCtxHolder{tc->makeOperationContext()};
        OperationContext* const opCtx{opCtxHolder.get()};

        opCtx->setDeadlineAfterNowBy(Milliseconds{0}, ErrorCodes::ExceededTimeLimit);
        auto value = cache.acquire(opCtx, "TestKey");
        ASSERT(value);
        ASSERT_EQ(5, value->counter);
    }
}

TEST_F(ReadThroughCacheAsyncTest, InvalidateReissuesLookup) {
    ThreadPool threadPool{ThreadPool::Options()};
    threadPool.startup();

    AtomicWord<int> countLookups(0);
    Barrier lookupStartedBarriers[] = {Barrier{2}, Barrier{2}, Barrier{2}};
    Barrier completeLookupBarriers[] = {Barrier{2}, Barrier{2}, Barrier{2}};

    Cache cache(getService(),
                threadPool,
                1,
                [&](OperationContext* opCtx, const std::string&, const Cache::ValueHandle&) {
                    int idx = countLookups.fetchAndAdd(1);
                    lookupStartedBarriers[idx].countDownAndWait();
                    completeLookupBarriers[idx].countDownAndWait();

                    if (idx < 2) {
                        ASSERT_THROWS_CODE(opCtx->checkForInterrupt(),
                                           DBException,
                                           ErrorCodes::ReadThroughCacheLookupCanceled);
                    } else {
                        opCtx->checkForInterrupt();
                    }

                    return Cache::LookupResult(CachedValue(idx));
                });

    // Join threads before destroying cache. This ensure the internal asynchronous processing tasks
    // are completed before the cache resources are released.
    ON_BLOCK_EXIT([&] {
        threadPool.shutdown();
        threadPool.join();
    });

    // Kick off the first lookup, which will block
    auto future = cache.acquireAsync("TestKey");
    ASSERT(!future.isReady());

    // Wait for the first lookup attempt to start and invalidate it before letting it proceed
    lookupStartedBarriers[0].countDownAndWait();
    ASSERT_EQ(1, countLookups.load());
    cache.invalidateKey("TestKey");
    ASSERT(!future.isReady());
    completeLookupBarriers[0].countDownAndWait();  // Lets lookup attempt 1 proceed
    ASSERT(!future.isReady());

    // Wait for the second lookup attempt to start and invalidate it before letting it proceed
    lookupStartedBarriers[1].countDownAndWait();
    ASSERT_EQ(2, countLookups.load());
    cache.invalidateKey("TestKey");
    ASSERT(!future.isReady());
    completeLookupBarriers[1].countDownAndWait();  // Lets lookup attempt 2 proceed
    ASSERT(!future.isReady());

    // Wait for the third lookup attempt to start, but not do not invalidate it before letting it
    // proceed (end of test)
    lookupStartedBarriers[2].countDownAndWait();
    ASSERT_EQ(3, countLookups.load());
    ASSERT(!future.isReady());
    completeLookupBarriers[2].countDownAndWait();  // Lets lookup attempt 3 proceed

    ASSERT_EQ(2, future.get()->counter);
}

TEST_F(ReadThroughCacheAsyncTest, AcquireWithAShutdownThreadPool) {
    ThreadPool threadPool{ThreadPool::Options()};
    threadPool.startup();
    threadPool.shutdown();
    threadPool.join();

    Cache cache(getService(),
                threadPool,
                1,
                [&](OperationContext*, const std::string&, const Cache::ValueHandle&) {
                    FAIL("Should not be called");
                    return Cache::LookupResult(boost::none);  // Will never be reached
                });

    auto future = cache.acquireAsync("TestKey");
    ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::ShutdownInProgress);
}

TEST_F(ReadThroughCacheAsyncTest, ShutdownWithConcurrentInvalidate) {
    ThreadPool threadPool{ThreadPool::Options()};
    Barrier lookupStartedBarrier(2);
    Barrier completeLookupBarrier(2);

    threadPool.startup();

    Cache cache(getService(),
                threadPool,
                1,
                [&](OperationContext*, const std::string&, const Cache::ValueHandle&) {
                    // Wait until noticed
                    lookupStartedBarrier.countDownAndWait();
                    completeLookupBarrier.countDownAndWait();
                    uasserted(ErrorCodes::InterruptedAtShutdown, "Interrupted at shutdown");
                    return Cache::LookupResult(boost::none);
                });

    // Join threads before destroying cache. This ensure the internal asynchronous processing tasks
    // are completed before the cache resources are released.
    ON_BLOCK_EXIT([&] {
        threadPool.shutdown();
        threadPool.join();
    });

    auto future = cache.acquireAsync("async", CacheCausalConsistency::kLatestCached);

    lookupStartedBarrier.countDownAndWait();
    cache.invalidateKey("async");
    completeLookupBarrier.countDownAndWait();

    ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::InterruptedAtShutdown);
}

class MockThreadPool : public ThreadPoolInterface {
public:
    ~MockThreadPool() override {
        ASSERT(!_mostRecentTask);
    }
    void startup() override {}
    void shutdown() override {}
    void join() override {}
    void schedule(Task task) override {
        ASSERT(!_mostRecentTask);
        _mostRecentTask = std::move(task);
    }
    void runMostRecentTask() {
        ASSERT(_mostRecentTask);
        auto f = std::move(_mostRecentTask);
        f(Status::OK());
    }

private:
    Task _mostRecentTask;
};

TEST_F(ReadThroughCacheAsyncTest, AdvanceTimeDuringLookupOfUnCachedKey) {
    MockThreadPool threadPool;
    boost::optional<CausallyConsistentCache::LookupResult> nextToReturn;
    CausallyConsistentCache cache(getService(),
                                  threadPool,
                                  1,
                                  [&](OperationContext*,
                                      const std::string&,
                                      const CausallyConsistentCache::ValueHandle&,
                                      const Timestamp&) { return std::move(*nextToReturn); });

    auto futureAtTS100 = cache.acquireAsync("TestKey", CacheCausalConsistency::kLatestKnown);
    ASSERT(!futureAtTS100.isReady());

    ASSERT(cache.advanceTimeInStore("TestKey", Timestamp(200)));
    auto futureAtTS200 = cache.acquireAsync("TestKey", CacheCausalConsistency::kLatestKnown);
    ASSERT(!futureAtTS200.isReady());

    nextToReturn.emplace(CachedValue(100), Timestamp(100));
    threadPool.runMostRecentTask();
    ASSERT_EQ(100, futureAtTS100.get()->counter);
    ASSERT(!futureAtTS100.get().isValid());
    ASSERT(!futureAtTS200.isReady());

    nextToReturn.emplace(CachedValue(200), Timestamp(200));
    threadPool.runMostRecentTask();
    ASSERT_EQ(200, futureAtTS200.get()->counter);
    ASSERT(futureAtTS200.get().isValid());
}

TEST_F(ReadThroughCacheAsyncTest, KeyDeletedAfterAdvanceTimeInStore) {
    MockThreadPool threadPool;
    boost::optional<CausallyConsistentCache::LookupResult> nextToReturn;
    CausallyConsistentCache cache(getService(),
                                  threadPool,
                                  1,
                                  [&](OperationContext*,
                                      const std::string&,
                                      const CausallyConsistentCache::ValueHandle&,
                                      const Timestamp&) { return std::move(*nextToReturn); });

    auto futureAtTS100 = cache.acquireAsync("TestKey", CacheCausalConsistency::kLatestKnown);
    nextToReturn.emplace(CachedValue(100), Timestamp(100));
    threadPool.runMostRecentTask();
    ASSERT_EQ(100, futureAtTS100.get()->counter);
    ASSERT(futureAtTS100.get().isValid());

    ASSERT(cache.advanceTimeInStore("TestKey", Timestamp(200)));
    auto futureAtTS200 = cache.acquireAsync("TestKey", CacheCausalConsistency::kLatestKnown);

    nextToReturn.emplace(boost::none, Timestamp(200));
    threadPool.runMostRecentTask();
    ASSERT(!futureAtTS100.get().isValid());
    ASSERT(!futureAtTS200.get());

    auto futureAtTS300 = cache.acquireAsync("TestKey", CacheCausalConsistency::kLatestCached);

    nextToReturn.emplace(boost::none, Timestamp(300));
    threadPool.runMostRecentTask();
    ASSERT(!futureAtTS300.get());
}

TEST_F(ReadThroughCacheAsyncTest, AcquireAsyncAndAdvanceTimeInterleave) {
    MockThreadPool threadPool;
    boost::optional<CausallyConsistentCache::LookupResult> nextToReturn;
    CausallyConsistentCache cache(getService(),
                                  threadPool,
                                  1,
                                  [&](OperationContext*,
                                      const std::string&,
                                      const CausallyConsistentCache::ValueHandle&,
                                      const Timestamp&) { return std::move(*nextToReturn); });

    auto futureAtTS100 = cache.acquireAsync("TestKey");
    nextToReturn.emplace(CachedValue(100), Timestamp(100));
    threadPool.runMostRecentTask();
    ASSERT_EQ(100, futureAtTS100.get()->counter);
    ASSERT(futureAtTS100.get().isValid());

    ASSERT(cache.advanceTimeInStore("TestKey", Timestamp(150)));
    auto futureAtTS150 = cache.acquireAsync("TestKey", CacheCausalConsistency::kLatestKnown);
    ASSERT(!futureAtTS100.get().isValid());
    ASSERT(!futureAtTS150.isReady());

    ASSERT(cache.advanceTimeInStore("TestKey", Timestamp(250)));
    auto futureAtTS250 = cache.acquireAsync("TestKey", CacheCausalConsistency::kLatestKnown);
    ASSERT(!futureAtTS100.get().isValid());
    ASSERT(!futureAtTS150.isReady());
    ASSERT(!futureAtTS250.isReady());

    nextToReturn.emplace(CachedValue(150), Timestamp(150));
    threadPool.runMostRecentTask();
    ASSERT_EQ(150, futureAtTS150.get()->counter);
    ASSERT(!futureAtTS150.get().isValid());
    ASSERT(!futureAtTS250.isReady());

    nextToReturn.emplace(CachedValue(250), Timestamp(250));
    threadPool.runMostRecentTask();
    ASSERT_EQ(250, futureAtTS250.get()->counter);
    ASSERT(futureAtTS250.get().isValid());
}

TEST_F(ReadThroughCacheAsyncTest, InvalidateCalledBeforeLookupTaskExecutes) {
    MockThreadPool threadPool;
    Cache cache(getService(),
                threadPool,
                1,
                [&](OperationContext*, const std::string&, const Cache::ValueHandle&) {
                    return Cache::LookupResult(CachedValue(123));
                });

    auto future = cache.acquireAsync("TestKey");
    cache.invalidateAll();

    ASSERT(!future.isReady());
    threadPool.runMostRecentTask();

    ASSERT(!future.isReady());
    threadPool.runMostRecentTask();

    ASSERT_EQ(123, future.get()->counter);
}

TEST_F(ReadThroughCacheAsyncTest, CacheSizeZero) {
    MockThreadPool threadPool;
    auto fnTest = [&](auto cache) {
        for (int i = 1; i <= 3; i++) {
            auto future = cache.acquireAsync("TestKey", CacheCausalConsistency::kLatestKnown);
            threadPool.runMostRecentTask();
            auto value = future.get();
            ASSERT(value);
            ASSERT_EQ(100 * i, value->counter);
            ASSERT_EQ(i, cache.countLookups);
        }
    };

    fnTest(Cache(getService(),
                 threadPool,
                 0,
                 [&, nextValue = 0](
                     OperationContext*, const std::string& key, const Cache::ValueHandle&) mutable {
                     ASSERT_EQ("TestKey", key);
                     return Cache::LookupResult(CachedValue(100 * ++nextValue));
                 }));

    fnTest(CausallyConsistentCache(getService(),
                                   threadPool,
                                   0,
                                   [&, nextValue = 0](OperationContext*,
                                                      const std::string& key,
                                                      const CausallyConsistentCache::ValueHandle&,
                                                      const Timestamp&) mutable {
                                       ASSERT_EQ("TestKey", key);
                                       ++nextValue;
                                       return CausallyConsistentCache::LookupResult(
                                           CachedValue(100 * nextValue), Timestamp(nextValue));
                                   }));
}

}  // namespace
}  // namespace mongo
