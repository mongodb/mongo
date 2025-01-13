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

#include <absl/container/flat_hash_set.h>

#include "mongo/util/assert_util.h"
#include "mongo/util/roaring_bitmaps.h"


namespace mongo {
/**
 * A set of unsigned 64 bit integers implemented as  a mixed data structure that uses hash table to
 * store the first `threshold` elements. Once the set's density reaches at least
 * threshold/universeSize, it switches to Roaring Bitmaps.
 */
class HashRoaringSet {
public:
    /**
     * Defines the possible states of the set.
     */
    enum State {
        // The set uses hash table to store the data.
        kHashTable,

        // The set switched from hash table to Roaring Bitmaps, but some old data is still stored in
        // hash table, because thedata migration is not yet completed.
        kHashTableAndBitmap,

        // The set uses Roaring Bitmaps only.
        kBitmap,
    };

    /**
     * Initialize a new instance of the set.
     * 'threshold' is the number of elements after which the set switches from hash table to Roaring
     * Bitmaps, 'chunkSize' is the number of elements moved during migration from the hash table to
     * Roaring Bitmaps each time an element (new or existing) is added to the set, 'universeSize' is
     * maximum difference between the maximal and minimal elements in the set that is allowed for
     * the cutover to Roaring Bitmaps, `onSwitchToRoaring` is a callback function that is called
     * when the cutover is initiated.
     */
    HashRoaringSet(size_t threshold,
                   size_t chunkSize,
                   uint64_t universeSize,
                   std::function<void()> onSwitchToRoaring = {})
        : _threshold(threshold),
          _chunkSize(chunkSize),
          _universeSize(universeSize),
          _onSwitchToRoaring(onSwitchToRoaring),
          _size(0),
          _minValue(std::numeric_limits<uint64_t>::max()),
          _maxValue(0),
          _state(kHashTable) {}

    /**
     * Add the value. Returns true if a new values was added, false otherwise.
     */
    bool addChecked(uint64_t value) {
        if (_size == _threshold) {
            _state = (_maxValue - _minValue) < _universeSize ? kHashTableAndBitmap : kHashTable;
            ++_size;
            if (_onSwitchToRoaring) {
                _onSwitchToRoaring();
            }
        }

        switch (_state) {
            case kHashTable:
                if (_hashTable.insert(value).second) {
                    ++_size;
                    _minValue = std::min(_minValue, value);
                    _maxValue = std::max(_maxValue, value);
                    return true;
                }
                return false;
            case kHashTableAndBitmap:
                migrateNextChunk();
                return !_hashTable.contains(value) && _bitmap.addChecked(value);
            case kBitmap:
                return _bitmap.addChecked(value);
        }

        MONGO_UNREACHABLE;
    }

    bool contains(uint64_t value) const {
        switch (_state) {
            case kHashTable:
                return _hashTable.contains(value);
            case kHashTableAndBitmap:
                return _hashTable.contains(value) || _bitmap.contains(value);
            case kBitmap:
                return _bitmap.contains(value);
        }

        MONGO_UNREACHABLE;
    }

    /**
     * Expose the current data structures used by the set at the moment.
     */
    State getCurrentState() const {
        return _state;
    }

private:
    /**
     * Move 'chunkSize' elements from hash table to Roaring Bitmaps.
     */
    void migrateNextChunk() {
        auto pos = _hashTable.begin();
        for (size_t i = 0; i < _chunkSize && pos != _hashTable.end(); ++i) {
            _bitmap.add(*pos);
            _hashTable.erase(pos++);
        }

        if (_hashTable.empty()) {
            _hashTable = absl::flat_hash_set<uint64_t>();
            _state = kBitmap;
        }
    }

    absl::flat_hash_set<uint64_t> _hashTable;
    Roaring64BTree _bitmap;

    const size_t _threshold;
    const size_t _chunkSize;
    const uint64_t _universeSize;
    std::function<void()> _onSwitchToRoaring;
    size_t _size;
    uint64_t _minValue;
    uint64_t _maxValue;

    State _state;
};
}  // namespace mongo
