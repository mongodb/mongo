/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <deque>

#include "mongo/base/static_assert.h"
#include "mongo/util/assert_util.h"

namespace mongo {

/**
 * NOTE: This structure should not be used for anything other than the Lock Manager.
 *
 * This is a simple implementation of an unordered associative array with minimal functionality,
 * used by the lock manager. It keeps a small number of memory entries to store values, in order to
 * avoid memory allocations, which dominate the cost of the lock manager calls by a wide margin.
 *
 * This class is not thread-safe.
 *
 * Note: this custom data structure is necessary because we need: fast memory access; to maintain
 * all data pointer/reference validity when entries are added/removed; and to avoid costly and
 * repetitive entry mallocs and frees.
 */
template <class KeyType, class ValueType>
class FastMapNoAlloc {
private:
    /**
     * Map entry through which we avoid releasing memory: we mark it as inUse or not.
     * Maps keys to values.
     */
    struct PreallocEntry {
        bool inUse = false;

        KeyType key;
        ValueType value;
    };

    typedef typename std::deque<PreallocEntry> Container;

    typedef typename Container::size_type size_type;

    typedef typename Container::iterator map_iterator;

    typedef typename Container::const_iterator const_map_iterator;


    /**
     * Forward-only iterator. Does not synchronize with the underlying collection in any way.
     * In other words, do not modify the collection while there is an open iterator on it.
     */
    template <class MapType, class IteratorValueType, class IteratorType>
    class IteratorImpl {
    public:
        //
        // Operators
        //

        operator bool() const {
            return !finished();
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
            return (_it == _map._fastAccess.end());
        }

        /**
         * Returns the address of the object at the current position. Cannot be called with an
         * uninitialized iterator, or iterator which has reached the end.
         */
        IteratorValueType* objAddr() const {
            invariant(!finished());

            return &(_it->value);
        }

        /**
         * Returns the key of the value at the current position. Cannot be called with an
         * uninitialized iterator or iterator which has reached the end.
         */
        const KeyType& key() const {
            invariant(!finished());

            return _it->key;
        }

        /**
         * Advances the iterator to the next entry. No particular order of iteration is
         * guaranteed.
         */
        void next() {
            invariant(!finished());
            while (++_it != _map._fastAccess.end()) {
                if (_it->inUse) {
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
            invariant(_it->inUse);

            _it->inUse = false;
            _map._fastAccessUsedSize--;

            next();
        }


    private:
        friend class FastMapNoAlloc<KeyType, ValueType>;

        // Used for iteration of the complete map
        IteratorImpl(MapType& map) : _map(map), _it(map._fastAccess.begin()) {
            while (_it != _map._fastAccess.end()) {
                if (_it->inUse) {
                    return;
                }
                ++_it;
            }
        }

        // Used for iterator starting at a position
        IteratorImpl(MapType& map, IteratorType it) : _map(map), _it(std::move(it)) {
            invariant(_it != _map._fastAccess.end());
        }

        // Used for iteration starting at a particular key
        IteratorImpl(MapType& map, const KeyType& key) : _map(map), _it(_map._fastAccess.begin()) {
            while (_it != _map._fastAccess.end()) {
                if (_it->inUse && _it->key == key) {
                    return;
                }

                ++_it;
            }
        }


        // The map being iterated on
        MapType& _map;

        // Iterator on the map
        IteratorType _it;
    };

public:
    typedef IteratorImpl<FastMapNoAlloc<KeyType, ValueType>, ValueType, map_iterator> Iterator;

    typedef IteratorImpl<const FastMapNoAlloc<KeyType, ValueType>,
                         const ValueType,
                         const_map_iterator>
        ConstIterator;

    FastMapNoAlloc() : _fastAccessUsedSize(0) {}

    /**
     * Inserts the specified entry in the map and returns a reference to the memory for the
     * entry just inserted.
     */
    Iterator insert(const KeyType& key) {
        if (_fastAccessUsedSize == _fastAccess.size()) {
            // Place the new entry in the front so the below map iteration is faster.
            _fastAccess.emplace_front();
        }

        map_iterator it = _fastAccess.begin();
        while (it != _fastAccess.end() && it->inUse) {
            ++it;
        }

        invariant(it != _fastAccess.end() && !(it->inUse));

        it->inUse = true;
        it->key = key;
        ++_fastAccessUsedSize;

        return Iterator(*this, it);
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
     * true.
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

    size_t size() const {
        return _fastAccessUsedSize;
    }
    bool empty() const {
        return (_fastAccessUsedSize == 0);
    }

private:
    // We chose a deque data structure because it maintains the validity of existing
    // pointers/references to its contents when it allocates more memory. Deque also gives us O(1)
    // emplace_front() in insert().
    std::deque<PreallocEntry> _fastAccess;

    size_type _fastAccessUsedSize;
};

}  // namespace mongo
