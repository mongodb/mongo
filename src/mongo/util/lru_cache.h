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

#include <boost/optional.hpp>
#include <cstdlib>
#include <iterator>
#include <list>

#include "mongo/stdx/unordered_map.h"

namespace mongo {

/**
 * A caching structure with a least recently used (LRU) replacement policy.
 * The number of entries allowed in the cache is set upon construction.
 *
 * This cache is not thread safe.
 *
 * Internally, this structure holds two containers: a list for LRU ordering and an
 * unordered_map for fast lookup. The add(), get(), and remove() operations are all O(1).
 *
 * Iteration over the cache will visit elements in order of last use, from most
 * recently used to least recently used.
 */
template <typename K,
          typename V,
          typename Hash = typename stdx::unordered_map<K, V>::hasher,
          typename KeyEqual = typename stdx::unordered_map<K, V, Hash>::key_equal>
class LRUCache {
    LRUCache(const LRUCache&) = delete;
    LRUCache& operator=(const LRUCache&) = delete;

    LRUCache(LRUCache&&) = delete;
    LRUCache& operator=(LRUCache&&) = delete;

public:
    explicit LRUCache(std::size_t maxSize) : _maxSize(maxSize) {}

    using ListEntry = std::pair<K, V>;
    using List = std::list<ListEntry>;

    using iterator = typename List::iterator;
    using const_iterator = typename List::const_iterator;

    using Map = stdx::unordered_map<K, iterator, Hash, KeyEqual>;

    using key_type = K;
    using mapped_type = V;

    /**
     * Inserts a new entry into the cache. If the given key already exists in the cache,
     * then we will drop the old entry and replace it with the given new entry. The cache
     * takes ownership of the new entry.
     *
     * If the cache is full when add() is called, the least recently used entry will be
     * evicted from the cache and returned to the caller.
     *
     * This method does not provide the strong exception safe guarantee. If a call
     * to this method throws, the cache may be left in an inconsistent state.
     */
    boost::optional<std::pair<K, V>> add(const K& key, V entry) {
        // If the key already exists, delete it first.
        auto i = _map.find(key);
        if (i != _map.end()) {
            _list.erase(i->second);
        }

        _list.push_front(std::make_pair(key, std::move(entry)));
        _map[key] = _list.begin();

        // If the store has grown beyond its allowed size,
        // evict the least recently used entry.
        if (size() > _maxSize) {
            auto pair = std::move(_list.back());

            _map.erase(pair.first);
            _list.pop_back();

            invariant(size() <= _maxSize);
            return std::move(pair);
        }

        invariant(size() <= _maxSize);
        return boost::none;
    }

    /**
     * Finds an element in the cache by key.
     */
    iterator find(const K& key) {
        return promote(key);
    }

    /**
     * Finds and element in the cache by key, without promoting the found
     * element to be the least recently used.
     *
     * This method is meant for testing and other callers that wish to "observe"
     * items in the cache without actually using them. Using this method over
     * the find(...) method above will prevent the LRUCache from functioning
     * properly.
     */
    const_iterator cfind(const K& key) const {
        auto it = _map.find(key);
        // TODO(SERVER-28890): Remove the function-style cast when MSVC's
        // `std::list< ... >::iterator` implementation doesn't conflict with their `/Zc:ternary`
        // flag support .
        return (it == _map.end()) ? end() : const_iterator(it->second);
    }

    /**
     * Promotes the element matching the given key, if one exists in the cache,
     * to the least recently used element.
     */
    iterator promote(const K& key) {
        auto it = _map.find(key);
        return (it == _map.end()) ? end() : promote(it->second);
    }

    /**
     * Promotes the element pointed to by the given iterator to be the least
     * recently used element in the cache.
     */
    iterator promote(const iterator& iter) {
        if (iter == _list.end()) {
            return iter;
        }

        _list.splice(_list.begin(), _list, iter);
        return _list.begin();
    }

    /**
     * Promotes the element pointed to by the given const_iterator to be the
     * least recently used element in the cache.
     */
    const_iterator promote(const const_iterator& iter) {
        if (iter == _list.cend()) {
            return iter;
        }

        _list.splice(_list.begin(), _list, iter);
        return _list.begin();
    }

    /**
     * Removes the element in the cache stored for this key, if one
     * exists. Returns the count of elements erased.
     */
    typename Map::size_type erase(const K& key) {
        auto it = _map.find(key);
        if (it == _map.end()) {
            return 0;
        }

        _list.erase(it->second);
        _map.erase(it);
        return 1;
    }

    /**
     * Removes the element pointed to by the given iterator from this
     * cache, and returns an iterator to the next least recently used
     * element, or the end iterator, if no such element exists.
     */
    iterator erase(iterator it) {
        invariant(it != _list.end());
        invariant(_map.erase(it->first) == 1);
        return _list.erase(it);
    }

    /**
     * Removes all items from the cache.
     */
    void clear() {
        _map.clear();
        _list.clear();
    }

    /**
     * If the given key has a matching element stored in the cache, returns true.
     * Otherwise, returns false.
     */
    bool hasKey(const K& key) const {
        return _map.find(key) != _map.end();
    }

    /**
     * Returns the number of elements currently in the cache.
     */
    std::size_t size() const {
        return _list.size();
    }

    bool empty() const {
        return _list.empty();
    }

    /**
     * Returns an iterator pointing to the most recently used element in the cache.
     */
    iterator begin() {
        return _list.begin();
    }

    /**
     * Returns an iterator pointing past the least recently used element in the cache.
     */
    iterator end() {
        return _list.end();
    }

    /**
     * Returns a const_iterator pointing to the most recently used element in the cache.
     */
    const_iterator begin() const {
        return _list.begin();
    }

    /**
     * Returns a const_iterafor pointing past the least recently used element in the cache.
     */
    const_iterator end() const {
        return _list.end();
    }

    /**
     * Returns a const_iterator pointing to the most recently used element in the cache.
     */
    const_iterator cbegin() const {
        return _list.cbegin();
    }

    /**
     * Returns a const_iterator pointing past the least recently used element in the cache.
     */
    const_iterator cend() const {
        return _list.cend();
    }

    typename Map::size_type count(const K& key) const {
        return _map.count(key);
    }

private:
    // The maximum allowable number of entries in the cache.
    const std::size_t _maxSize;

    // (K, V) pairs are stored in this std::list. They are sorted in order
    // of use, where the front is the most recently used and the back is the
    // least recently used.
    List _list;

    // Maps from a key to the corresponding std::list entry.
    Map _map;
};

}  // namespace mongo
