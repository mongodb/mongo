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

namespace mongo {

template <typename K_L, typename K_S>
struct UnorderedFastKeyTable_LS_C {
    K_S operator()(const K_L& a) const {
        return K_S(a);
    }
};

template <typename K_L,                                         // key lookup
          typename K_S,                                         // key storage
          typename V,                                           // value
          typename H,                                           // hash of K_L
          typename E,                                           // equal of K_L
          typename C,                                           // convertor from K_S -> K_L
          typename C_LS = UnorderedFastKeyTable_LS_C<K_L, K_S>  // convertor from K_L -> K_S
          >
class UnorderedFastKeyTable {
public:
    typedef std::pair<K_S, V> value_type;
    typedef K_L key_type;
    typedef V mapped_type;

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
        Area(unsigned capacity, unsigned maxProbe);
        Area(const Area& other);

        int find(const K_L& key,
                 uint32_t hash,
                 int* firstEmpty,
                 const UnorderedFastKeyTable& sm) const;

        bool transfer(Area* newArea, const UnorderedFastKeyTable& sm) const;

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

    UnorderedFastKeyTable(const UnorderedFastKeyTable& other);

    UnorderedFastKeyTable(std::initializer_list<std::pair<key_type, mapped_type>> entries);

    UnorderedFastKeyTable& operator=(const UnorderedFastKeyTable& other) {
        other.copyTo(this);
        return *this;
    }

    void copyTo(UnorderedFastKeyTable* out) const;

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

    V& operator[](const K_L& key) {
        return get(key);
    }

    V& get(const K_L& key);

    /**
     * @return number of elements removed
     */
    size_t erase(const K_L& key);

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
    const_iterator find(const K_L& key) const;

    const_iterator begin() const;

    const_iterator end() const;

private:
    void _grow();

    // ----

    size_t _size = 0;
    Area _area;

    H _hash;
    E _equals;
    C _convertor;
    C_LS _convertorOther;
};
}

#include "mongo/util/unordered_fast_key_table_internal.h"
