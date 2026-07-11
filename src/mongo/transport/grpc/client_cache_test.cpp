// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/grpc/client_cache.h"

#include "mongo/stdx/thread.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"

#include <vector>

namespace mongo::transport::grpc {

class ClientCacheTest : public unittest::Test {
public:
    static constexpr size_t kCacheSize = 5;

    void setUp() override {
        _cache = std::make_unique<ClientCache>(kCacheSize);
    }

    void tearDown() override {
        _cache.reset();
    }

    ClientCache& cache() {
        return *_cache;
    }

private:
    std::unique_ptr<ClientCache> _cache;
};

TEST_F(ClientCacheTest, BasicAdd) {
    ASSERT_EQUALS(cache().add(UUID::gen()), ClientCache::AddResult::kCreated);
    ASSERT_EQUALS(cache().add(UUID::gen()), ClientCache::AddResult::kCreated);
    ASSERT_EQUALS(cache().add(UUID::gen()), ClientCache::AddResult::kCreated);
    ASSERT_EQUALS(cache().add(UUID::gen()), ClientCache::AddResult::kCreated);

    auto uuid = UUID::gen();
    ASSERT_EQUALS(cache().add(uuid), ClientCache::AddResult::kCreated);
    ASSERT_EQUALS(cache().add(uuid), ClientCache::AddResult::kRefreshed);

    ASSERT_EQUALS(cache().add(UUID::gen()), ClientCache::AddResult::kCreated);
}

TEST_F(ClientCacheTest, Eviction) {
    // Generate 1-too-many UUIDs to fit in the cache.
    std::vector<UUID> uuids = {};
    for (size_t i = 0; i < kCacheSize + 1; i++) {
        uuids.push_back(UUID::gen());
    }

    // Add them in order twice, asserting that an id is always evicted before it is readded to the
    // cache.
    for (size_t i = 0; i < uuids.size() * 2; i++) {
        ASSERT_EQUALS(cache().add(uuids[i % uuids.size()]), ClientCache::AddResult::kCreated)
            << "clientId should have been evicted before it was added back to the cache";
    }
}

TEST_F(ClientCacheTest, RepeatedAddRefreshesUsage) {
    auto uuid = UUID::gen();
    ASSERT_EQUALS(cache().add(uuid), ClientCache::AddResult::kCreated);

    for (size_t i = 0; i < kCacheSize - 1; i++) {
        ASSERT_EQUALS(cache().add(UUID::gen()), ClientCache::AddResult::kCreated);
    }

    ASSERT_EQUALS(cache().add(uuid), ClientCache::AddResult::kRefreshed);
    ASSERT_EQUALS(cache().add(UUID::gen()), ClientCache::AddResult::kCreated);
    ASSERT_EQUALS(cache().add(uuid), ClientCache::AddResult::kRefreshed)
        << "the previous add of clientId should have caused it to not be evicted when a new entry "
           "was created";
}

TEST_F(ClientCacheTest, CacheIsThreadSafe) {
    // Perform a bunch of concurrent operations with the cache and ensure nothing crashes and
    // TSAN doesn't complain.
    unittest::threadAssertionMonitoredTest([&](unittest::ThreadAssertionMonitor& monitor) {
        std::vector<stdx::thread> threads;

        for (auto i = 0; i < 5; i++) {
            auto thread = monitor.spawn([&]() {
                for (auto j = 0; j < 20; j++) {
                    ASSERT_EQUALS(cache().add(UUID::gen()), ClientCache::AddResult::kCreated);
                }
            });
            threads.push_back(std::move(thread));
        }

        for (auto&& thread : threads) {
            thread.join();
        }
    });
}

}  // namespace mongo::transport::grpc
