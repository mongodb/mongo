// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <deque>
#include <utility>

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
 * repetitive entry mallocs and frees (although the mallocs do occur within the deque, powering the
 * data structure).
 *
 * Asymptotics:
 *   Time:
 * - Insertion: avg O(N)
 *   Since in case there is any element deleted from the data structure, we would first
 *   iterate over all entries, in order to proceed with drop-in replacement
 * - Lookup by key: avg O(N)
 *   We are iterating over entire deque in search of the same key, no order is guaranteed
 * - Remove (supported only by iterator): O(1)
 *
 *   Memory:
 *   O(N), but due to drop-in replacement of elements, guaranteed to equal amount of active elements
 */
template <class KeyType, class ValueType>
class [[MONGO_MOD_PRIVATE]] FastMapNoAlloc {
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
