// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/util/modules.h"

#include <list>
#include <vector>

namespace mongo {
namespace repl {

/**
 * SlottedTimestampList tracks allocated oplog timestamps in a doubly linked list.
 * It supports inserting a new timestamp to the end of the list as well as erasing
 * from anywhere in the list, both in O(1) time. To avoid memory allocations, the
 * list keeps a freelist of slots that can be reused for newly inserted timestamps
 * and never shrinks.
 */
class SlottedTimestampList {
public:
    using iterator = std::list<Timestamp>::iterator;
    using const_iterator = std::list<Timestamp>::const_iterator;

    SlottedTimestampList() {
        _freeListEnd = _tsList.begin();
    }

    /**
     * Inserts a new timestamp at the end of the list. A free slot is reused
     * if available, to avoid memory allocation.
     *
     * @param val new timestamp to insert
     * @return const_iterator pointing to the newly inserted timestamp
     */
    const_iterator insert(const Timestamp& val) {
        // No free slots.
        if (_freeListEnd == _tsList.begin()) {
            auto it = _tsList.insert(_tsList.end(), val);
            _freeListEnd = _tsList.begin();
            return it;
        }

        // Reusing free slot.
        auto result = _tsList.begin();
        *result = val;
        _tsList.splice(_tsList.end(), _tsList, result);

        // Entire list is free slots.
        if (_freeListEnd == _tsList.end()) {
            _freeListEnd = result;
        }

        return result;
    }

    /**
     * Erases the timestamp at pos from the list. This does not shrink the
     * list, but instead moves the corresponding slot to the freelist so
     * that it can be reused later for new inserts.
     *
     * @param pos const_iterator of the element to be erased
     * @return true if the erased element is the first on the in-use list
     */
    bool erase(const_iterator pos) {
        // Erase pos if it is the first in-use timestamp
        if (pos == _freeListEnd) {
            _freeListEnd++;
            return true;
        }
        // Erase pos if it is not the first in-use timestamp
        _tsList.splice(_tsList.begin(), _tsList, pos);
        return false;
    }

    /**
     * Erases all tracked timestamps from the list.
     */
    void clear() {
        _freeListEnd = _tsList.end();
    }

    /**
     * Returns the number of tracked timestamps.
     *
     * @return the number of tracked timestamps
     */
    std::size_t size() const {
        return std::distance(static_cast<const_iterator>(_freeListEnd), _tsList.end());
    }

    /**
     * Checks if the list has no tracked timestamps.
     *
     * @return true if the list has no tracked timestamps, false otherwise
     */
    bool empty() const {
        return _freeListEnd == _tsList.end();
    }

    /**
     * Returns a reference to the first tracked timestamp in the list.
     *
     * @return reference to the first tracked timestamp
     */
    const Timestamp& front() const {
        invariant(!empty());
        return *_freeListEnd;
    }

    /**
     * Returns a reference to the last tracked timestamp in the list.
     *
     * @return reference to the last tracked timestamp
     */
    const Timestamp& back() const {
        invariant(!empty());
        return *_tsList.rbegin();
    }

    /**
     * Returns a vector of all tracked timestamps, in insertion order.
     *
     * @return vector of all tracked timestamps in insertion order.
     */
    std::vector<Timestamp> getVector_forTest() const {
        return std::vector(static_cast<const_iterator>(_freeListEnd), _tsList.end());
    }

    /**
     * Returns the total number of in-use timestamps and free slots.
     *
     * @return total number of in-use timestamps and free slots
     */
    std::size_t getCapacity_forTest() const {
        return _tsList.size();
    }

private:
    /**
     * _tsList contains two logical sub-lists linked together, a sub-list [_tsList.begin(),
     * _freeListEnd) containing free slots that can be reused, followed by a sub-list
     * [_freeListEnd, _tsList.end()) containing in-use slots that track the reserved oplog
     * timestamps in ascending order. _tsList starts empty and expands when new timestamps
     * are inserted and there are no free slots left. When a timestamp is fulfilled, the
     * corresponding slot becomes a free slot and will be moved back to free slot sub-list.
     * _tsList never shrinks and its size is capped at the maximum number of simultaneous
     * timestamp reservations, which should be reasonably small.
     */
    std::list<Timestamp> _tsList;
    iterator _freeListEnd;
};

}  // namespace repl
}  // namespace mongo
