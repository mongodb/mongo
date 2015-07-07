/**
 *    Copyright (C) 2014 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/platform/unordered_map.h"
#include "mongo/util/assert_util.h"

namespace mongo {

/**
 * NOTE: This structure should not be used for anything other than the Lock Manager.
 *
 * This is a simple implementation of an unordered associative array with minimal
 * functionality, used by the lock manager. It keeps a small number of memory entries to store
 * values, in order to avoid memory allocations, which dominate the cost of the lock manager
 * calls by a wide margin.
 *
 * This class is not thread-safe.
 */
template <class KeyType, class ValueType, int PreallocCount>
class FastMapNoAlloc {
public:
    /**
     * Forward-only iterator. Does not synchronize with the underlying collection in any way.
     * In other words, do not modify the collection while there is an open iterator on it.
     */
    template <class MapType, class IteratorValueType>
    class IteratorImpl {
    public:
        IteratorImpl(const IteratorImpl& other) : _map(other._map), _idx(other._idx) {}


        //
        // Operators
        //

        bool operator!() const {
            return finished();
        }

        IteratorValueType& operator*() const {
            return *objAddr();
        }

        IteratorValueType* operator->() const {
            return objAddr();
        }


        //
        // Other methods
        //

        /**
         * Returns whether the iterator has been exhausted through calls to next. This value
         * can be used to determine whether a previous call to find has found something.
         */
        bool finished() const {
            return (MONGO_unlikely(_idx == PreallocCount));
        }

        /**
         * Returns the address of the object at the current position. Cannot be called with an
         * uninitialized iterator, or iterator which has reached the end.
         */
        IteratorValueType* objAddr() const {
            invariant(!finished());

            return &_map._fastAccess[_idx].value;
        }

        /**
         * Returns the key of the value at the current position. Cannot be called with an
         * uninitialized iterator or iterator which has reached the end.
         */
        const KeyType& key() const {
            invariant(!finished());

            return _map._fastAccess[_idx].key;
        }

        /**
         * Advances the iterator to the next entry. No particular order of iteration is
         * guaranteed.
         */
        void next() {
            invariant(!finished());

            while (++_idx < PreallocCount) {
                if (_map._fastAccess[_idx].inUse) {
                    return;
                }
            }
        }

        /**
         * Removes the element at the current position and moves the iterator to the next,
         * which might be the last entry on the map.
         */
        void remove() {
            invariant(!finished());
            invariant(_map._fastAccess[_idx].inUse);

            _map._fastAccess[_idx].inUse = false;
            _map._fastAccessUsedSize--;

            next();
        }


    private:
        friend class FastMapNoAlloc<KeyType, ValueType, PreallocCount>;

        // Used for iteration of the complete map
        IteratorImpl(MapType& map) : _map(map), _idx(-1) {
            next();
        }

        // Used for iterator starting at a position
        IteratorImpl(MapType& map, int idx) : _map(map), _idx(idx) {
            invariant(_idx >= 0);
        }

        // Used for iteration starting at a particular key
        IteratorImpl(MapType& map, const KeyType& key) : _map(map), _idx(0) {
            while (_idx < PreallocCount) {
                if (_map._fastAccess[_idx].inUse && (_map._fastAccess[_idx].key == key)) {
                    return;
                }

                ++_idx;
            }
        }


        // The map being iterated on
        MapType& _map;

        // Index to the current entry being iterated
        int _idx;
    };


    typedef IteratorImpl<FastMapNoAlloc<KeyType, ValueType, PreallocCount>, ValueType> Iterator;

    typedef IteratorImpl<const FastMapNoAlloc<KeyType, ValueType, PreallocCount>, const ValueType>
        ConstIterator;


    FastMapNoAlloc() : _fastAccess(), _fastAccessUsedSize(0) {}

    /**
     * Inserts the specified entry in the map and returns a reference to the memory for the
     * entry just inserted.
     */
    Iterator insert(const KeyType& key) {
        // Find the first unused slot. This could probably be even further optimized by adding
        // a field pointing to the first unused location.
        int idx = 0;
        for (; _fastAccess[idx].inUse; idx++)
            ;

        invariant(idx < PreallocCount);

        _fastAccess[idx].inUse = true;
        _fastAccess[idx].key = key;
        _fastAccessUsedSize++;

        return Iterator(*this, idx);
    }

    /**
     * Returns an iterator to the first element in the map.
     */
    Iterator begin() {
        return Iterator(*this);
    }

    ConstIterator begin() const {
        return ConstIterator(*this);
    }

    /**
     * Returns an iterator pointing to the first position, which has entry with the specified
     * key. Before dereferencing the returned iterator, it should be checked for validity using
     * the finished() method or the ! operator. If no element was found, finished() will return
     * false.
     *
     * While it is allowed to call next() on the returned iterator, this is not very useful,
     * because the container is not ordered.
     */
    Iterator find(const KeyType& key) {
        return Iterator(*this, key);
    }

    ConstIterator find(const KeyType& key) const {
        return ConstIterator(*this, key);
    }

    int size() const {
        return _fastAccessUsedSize;
    }
    bool empty() const {
        return (_fastAccessUsedSize == 0);
    }

private:
    // Empty and very large maps do not make sense since there will be no performance gain, so
    // disallow them.
    static_assert(PreallocCount > 0, "PreallocCount > 0");
    static_assert(PreallocCount < 32, "PreallocCount < 32");

    // Iterator accesses the map directly
    friend class IteratorImpl<FastMapNoAlloc<KeyType, ValueType, PreallocCount>, ValueType>;

    friend class IteratorImpl<const FastMapNoAlloc<KeyType, ValueType, PreallocCount>,
                              const ValueType>;


    struct PreallocEntry {
        PreallocEntry() : inUse(false) {}

        bool inUse;

        KeyType key;
        ValueType value;
    };

    // Pre-allocated memory for entries
    PreallocEntry _fastAccess[PreallocCount];
    int _fastAccessUsedSize;
};

}  // namespace mongo
