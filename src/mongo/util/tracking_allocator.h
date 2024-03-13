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
#include "mongo/util/processinfo.h"
#include "mongo/util/shared_buffer.h"

namespace mongo {

/**
 * A minimal implementation of a partitioned counter for incrementing and decrementing allocations
 * across multiple threads.
 */
class TrackingAllocatorStats {
public:
    // The counter will be partitioned based on the number of available cores.
    TrackingAllocatorStats()
        : _numPartitions(ProcessInfo::getNumLogicalCores() * 2), _bytesAllocated(_numPartitions) {}
    TrackingAllocatorStats(size_t numPartitions)
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
        invariant(sum >= 0,
                  str::stream() << "Tracking allocator memory usage was negative " << sum);
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
class TrackingAllocator {
public:
    using value_type = T;
    using propagate_on_container_move_assignment = std::true_type;

    TrackingAllocator() = delete;
    explicit TrackingAllocator(TrackingAllocatorStats& stats) noexcept : _stats(stats){};
    TrackingAllocator(const TrackingAllocator&) noexcept = default;

    ~TrackingAllocator() = default;

    template <class U>
    TrackingAllocator(const TrackingAllocator<U>& ta) noexcept : _stats{ta.getStats()} {};

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

    TrackingAllocatorStats& getStats() const {
        return _stats;
    }

private:
    std::reference_wrapper<TrackingAllocatorStats> _stats;
};

template <class T, class U>
bool operator==(const TrackingAllocator<T>& lhs, const TrackingAllocator<U>& rhs) noexcept {
    return &lhs.getStats() == &rhs.getStats();
}

template <class T, class U>
bool operator!=(const TrackingAllocator<T>& lhs, const TrackingAllocator<U>& rhs) noexcept {
    return &lhs.getStats() != &rhs.getStats();
}

class TrackingSharedBufferAllocator {
public:
    static constexpr size_t kBuffHolderSize = SharedBuffer::kHolderSize;

    TrackingSharedBufferAllocator(TrackingAllocatorStats& stats, size_t size = 0) : _stats(stats) {
        if (size > 0) {
            malloc(size);
        }
    }

    TrackingSharedBufferAllocator(TrackingSharedBufferAllocator&&) = default;
    TrackingSharedBufferAllocator& operator=(TrackingSharedBufferAllocator&&) = default;

    void malloc(size_t size) {
        _stats.get().bytesAllocated(size);
        _buf = SharedBuffer::allocate(size);
    }

    void realloc(size_t size) {
        if (size > _buf.capacity()) {
            _stats.get().bytesAllocated(size - _buf.capacity());
        } else {
            _stats.get().bytesDeallocated(_buf.capacity() - size);
        }
        _buf.realloc(size);
    }

    void free() {
        _stats.get().bytesDeallocated(_buf.capacity());
        _buf = {};
    }

    SharedBuffer release() {
        _stats.get().bytesDeallocated(_buf.capacity());
        return std::move(_buf);
    }

    size_t capacity() const {
        return _buf.capacity();
    }

    char* get() const {
        return _buf.get();
    }

private:
    SharedBuffer _buf;
    std::reference_wrapper<TrackingAllocatorStats> _stats;
};

}  // namespace mongo
