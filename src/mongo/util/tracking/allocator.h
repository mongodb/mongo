/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/aligned.h"
#include "mongo/util/shared_buffer.h"

namespace mongo::tracking {

/**
 * A minimal implementation of a partitioned counter for incrementing and decrementing allocations
 * across multiple threads.
 */
class AllocatorStats {
public:
    explicit AllocatorStats(size_t numPartitions)
        : _numPartitions(numPartitions), _bytesAllocated(_numPartitions) {}

    void bytesAllocated(size_t n) {
        auto& counter = _bytesAllocated[_getSlot()];
        counter.value.fetchAndAddRelaxed(n);
    }

    void bytesDeallocated(size_t n) {
        auto& counter = _bytesAllocated[_getSlot()];
        counter.value.fetchAndSubtractRelaxed(n);
    }

    uint64_t allocated() const {
        int64_t sum = 0;
        for (auto& counter : _bytesAllocated) {
            sum += counter.value.loadRelaxed();
        }

        // After summing the memory usage, we should not have a negative number.
        invariant(sum >= 0, std::to_string(sum));
        return static_cast<uint64_t>(sum);
    }

private:
    size_t _getSlot() const {
        return std::hash<std::thread::id>{}(stdx::this_thread::get_id()) % _numPartitions;
    }

    const size_t _numPartitions;

    // The counter is signed to handle the case where a thread closes a time-series bucket that
    // deallocates more memory than is going to be allocated.
    struct AlignedAtomic {
        alignas(stdx::hardware_destructive_interference_size) AtomicWord<int64_t> value;
    };
    static_assert(alignof(AlignedAtomic) == stdx::hardware_destructive_interference_size);

    std::vector<AlignedAtomic> _bytesAllocated;
};

/**
 * A minimal allocator that keeps track of the number of bytes allocated and deallocated.
 */
template <class T>
class Allocator {
public:
    using value_type = T;
    using propagate_on_container_move_assignment = std::true_type;

    Allocator() = delete;
    explicit Allocator(AllocatorStats& stats) noexcept : _stats(stats){};
    Allocator(const Allocator&) noexcept = default;

    ~Allocator() = default;

    template <class U>
    Allocator(const Allocator<U>& ta) noexcept : _stats{ta.stats()} {};

    T* allocate(size_t n) {
        const size_t allocation = n * sizeof(T);
        _stats.get().bytesAllocated(allocation);
        return static_cast<T*>(::operator new(allocation));
    }

    void deallocate(T* p, size_t n) {
        auto size = n * sizeof(T);
        _stats.get().bytesDeallocated(size);
        ::operator delete(p, size);
    }

    AllocatorStats& stats() const {
        return _stats;
    }

private:
    std::reference_wrapper<AllocatorStats> _stats;
};

template <class T, class U>
bool operator==(const Allocator<T>& lhs, const Allocator<U>& rhs) noexcept {
    return &lhs.stats() == &rhs.stats();
}

template <class T, class U>
bool operator!=(const Allocator<T>& lhs, const Allocator<U>& rhs) noexcept {
    return &lhs.stats() != &rhs.stats();
}

}  // namespace mongo::tracking
