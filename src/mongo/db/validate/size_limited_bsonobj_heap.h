// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"

#include <cstddef>
#include <vector>

namespace mongo {
/**
 * A heap-backed container of BSONObj that enforces a total byte-size limit.
 *
 * Semantics:
 *  - Tracks an internal used-byte count for all stored BSONObjs.
 *  - When adding a new object would exceed the configured maxBytes, evicts the
 *    largest BSONObj (by objsize()) from the heap to bring usage back under
 *    the limit, as long as it would not drop the container below 1 element.
 *  - Always keeps at least one element, even if that element alone exceeds
 *    the configured limit.
 */
class SizeLimitedBSONObjHeap {
public:
    explicit SizeLimitedBSONObjHeap(size_t maxBytes) : _maxBytes(maxBytes) {}

    /**
     * Adds `obj` to the heap. Returns true if an existing element was evicted
     * to enforce the size limit, false otherwise.
     */
    bool add(BSONObj obj);

    /** Returns the configured size limit in bytes. */
    size_t maxBytes() const {
        return _maxBytes;
    }

    /** Returns the current total size in bytes of all stored BSONObjs. */
    size_t usedBytes() const {
        return _usedBytes;
    }

    /** Number of elements currently stored. */
    size_t size() const {
        return _storage.size();
    }

    bool empty() const {
        return _storage.empty();
    }

    /** Clears all stored elements and resets usage to zero. */
    void clear();

    /**
     * Read-only access to the underlying container for const iteration.
     *
     * NOTE: The vector is maintained as a heap with LargestObjsPopFirstCmp,
     * so the order is heap order, not insertion order.
     */
    const std::vector<BSONObj>& entries() const {
        return _storage;
    }

private:
    struct LargestObjsPopFirstCmp {
        bool operator()(const BSONObj& lhs, const BSONObj& rhs) const {
            // For std::push_heap/std::pop_heap with this comparator,
            // the "largest" object by size will be at the front.
            return lhs.objsize() < rhs.objsize();
        }
    };

    size_t _maxBytes = 0;
    size_t _usedBytes = 0;
    std::vector<BSONObj> _storage;  // maintained as a heap with LargestObjsPopFirstCmp
};

}  // namespace mongo
