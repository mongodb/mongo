// unordered_fast_key_table_internal.h


/*    Copyright 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include "mongo/util/unordered_fast_key_table.h"

namespace mongo {

template <typename K_L, typename K_S, typename V, typename Traits>
inline int UnorderedFastKeyTable<K_L, K_S, V, Traits>::Area::find(const HashedKey& key,
                                                                  int* firstEmpty) const {
    dassert(capacity());                        // Caller must special-case empty tables.
    dassert(!firstEmpty || *firstEmpty == -1);  // Caller must initialize *firstEmpty.

    unsigned probe = 0;
    do {
        unsigned pos = (key.hash() + probe) & _hashMask;

        if (!_entries[pos].used) {
            // space is empty
            if (firstEmpty && *firstEmpty == -1)
                *firstEmpty = pos;
            if (!_entries[pos].everUsed)
                return -1;
            continue;
        }

        if (_entries[pos].curHash != key.hash()) {
            // space has something else
            continue;
        }

        if (!Traits::equals(key.key(), Traits::toLookup(_entries[pos].data.first))) {
            // hashes match
            // strings are not equals
            continue;
        }

        // hashes and strings are equal
        // yay!
        return pos;
    } while (++probe < _maxProbe);
    return -1;
}

template <typename K_L, typename K_S, typename V, typename Traits>
inline bool UnorderedFastKeyTable<K_L, K_S, V, Traits>::Area::transfer(Area* newArea) const {
    for (auto&& entry : *this) {
        if (!entry.used)
            continue;

        int firstEmpty = -1;
        int loc = newArea->find(HashedKey(Traits::toLookup(entry.data.first), entry.curHash),
                                &firstEmpty);

        verify(loc == -1);
        if (firstEmpty < 0) {
            return false;
        }

        newArea->_entries[firstEmpty] = entry;
    }
    return true;
}

template <typename K_L, typename K_S, typename V, typename Traits>
inline UnorderedFastKeyTable<K_L, K_S, V, Traits>::UnorderedFastKeyTable(
    std::initializer_list<std::pair<key_type, mapped_type>> entries)
    : UnorderedFastKeyTable() {
    for (auto&& entry : entries) {
        // Only insert the entry if the key is not equivalent to the key of any other element
        // already in the table.
        auto key = HashedKey(entry.first);
        if (find(key) == end()) {
            get(key) = entry.second;
        }
    }
}

template <typename K_L, typename K_S, typename V, typename Traits>
inline V& UnorderedFastKeyTable<K_L, K_S, V, Traits>::get(const HashedKey& key) {
    if (!_area._entries) {
        // This is the first insert ever. Need to allocate initial space.
        dassert(_area.capacity() == 0);
        _grow();
    }

    for (int numGrowTries = 0; numGrowTries < 5; numGrowTries++) {
        int firstEmpty = -1;
        int pos = _area.find(key, &firstEmpty);
        if (pos >= 0)
            return _area._entries[pos].data.second;

        // key not in map
        // need to add
        if (firstEmpty >= 0) {
            _size++;
            _area._entries[firstEmpty].used = true;
            _area._entries[firstEmpty].everUsed = true;
            _area._entries[firstEmpty].curHash = key.hash();
            _area._entries[firstEmpty].data.first = Traits::toStorage(key.key());
            return _area._entries[firstEmpty].data.second;
        }

        // no space left in map
        _grow();
    }
    msgasserted(16471, "UnorderedFastKeyTable couldn't add entry after growing many times");
}

template <typename K_L, typename K_S, typename V, typename Traits>
inline size_t UnorderedFastKeyTable<K_L, K_S, V, Traits>::erase(const HashedKey& key) {
    if (_size == 0)
        return 0;  // Nothing to delete.

    int pos = _area.find(key, nullptr);

    if (pos < 0)
        return 0;

    --_size;
    _area._entries[pos].used = false;
    _area._entries[pos].data.second = V();
    return 1;
}

template <typename K_L, typename K_S, typename V, typename Traits>
void UnorderedFastKeyTable<K_L, K_S, V, Traits>::erase(const_iterator it) {
    dassert(it._position >= 0);
    dassert(it._area == &_area);

    --_size;
    _area._entries[it._position].used = false;
    _area._entries[it._position].data.second = V();
}

template <typename K_L, typename K_S, typename V, typename Traits>
inline void UnorderedFastKeyTable<K_L, K_S, V, Traits>::_grow() {
    unsigned capacity = _area.capacity();
    for (int numGrowTries = 0; numGrowTries < 5; numGrowTries++) {
        if (capacity == 0) {
            const unsigned kDefaultStartingCapacity = 16;
            capacity = kDefaultStartingCapacity;
        } else {
            capacity *= 2;
        }

        const double kMaxProbeRatio = 0.05;
        unsigned maxProbes = (capacity * kMaxProbeRatio) + 1;  // Round up

        Area newArea(capacity, maxProbes);
        bool success = _area.transfer(&newArea);
        if (!success) {
            continue;
        }
        _area.swap(&newArea);
        return;
    }
    msgasserted(16845, "UnorderedFastKeyTable::_grow couldn't add entry after growing many times");
}
}
