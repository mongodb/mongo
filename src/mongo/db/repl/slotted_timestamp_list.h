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
