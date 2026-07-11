// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/atomic.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <cstdint>
#include <new>
#include <thread>
#include <type_traits>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace tracking {

/**
 * A minimal implementation of a partitioned counter for incrementing and decrementing allocations
 * across multiple threads.
 */
class AllocatorStats {
public:
    explicit AllocatorStats(size_t numPartitions)
        : _numPartitions(numPartitions * 2), _bytesAllocated(_numPartitions) {}

    void bytesAllocated(size_t n) {
        // The second half of '_bytesAllocated' is reserved for tracking allocation.
        auto& counter = _bytesAllocated[_getAllocSlot()];
        counter.value.fetchAndAddRelaxed(n);
    }

    void bytesDeallocated(size_t n) {
        // The first half of '_bytesAllocated' is reserved for tracking deallocation.
        auto& counter = _bytesAllocated[_getDeallocSlot()];
        counter.value.fetchAndSubtractRelaxed(n);
    }

    uint64_t allocated() const {
        int64_t sum = 0;
        for (auto& counter : _bytesAllocated) {
            sum += counter.value.loadRelaxed();
        }

        // After summing the memory usage, we should not have a negative number.
        // Since the first half is only for deallocation and second half for allocation, iterating
        // through '_bytesAllocated' can only miss the bytes decremented in a matching
        // allocation/deallocation when there is a race. This avoids undercounting.
        invariant(sum >= 0, std::to_string(sum));
        return static_cast<uint64_t>(sum);
    }

private:
    size_t _getDeallocSlot() const {
        return std::hash<std::thread::id>{}(std::this_thread::get_id()) % (_numPartitions / 2);
    }

    size_t _getAllocSlot() const {
        return std::hash<std::thread::id>{}(std::this_thread::get_id()) % (_numPartitions / 2) +
            _numPartitions / 2;
    }

    const size_t _numPartitions;

    // The counter is signed to handle the case where a thread closes a time-series bucket that
    // deallocates more memory than is going to be allocated.
    struct AlignedAtomic {
        alignas(std::hardware_destructive_interference_size) Atomic<int64_t> value;
    };
    static_assert(alignof(AlignedAtomic) == std::hardware_destructive_interference_size);

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
    explicit Allocator(AllocatorStats& stats) : _stats(stats) {}
    Allocator(const Allocator&) noexcept = default;

    ~Allocator() = default;

    template <class U>
    Allocator(const Allocator<U>& ta) noexcept : _stats{ta.stats()} {}

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
bool operator==(const Allocator<T>& lhs, const Allocator<U>& rhs) {
    return &lhs.stats() == &rhs.stats();
}

}  // namespace tracking
}  // namespace mongo
