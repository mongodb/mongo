/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <vector>

#include "mongo/stdx/thread.h"
#include "mongo/transport/grpc/client_cache.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"

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
