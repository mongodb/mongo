// unordered_fast_key_table.h

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

#include <memory>

#include "mongo/base/disallow_copying.h"
#include "mongo/util/assert_util.h"

namespace mongo {

/**
 * A hash map that allows a different type to be used stored (K_S) than is used for lookups (K_L).
 *
 * Takes a Traits class that must have the following:
 *
 * static uint32_t hash(K_L); // Computes a 32-bit hash of the key.
 * static bool equals(K_L, K_L); // Returns true if the keys are equal.
 * static K_S toStorage(K_L); // Converts from K_L to K_S.
 * static K_L toLookup(K_S); // Converts from K_S to K_L.
 * class HashedKey {
 * public:
 *     explicit HashedKey(K_L key); // Computes hash of key.
 *     HashedKey(K_L key, uint32_t hash); // Populates with known hash.
 *
 *     const K_L& key() const;
 *     uint32_t hash() const; // Should be free to call repeatedly.
 * };
 */
template <typename K_L,  // key lookup
          typename K_S,  // key storage
          typename V,    // value
          typename Traits>
class UnorderedFastKeyTable {
public:
    // Typedefs for compatibility with std::map.
    using value_type = std::pair<K_S, V>;
    using key_type = K_L;
    using mapped_type = V;

    using HashedKey = typename Traits::HashedKey;

private:
    struct Entry {
        Entry() : used(false), everUsed(false) {}

        bool used;
        bool everUsed;
        uint32_t curHash;
        value_type data;
    };

    struct Area {
        Area() = default;  // TODO constexpr

        Area(unsigned capacity, unsigned maxProbe)
            : _hashMask(capacity - 1),
              _maxProbe(maxProbe),
              _entries(capacity ? new Entry[capacity] : nullptr) {
            // Capacity must be a power of two or zero. See the comment on _hashMask for why.
            dassert((capacity & (capacity - 1)) == 0);
        }

        Area(const Area& other) : Area(other.capacity(), other._maxProbe) {
            std::copy(other.begin(), other.end(), begin());
        }

        Area& operator=(const Area& other) {
            Area(other).swap(this);
            return *this;
        }

        int find(const HashedKey& key, int* firstEmpty) const;

        bool transfer(Area* newArea) const;

        void swap(Area* other) {
            using std::swap;
            swap(_hashMask, other->_hashMask);
            swap(_maxProbe, other->_maxProbe);
            swap(_entries, other->_entries);
        }

        unsigned capacity() const {
            return _hashMask + 1;
        }

        Entry* begin() {
            return _entries.get();
        }
        Entry* end() {
            return _entries.get() + capacity();
        }

        const Entry* begin() const {
            return _entries.get();
        }
        const Entry* end() const {
            return _entries.get() + capacity();
        }

        // Capacity is always a power of two. This means that the operation (hash % capacity) can be
        // preformed by (hash & (capacity - 1)). Since we need the mask more than the capacity we
        // store it directly and derive the capacity from it. The default capacity is 0 so the
        // default hashMask is -1.
        unsigned _hashMask = -1;
        unsigned _maxProbe = 0;
        std::unique_ptr<Entry[]> _entries = {};
    };

public:
    UnorderedFastKeyTable() = default;  // TODO constexpr

    UnorderedFastKeyTable(std::initializer_list<std::pair<key_type, mapped_type>> entries);

    /**
     * @return number of elements in map
     */
    size_t size() const {
        return _size;
    }

    bool empty() const {
        return _size == 0;
    }

    /*
     * @return storage space
     */
    size_t capacity() const {
        return _area.capacity();
    }

    V& operator[](const HashedKey& key) {
        return get(key);
    }
    V& operator[](const K_L& key) {
        return get(key);
    }

    V& get(const HashedKey& key);
    V& get(const K_L& key) {
        return get(HashedKey(key));
    }

    /**
     * @return number of elements removed
     */
    size_t erase(const HashedKey& key);
    size_t erase(const K_L& key) {
        if (empty())
            return 0;  // Don't waste time hashing.
        return erase(HashedKey(key));
    }

    class const_iterator {
        friend class UnorderedFastKeyTable;

    public:
        const_iterator() {
            _position = -1;
        }
        const_iterator(const Area* area) {
            _area = area;
            _position = 0;
            _max = _area->capacity() - 1;
            _skip();
        }
        const_iterator(const Area* area, int pos) {
            _area = area;
            _position = pos;
            _max = pos;
        }

        const value_type* operator->() const {
            return &_area->_entries[_position].data;
        }

        const value_type& operator*() const {
            return _area->_entries[_position].data;
        }

        const_iterator operator++() {
            if (_position < 0)
                return *this;
            _position++;
            _skip();
            return *this;
        }

        bool operator==(const const_iterator& other) const {
            return _position == other._position;
        }
        bool operator!=(const const_iterator& other) const {
            return _position != other._position;
        }

    private:
        void _skip() {
            while (true) {
                if (_position > _max) {
                    _position = -1;
                    break;
                }
                if (_area->_entries[_position].used)
                    break;
                ++_position;
            }
        }

        const Area* _area;
        int _position;
        int _max;  // inclusive
    };

    void erase(const_iterator it);

    /**
     * @return either a one-shot iterator with the key, or end()
     */
    const_iterator find(const K_L& key) const {
        if (empty())
            return end();  // Don't waste time hashing.
        return find(HashedKey(key));
    }

    const_iterator find(const HashedKey& key) const {
        if (empty())
            return end();
        return const_iterator(&_area, _area.find(key, nullptr));
    }

    const_iterator begin() const {
        return const_iterator(&_area);
    }

    const_iterator end() const {
        return const_iterator();
    }

private:
    void _grow();

    size_t _size = 0;
    Area _area;
};
}

#include "mongo/util/unordered_fast_key_table_internal.h"
