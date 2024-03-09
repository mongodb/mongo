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

namespace mongo {

struct TrackingAllocatorStats {
    AtomicWord<uint64_t> bytesAllocated;
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
    explicit TrackingAllocator(TrackingAllocatorStats* stats) noexcept : _stats(stats){};
    TrackingAllocator(const TrackingAllocator&) noexcept = default;

    ~TrackingAllocator() = default;

    template <class U>
    TrackingAllocator(const TrackingAllocator<U>& ta) noexcept : _stats{ta.getStats()} {};

    T* allocate(size_t n) {
        const size_t allocation = n * sizeof(T);
        _stats->bytesAllocated.fetchAndAddRelaxed(allocation);
        return static_cast<T*>(::operator new(allocation));
    }

    void deallocate(T* p, size_t n) {
        auto size = n * sizeof(T);
        _stats->bytesAllocated.fetchAndSubtractRelaxed(size);
        ::operator delete(p, size);
    }

    TrackingAllocatorStats* getStats() const {
        return _stats;
    }

private:
    TrackingAllocatorStats* _stats;
};

template <class T, class U>
bool operator==(const TrackingAllocator<T>& lhs, const TrackingAllocator<U>& rhs) noexcept {
    return lhs.getStats() == rhs.getStats();
}

template <class T, class U>
bool operator!=(const TrackingAllocator<T>& lhs, const TrackingAllocator<U>& rhs) noexcept {
    return lhs.getStats() != rhs.getStats();
}

}  // namespace mongo
