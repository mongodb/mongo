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

#pragma once

#include "mongo/platform/compiler.h"
#include "mongo/stdx/new.h"
#include "mongo/util/concurrency/with_lock.h"

namespace mongo {

namespace repl {

/**
 *    Provides lock-free reads for arbitrarily sized values, at the expense of
 *    keeping two copies at all times. Writes are not lock-free, and writers
 *    need to synchronize before updating the values.
 *
 *    Under the hood, this type maintains two copies of the data. Readers will
 *    always read from the active copy, where read-after-write data races are
 *    avoided through acquire/release semantics. A generation number ensures
 *    that readers can detect such races in the presence of many writers.
 *
 *    Prefer using other synchronization primitives over this one when possible.
 */
template <typename DatumT>
class DataWithLockFreeReads {
public:
    using datum_type = DatumT;

    // store() requires the caller to hold the lock that protects the
    // uncached value to avoid races.
    void store(WithLock lk, const datum_type& datum) {
        auto curGen = _generation.load();
        auto nextGen = curGen + 1;
        _buffers[nextGen & 1] = datum;
        invariant(_generation.compareAndSwap(&curGen, nextGen));
    }

    datum_type load() const {
        while (true) {
            // Get the counter, use it to index buffers and perform a read, and
            // then read the counter again. If the counter hasn't moved by more
            // than 1, then the value that you just read was not torn.
            auto initialGen = _generation.loadAcquire();
            datum_type result = _buffers[initialGen & 1];
            auto finalGen = _generation.loadAcquire();
            if (MONGO_likely(finalGen == initialGen)) {
                return result;
            }
        }
    }

private:
    alignas(stdx::hardware_destructive_interference_size) datum_type _buffers[2];

    alignas(stdx::hardware_destructive_interference_size) mutable AtomicWord<uint64_t> _generation{
        0};
};
}  // namespace repl
}  // namespace mongo
