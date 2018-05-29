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

#include <iterator>
#include <memory>
#include <type_traits>

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
    using value_type = std::pair<const K_S, V>;
    using key_type = K_L;
    using mapped_type = V;

    using HashedKey = typename Traits::HashedKey;

private:
    class Entry {
    public:
        Entry() = default;

        Entry(const Entry& other)
            : _used(other._used), _everUsed(other.everUsed), _curHash(other._curHash) {
            if (other.isUsed()) {
                new (&_data) value_type(other.getData());
            }
        }

        Entry& operator=(const Entry& other) {
            if (this == &other) {
                return *this;
            }

            if (isUsed()) {
                unUse();
            }

            _used = other._used;
            _everUsed = other._everUsed;
            _curHash = other._curHash;

            if (other.isUsed()) {
                new (&_data) value_type(other.getData());
            }

            return *this;
        }

        ~Entry() {
            if (isUsed()) {
                unUse();
            }
        }

        template <typename... Args>
        void emplaceData(const HashedKey& key, Args&&... args) {
            dassert(!isUsed());
            _used = true;
            _everUsed = true;
            _curHash = key.hash();
            new (&_data) value_type(std::piecewise_construct,
                                    std::forward_as_tuple(Traits::toStorage(key.key())),
                                    std::forward_as_tuple(std::forward<Args>(args)...));
        }

        bool isUsed() const {
            return _used;
        }

        bool wasEverUsed() const {
            return _everUsed;
        }

        uint32_t getCurHash() const {
            dassert(isUsed());
            return _curHash;
        }

        void unUse() {
            dassert(isUsed());
            _used = false;
            reinterpret_cast<value_type*>(&_data)->~value_type();
        }

        value_type& getData() {
            dassert(isUsed());
            return *reinterpret_cast<value_type*>(&_data);
        }

        const value_type& getData() const {
            dassert(isUsed());
            return *reinterpret_cast<const value_type*>(&_data);
        }

    private:
        bool _used = false;
        bool _everUsed = false;
        uint32_t _curHash;
        typename std::aligned_storage<sizeof(value_type),
                                      std::alignment_of<value_type>::value>::type _data;
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

    void clear() {
        *this = {};
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

    template <typename AreaPtr,
              typename reference = decltype(AreaPtr()->begin()->getData()),
              typename pointer = typename std::add_pointer<reference>::type>
    class iterator_impl
        : public std::
              iterator<std::forward_iterator_tag, value_type, std::ptrdiff_t, pointer, reference> {
        friend class UnorderedFastKeyTable;

    public:
        iterator_impl() {
            _position = -1;
        }
        iterator_impl(AreaPtr area) {
            _area = area;
            _position = 0;
            _max = _area->capacity() - 1;
            _skip();
        }
        iterator_impl(AreaPtr area, int pos) {
            _area = area;
            _position = pos;
            _max = pos;
        }

        template <typename... Args>
        iterator_impl(const iterator_impl<Args...>& other)
            : _area(other._area), _position(other._position), _max(other._max) {}

        pointer operator->() const {
            return &_area->_entries[_position].getData();
        }

        reference operator*() const {
            return _area->_entries[_position].getData();
        }

        iterator_impl& operator++() {
            if (_position < 0)
                return *this;
            _position++;
            _skip();
            return *this;
        }

        iterator_impl operator++(int) {
            iterator_impl before(*this);
            operator++();
            return before;
        }

        bool operator==(const iterator_impl& other) const {
            return _position == other._position;
        }
        bool operator!=(const iterator_impl& other) const {
            return _position != other._position;
        }

    private:
        void _skip() {
            while (true) {
                if (_position > _max) {
                    _position = -1;
                    break;
                }
                if (_area->_entries[_position].isUsed())
                    break;
                ++_position;
            }
        }

        AreaPtr _area;
        int _position;
        int _max;  // inclusive
    };

    using iterator = iterator_impl<Area*>;
    using const_iterator = iterator_impl<const Area*>;

    void erase(const_iterator it);

    /**
     * @return either a one-shot iterator with the key, or end()
     */
    const_iterator find(const K_L& key) const {
        if (empty())
            return end();  // Don't waste time hashing.
        return const_iterator(&_area, _area.find(HashedKey(key), nullptr));
    }

    const_iterator find(const HashedKey& key) const {
        if (empty())
            return end();
        return const_iterator(&_area, _area.find(key, nullptr));
    }

    iterator find(const K_L& key) {
        if (empty())
            return end();  // Don't waste time hashing.
        return iterator(&_area, _area.find(HashedKey(key), nullptr));
    }

    iterator find(const HashedKey& key) {
        if (empty())
            return end();
        return iterator(&_area, _area.find(key, nullptr));
    }

    size_t count(const K_L& key) const {
        if (empty())
            return 0;  // Don't waste time hashing.
        return _area.find(HashedKey(key), nullptr) != -1;
    }

    size_t count(const HashedKey& key) const {
        if (empty())
            return 0;
        return _area.find(key, nullptr) != -1;
    }

    const_iterator begin() const {
        return const_iterator(&_area);
    }

    const_iterator end() const {
        return const_iterator();
    }

    iterator begin() {
        return iterator(&_area);
    }

    iterator end() {
        return iterator();
    }

    const_iterator cbegin() const {
        return const_iterator(&_area);
    }

    const_iterator cend() const {
        return const_iterator();
    }

    template <typename... Args>
    std::pair<iterator, bool> try_emplace(const HashedKey& key, Args&&... args);

    template <typename... Args>
    std::pair<iterator, bool> try_emplace(const K_L& key, Args&&... args) {
        return try_emplace(HashedKey(key), std::forward<Args>(args)...);
    }

    void swap(UnorderedFastKeyTable& other) {
        _area.swap(&(other._area));
        std::swap(_size, other._size);
    }

    friend void swap(UnorderedFastKeyTable& lhs, UnorderedFastKeyTable& rhs) {
        return lhs.swap(rhs);
    }

private:
    void _grow();

    size_t _size = 0;
    Area _area;
};
}

#include "mongo/util/unordered_fast_key_table_internal.h"
