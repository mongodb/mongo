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

#include "mongo/platform/basic.h"

#include <string>

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/read_through_cache.h"

namespace mongo {
namespace {

using unittest::assertGet;

// The structure for testing is intentionally made movable, but non-copyable
struct CachedValue {
    CachedValue(int counter) : counter(counter) {}
    CachedValue(CachedValue&&) = default;
    CachedValue& operator=(CachedValue&&) = default;

    int counter;
};

class Cache : public ReadThroughCache<std::string, CachedValue> {
public:
    Cache(ServiceContext* service, ThreadPoolInterface& threadPool, size_t size, LookupFn lookupFn)
        : ReadThroughCache(_mutex, service, threadPool, std::move(lookupFn), size) {}

private:
    Mutex _mutex = MONGO_MAKE_LATCH("ReadThroughCacheTest::Cache");
};

/**
 * Fixture for tests, which do not need to exercise the multi-threading capabilities of the cache
 * and as such do not require control over the creation/destruction of their operation contexts.
 */
class ReadThroughCacheTest : public ServiceContextTest {
protected:
    // Extends Cache and automatically provides it with a thread pool, which will be shutdown and
    // joined before the Cache is destroyed (which is part of the contract of ReadThroughCache)
    class CacheWithThreadPool : public Cache {
    public:
        CacheWithThreadPool(ServiceContext* service, size_t size, LookupFn lookupFn)
            : Cache(service, _threadPool, size, std::move(lookupFn)) {
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
    int countLookups = 0;
    CacheWithThreadPool cache(
        getServiceContext(), 1, [&](OperationContext*, const std::string& key) {
            ASSERT_EQ("TestKey", key);
            countLookups++;
            return Cache::LookupResult(CachedValue(100 * countLookups));
        });

    for (int i = 1; i <= 3; i++) {
        auto value = cache.acquire(_opCtx, "TestKey");
        ASSERT(value);
        ASSERT_EQ(100 * i, value->counter);
        ASSERT_EQ(i, countLookups);

        ASSERT(cache.acquire(_opCtx, "TestKey"));
        ASSERT_EQ(i, countLookups);

        cache.invalidate("TestKey");
    }
}

TEST_F(ReadThroughCacheTest, FailedLookup) {
    CacheWithThreadPool cache(
        getServiceContext(),
        1,
        [&](OperationContext*, const std::string& key) -> Cache::LookupResult {
            uasserted(ErrorCodes::InternalError, "Test error");
        });

    ASSERT_THROWS_CODE(cache.acquire(_opCtx, "TestKey"), DBException, ErrorCodes::InternalError);
}

TEST_F(ReadThroughCacheTest, CacheSizeZero) {
    int countLookups = 0;
    CacheWithThreadPool cache(
        getServiceContext(), 0, [&](OperationContext*, const std::string& key) {
            ASSERT_EQ("TestKey", key);
            countLookups++;
            return Cache::LookupResult(CachedValue(100 * countLookups));
        });

    for (int i = 1; i <= 3; i++) {
        auto value = cache.acquire(_opCtx, "TestKey");
        ASSERT(value);
        ASSERT_EQ(100 * i, value->counter);
        ASSERT_EQ(i, countLookups);
    }
}

TEST_F(ReadThroughCacheTest, InvalidateCacheSizeZeroReissuesLookup) {
    int countLookups = 0;
    CacheWithThreadPool cache(
        getServiceContext(), 0, [&](OperationContext*, const std::string& key) {
            ASSERT_EQ("TestKey", key);
            countLookups++;
            return Cache::LookupResult(CachedValue(1000 * countLookups));
        });

    auto value = cache.acquire(_opCtx, "TestKey");
    ASSERT(value);
    ASSERT_EQ(1000, value->counter);
    ASSERT_EQ(1, countLookups);

    // Because 'value' above is held alive, the cache will not perform lookup until it is destroyed
    ASSERT_EQ(1000, cache.acquire(_opCtx, "TestKey")->counter);
    ASSERT_EQ(1, countLookups);

    cache.invalidate("TestKey");
    auto valueAfterInvalidate = cache.acquire(_opCtx, "TestKey");
    ASSERT(!value.isValid());
    ASSERT(valueAfterInvalidate);
    ASSERT_EQ(2000, valueAfterInvalidate->counter);
    ASSERT_EQ(2, countLookups);
}

TEST_F(ReadThroughCacheTest, KeyDoesNotExist) {
    CacheWithThreadPool cache(
        getServiceContext(), 1, [&](OperationContext*, const std::string& key) {
            ASSERT_EQ("TestKey", key);
            return Cache::LookupResult(boost::none);
        });

    ASSERT(!cache.acquire(_opCtx, "TestKey"));
}

/**
 * Fixture for tests, which need to control the creation/destruction of their operation contexts.
 */
class ReadThroughCacheAsyncTest : public unittest::Test,
                                  public ScopedGlobalServiceContextForTest {};

using Barrier = unittest::Barrier;

TEST_F(ReadThroughCacheAsyncTest, SuccessfulInProgressLookup) {
    ThreadPool threadPool{ThreadPool::Options()};
    threadPool.startup();

    Cache cache(getServiceContext(), threadPool, 1, [&](OperationContext*, const std::string& key) {
        return Cache::LookupResult(CachedValue(500));
    });

    Cache::InProgressLookup inProgress(cache, "TestKey");
    auto future = inProgress.addWaiter(WithLock::withoutLock());
    ASSERT(!future.isReady());

    auto optVal = inProgress.asyncLookupRound().get();
    ASSERT(inProgress.valid(WithLock::withoutLock()));
    ASSERT(optVal.v);
    ASSERT_EQ(500, optVal.v->counter);
    inProgress.signalWaiters(Cache::ValueHandle(std::move(*optVal.v)));

    ASSERT(future.isReady());
    ASSERT_EQ(500, future.get()->counter);
}

TEST_F(ReadThroughCacheAsyncTest, FailedInProgressLookup) {
    ThreadPool threadPool{ThreadPool::Options()};
    threadPool.startup();

    Cache cache(getServiceContext(),
                threadPool,
                1,
                [&](OperationContext*, const std::string& key) -> Cache::LookupResult {
                    uasserted(ErrorCodes::InternalError, "Test error");
                });

    Cache::InProgressLookup inProgress(cache, "TestKey");
    auto future = inProgress.addWaiter(WithLock::withoutLock());
    ASSERT(!future.isReady());

    auto asyncLookupResult = inProgress.asyncLookupRound().getNoThrow();
    ASSERT_THROWS_CODE(inProgress.asyncLookupRound().get(), DBException, ErrorCodes::InternalError);
    ASSERT(inProgress.valid(WithLock::withoutLock()));
    inProgress.signalWaiters(asyncLookupResult.getStatus());

    ASSERT(future.isReady());
    ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::InternalError);
}

TEST_F(ReadThroughCacheAsyncTest, AcquireObservesOperationContextDeadline) {
    ThreadPool threadPool{ThreadPool::Options()};
    threadPool.startup();

    Barrier lookupStartedBarrier(2);
    Barrier completeLookupBarrier(2);
    Cache cache(getServiceContext(), threadPool, 1, [&](OperationContext*, const std::string& key) {
        lookupStartedBarrier.countDownAndWait();
        completeLookupBarrier.countDownAndWait();
        return Cache::LookupResult(CachedValue(5));
    });

    {
        ThreadClient tc(getServiceContext());
        const ServiceContext::UniqueOperationContext opCtxHolder{tc->makeOperationContext()};
        OperationContext* const opCtx{opCtxHolder.get()};

        opCtx->setDeadlineAfterNowBy(Milliseconds{5}, ErrorCodes::ExceededTimeLimit);
        ASSERT_THROWS_CODE(
            cache.acquire(opCtx, "TestKey"), DBException, ErrorCodes::ExceededTimeLimit);

        lookupStartedBarrier.countDownAndWait();
    }

    completeLookupBarrier.countDownAndWait();

    {
        ThreadClient tc(getServiceContext());
        const ServiceContext::UniqueOperationContext opCtxHolder{tc->makeOperationContext()};
        OperationContext* const opCtx{opCtxHolder.get()};

        auto value = cache.acquire(opCtx, "TestKey");
        ASSERT(value);
        ASSERT_EQ(5, value->counter);
    }

    {
        ThreadClient tc(getServiceContext());
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

    Cache cache(getServiceContext(), threadPool, 1, [&](OperationContext*, const std::string& key) {
        int idx = countLookups.fetchAndAdd(1);
        lookupStartedBarriers[idx].countDownAndWait();
        completeLookupBarriers[idx].countDownAndWait();
        return Cache::LookupResult(CachedValue(idx));
    });

    // Kick off the first lookup, which will block
    auto future = cache.acquireAsync("TestKey");
    ASSERT(!future.isReady());

    // Wait for the first lookup attempt to start and invalidate it before letting it proceed
    lookupStartedBarriers[0].countDownAndWait();
    ASSERT_EQ(1, countLookups.load());
    cache.invalidate("TestKey");
    ASSERT(!future.isReady());
    completeLookupBarriers[0].countDownAndWait();  // Lets lookup attempt 1 proceed
    ASSERT(!future.isReady());

    // Wait for the second lookup attempt to start and invalidate it before letting it proceed
    lookupStartedBarriers[1].countDownAndWait();
    ASSERT_EQ(2, countLookups.load());
    cache.invalidate("TestKey");
    ASSERT(!future.isReady());
    completeLookupBarriers[1].countDownAndWait();  // Lets lookup attempt 2 proceed
    ASSERT(!future.isReady());

    // Wait for the third lookup attempt to start, but not do not invalidate it before letting it
    // proceed
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

    Cache cache(getServiceContext(), threadPool, 1, [&](OperationContext*, const std::string&) {
        FAIL("Should not be called");
        return Cache::LookupResult(CachedValue(0));  // Will never be reached
    });

    auto future = cache.acquireAsync("TestKey");
    ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::ShutdownInProgress);
}

TEST_F(ReadThroughCacheAsyncTest, InvalidateCalledBeforeLookupTaskExecutes) {
    struct MockThreadPool : public ThreadPoolInterface {
        void startup() override {}
        void shutdown() override {}
        void join() override {}
        void schedule(Task task) override {
            ASSERT(!mostRecentTask);
            mostRecentTask = std::move(task);
        }
        void runMostRecentTask() {
            ASSERT(mostRecentTask);
            auto f = std::move(mostRecentTask);
            f(Status::OK());
        }

        Task mostRecentTask;
    } threadPool;

    Cache cache(getServiceContext(), threadPool, 1, [&](OperationContext*, const std::string&) {
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

}  // namespace
}  // namespace mongo
