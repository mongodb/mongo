/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/repl/data_with_lock_free_reads.h"

#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/with_lock.h"

#include <random>

#include <boost/functional/hash.hpp>

namespace mongo {
namespace repl {

TEST(DataWithLockFreeReadsTest, ShouldStoreAndLoad) {
    DataWithLockFreeReads<int> dc;
    dc.store(WithLock::withoutLock(), 1);
    ASSERT_EQ(dc.load(), 1);
    dc.store(WithLock::withoutLock(), 2);
    ASSERT_EQ(dc.load(), 2);
    dc.store(WithLock::withoutLock(), 3);
    dc.store(WithLock::withoutLock(), 4);
    dc.store(WithLock::withoutLock(), 5);
    ASSERT_EQ(dc.load(), 5);
}

struct alignas(128) Payload {
public:
    explicit Payload() {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        std::uniform_int_distribution<uint64_t> dist;
        for (size_t i = 0; i < 16; ++i) {
            _data[i] = dist(gen);
        }
    }

    bool operator==(const Payload& other) const {
        return std::memcmp(_data, other._data, sizeof(_data)) == 0;
    }

    struct Hash {
        std::size_t operator()(const Payload& payload) const {
            std::size_t seed = 0;
            for (int i = 0; i < 16; ++i) {
                boost::hash_combine(seed, payload._data[i]);
            }
            return seed;
        }
    };

private:
    uint64_t _data[16];
};
static_assert(sizeof(Payload) == 128);

TEST(DataWithLockFreeReadsTest, ShouldNotSeeTornReads) {
    const int numPayloads = 64;
    const int numReaders = 8;
    const int totalReads = 2 << 16;
    AtomicWord<int> loaded{0};
    AtomicWord<bool> stopWrites{false};
    stdx::mutex mu;
    auto payloads = std::array<Payload, numPayloads>();
    stdx::unordered_set<Payload, Payload::Hash> payloadsLookup;
    for (size_t i = 0; i < payloads.size(); ++i) {
        payloads[i] = Payload();
        payloadsLookup.insert(payloads[i]);
    }
    DataWithLockFreeReads<Payload*> dc;
    {
        stdx::lock_guard lk(mu);
        dc.store(lk, &payloads[0]);
    }
    std::vector<stdx::thread> readers;
    for (int i = 0; i < numReaders; ++i) {
        readers.emplace_back([&]() {
            while (loaded.fetchAndAdd(1) < totalReads) {
                auto p = dc.load();
                ASSERT_TRUE(payloadsLookup.contains(*p));
            }
        });
    }
    stdx::thread writer([&]() {
        while (!stopWrites.load()) {
            for (size_t i = 0; i < payloads.size(); ++i) {
                stdx::lock_guard lk(mu);
                dc.store(lk, &payloads[i]);
            }
        }
    });
    for (auto& thread : readers) {
        thread.join();
    }
    stopWrites.store(true);
    writer.join();
}
}  // namespace repl
}  // namespace mongo
