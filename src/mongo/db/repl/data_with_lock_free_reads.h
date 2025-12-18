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
#include "mongo/platform/rwmutex.h"
#include "mongo/stdx/new.h"
#include "mongo/util/concurrency/with_lock.h"

#include <cstdint>
#include <type_traits>

namespace mongo {
namespace repl {
template <size_t N>
using DataWithLockFreeReadsBuffer MONGO_MOD_PUB = std::array<uint64_t, N>;

template <typename T>
concept SerializableForLockFreeReads = requires(
    T obj, DataWithLockFreeReadsBuffer<T::serializationForLockFreeReadsU64Count> serialized) {
    { T::serializationForLockFreeReadsU64Count } -> std::same_as<const size_t&>;
    { obj.serializeForLockFreeReads() };
    { T::parseForLockFreeReads(serialized) } -> std::same_as<T>;
};

/**
 *    Provides lock-free reads via a SeqLock for arbitrarily sized values, at
 *    the expense of possibly having to retry reads when the underlying atomics
 *    are updated while reading. Writes are not lock-free, and writers need to
 *    hold a mutex while updating the values.
 *
 *    This is modeled in CppMem in data_with_lock_free_reads_X.cppmem. For more
 *    about CppMem, see http://svr-pes20-cppmem.cl.cam.ac.uk/cppmem/index.html.
 *
 *    Prefer using other synchronization primitives over this one when possible.
 */
template <SerializableForLockFreeReads T>
class MONGO_MOD_PUB DataWithLockFreeReads {
public:
    using value_type = T;
    static constexpr size_t N = value_type::serializationForLockFreeReadsU64Count;
    using Buffer = DataWithLockFreeReadsBuffer<N>;

    void store(WithLock lk, const value_type& datum) noexcept {
        auto buf = datum.serializeForLockFreeReads();
        // Since writers must hold the mutex, we can avoid
        // what would otherwise be an acquire load here.
        auto curGen = _generation.loadRelaxed();
        invariant(!(curGen & 1));
        _generation.storeRelaxed(curGen + 1);
        for (size_t i = 0; i < N; ++i) {
            _buffer[i].store(buf[i]);  // release
        }
        _generation.store(curGen + 2);  // release
        _generation.notifyAll();
    }

    value_type load() const noexcept {
        uint64_t initialGen;
        Buffer serialized;
        do {
            initialGen = _generation.load();  // acquire
            while (MONGO_unlikely(initialGen & 1)) {
                initialGen = _generation.wait(initialGen);  // acquire
            }
            for (size_t i = 0; i < N; ++i) {
                serialized[i] = _buffer[i].load();  // acquire
            }
        } while (initialGen != _generation.loadRelaxed());
        return value_type::parseForLockFreeReads(serialized);
    }

private:
    static_assert(value_type::serializationForLockFreeReadsU64Count > 1,
                  "Anything that can fit in a single Atomic should use that instead");

    alignas(stdx::hardware_destructive_interference_size) Atomic<uint64_t> _buffer[N];

    // Low bit is 1 while there is a write in progress. Remaining bits are a
    // count of completed modifications to _buffer.
    WaitableAtomic<uint64_t> _generation;
};

}  // namespace repl
}  // namespace mongo
