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

#include "mongo/stdx/new.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/with_lock.h"

#include <cstdint>
#include <random>

#include <boost/functional/hash.hpp>

namespace mongo {
namespace repl {

template <size_t N>
struct Data {
    static constexpr size_t serializationForLockFreeReadsU64Count = N;
    using Buffer = DataWithLockFreeReadsBuffer<serializationForLockFreeReadsU64Count>;

    Data() {
        _data.fill(0);
    }

    explicit Data(uint64_t value) {
        _data.fill(value);
    }

    static Data parseForLockFreeReads(Buffer& data) {
        auto d = Data();
        d._data = data;
        return d;
    }

    Buffer serializeForLockFreeReads() const {
        return _data;
    }

    const Buffer& getData() const {
        return _data;
    }

private:
    Buffer _data;
};

TEST(DataWithLockFreeReadsTest, DataSerDeser) {
    const size_t dataSize = 4;
    auto d0 = Data<dataSize>(7);
    auto buf = d0.serializeForLockFreeReads();
    auto d1 = Data<dataSize>::parseForLockFreeReads(buf);
    ASSERT_EQ(d0.getData(), d1.getData());
}

TEST(DataWithLockFreeReadsTest, ShouldStoreAndLoad) {
    const size_t dataSize = 4;
    DataWithLockFreeReads<Data<dataSize>> dc;
    auto last = Data<dataSize>(1);
    dc.store(WithLock::withoutLock(), last);
    ASSERT_EQ(last.getData(), dc.load().getData());
    last = Data<dataSize>(2);
    dc.store(WithLock::withoutLock(), last);
    ASSERT_EQ(last.getData(), dc.load().getData());
    last = Data<dataSize>(3);
    dc.store(WithLock::withoutLock(), last);
    last = Data<dataSize>(4);
    dc.store(WithLock::withoutLock(), last);
    last = Data<dataSize>(5);
    dc.store(WithLock::withoutLock(), last);
    ASSERT_EQ(last.getData(), dc.load().getData());
}

TEST(DataWithLockFreeReadsTest, ShouldNotSeeTornReads) {
    const int numReaders = 8;
    const long long totalReads = 1LL << 10;
    AtomicWord<bool> stopWrites{false};
    DataWithLockFreeReads<Data<4>> dc;
    dc.store(WithLock::withoutLock(), Data<4>(0));
    stdx::thread writer([&]() {
        uint64_t j = 0;
        while (!stopWrites.load()) {
            // Simulate writers that are doing something else sometimes, not
            // just hot-spinning.
            sleepmillis(1);
            dc.store(WithLock::withoutLock(), Data<4>(j++));
        }
    });
    std::vector<stdx::thread> readers;
    for (int i = 0; i < numReaders; ++i) {
        readers.emplace_back([&]() {
            auto readsRemaining = totalReads;
            auto lastValue = dc.load();
            while (readsRemaining > 0) {
                auto d = dc.load();
                ASSERT_EQ(d.getData()[0], d.getData()[d.getData().size() - 1]);
                if (d.getData() != lastValue.getData()) {
                    --readsRemaining;
                }
                lastValue = d;
            }
        });
    }
    for (auto& thread : readers) {
        thread.join();
    }
    stopWrites.store(true);
    writer.join();
}
}  // namespace repl
}  // namespace mongo
