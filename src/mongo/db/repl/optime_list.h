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

#include <list>
#include <memory_resource>
#include <utility>
#include <vector>

#include "mongo/bson/timestamp.h"
#include "mongo/platform/mutex.h"
#include "mongo/unittest/assert.h"

namespace mongo {
namespace repl {
/**
 * OpTimeList tracks the inflight Oplog Timestamps at the Replication layer. Oplog Timestamp is
 * inserted to OpTimeList at the OpTime reservation and removed when the corresponding operation
 * completes (written or abandoned)
 *
 * Caller needs to handle synchronization to guarantee in order insertions
 */
class OpTimeList {
public:
    typedef typename std::list<Timestamp>::const_iterator const_iterator;
    typedef typename std::list<Timestamp>::iterator iterator;

    OpTimeList() {
        _freeListEnd = _dlist.begin();
    }

    /**
     * Insert val before pos to the in-use sublist.
     *
     * @param pos iterator before which the content will be inserted
     * @param val element value to insert
     * @return iterator pointing to the newly inserted val
     */
    iterator insert(const_iterator pos, Timestamp&& val) {
        stdx::lock_guard<Latch> lk(_mutex);
        iterator result = _dlist.begin();
        // Reuse free slots
        if (_freeListEnd != _dlist.begin()) {
            if (_freeListEnd == pos) {
                // Update _freeListEnd to point to head of the in-use sublist
                _freeListEnd--;
                *_freeListEnd = val;
                result = _freeListEnd;
            } else {
                *result = val;
                _dlist.splice(std::move(pos), _dlist, result);
            }
        } else {  // no more free slots
            if (_freeListEnd == pos) {
                result = _dlist.insert(std::move(pos), val);
                // Update _freeListEnd to point to head of the in-use sublist
                _freeListEnd--;
            } else {
                result = _dlist.insert(std::move(pos), val);
            }
        }
        return result;
    }

    const_iterator end() const {
        stdx::lock_guard<Latch> lk(_mutex);
        return _dlist.end();
    }

    const_iterator begin() const {
        stdx::lock_guard<Latch> lk(_mutex);
        return _freeListEnd;
    }

    const Timestamp& front() const {
        stdx::lock_guard<Latch> lk(_mutex);
        return *_freeListEnd;
    }

    const Timestamp& back() const {
        stdx::lock_guard<Latch> lk(_mutex);
        return *_dlist.rbegin();
    }

    /**
     * Remove the element at pos of the in-use sublist. The corresponding slot is moved to the free
     * slot sublist. The implementation is optimized for the most common scenario, FIFO, where
     * removal of the first element is done by advancing the iterator by 1
     *
     * @param pos const_iterator of the element to be removed
     */
    void erase(const_iterator pos) {
        stdx::lock_guard<Latch> lk(_mutex);
        if (pos == _freeListEnd) {  // first in use
            _freeListEnd++;
        } else {
            _dlist.splice(_dlist.begin(), _dlist, pos);
        }
    }

    /**
     * Remove the element at pos of the in-use sublist. The corresponding slot is moved to the free
     * slot sublist. The implementation is optimized for the most common scenario, FIFO, where
     * removal of the first element is done by advancing the iterator by 1
     *
     * @param pos const_iterator of the element to be removed
     * @return true if the removed element is the first on the in-use sublist
     */
    bool eraseTrueIfFirst(const_iterator pos) {
        stdx::lock_guard<Latch> lk(_mutex);
        if (pos == _freeListEnd) {
            _freeListEnd++;
            return true;
        } else {
            _dlist.splice(_dlist.begin(), _dlist, pos);
            return false;
        }
    }

    /**
     * Clear the in-use sublist, resulting in all slots being free
     */
    void clear_forTest() {
        stdx::lock_guard<Latch> lk(_mutex);
        _freeListEnd = _dlist.end();
    }

    /**
     * @return A vector of Timestamps retrieved from the in-use sublist, maintaining the original
     * order
     */
    std::vector<Timestamp> getVector_forTest() const {
        stdx::lock_guard<Latch> lk(_mutex);
        std::vector<Timestamp> result;
        for (iterator it = _freeListEnd; it != _dlist.end(); it++) {
            result.emplace_back(std::move(*it));
        }
        return result;
    }

    /**
     * @return true if the in-use sublist is empty
     */
    bool empty() const {
        stdx::lock_guard<Latch> lk(_mutex);
        return _freeListEnd == _dlist.end();
    }

private:
    mutable Mutex _mutex = MONGO_MAKE_LATCH("OpTimeList::_mutex");
    /**
     * _dlist contains two logical sublists linked together, a sublist [_dlist.begin(),
     * _freeListEnd) containing free slots that can be reused followed by a sublist
     * [_freeListEnd, _dlist.end()] containing in-use slots that track the inflight Oplog Timestamps
     * in ascending order. _dlist starts empty and expands when a new Oplog Timestamp is inserted
     * and there is no free slot left. When an Oplog Timestamps completes, the corresponding slot
     * becomes a free slot and will be moved back to free slot sublist. _dlist never shrinks and its
     * maximum size is limited by the maximum number of simultaneous Oplog reservations, which
     * should be reasonably small.
     */
    std::list<Timestamp> _dlist;
    iterator _freeListEnd;
};
}  // namespace repl
}  // namespace mongo
