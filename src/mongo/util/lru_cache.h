/**
 *    Copyright (C) 2017 MongoDB, Inc.
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

#include <cstdlib>
#include <iterator>
#include <list>

#include <boost/optional.hpp>

#include "mongo/base/disallow_copying.h"
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
          typename Hash = std::hash<K>,
          typename KeyEqual = std::equal_to<K>>
class LRUCache {
    MONGO_DISALLOW_COPYING(LRUCache);

public:
    explicit LRUCache(std::size_t maxSize) : _maxSize(maxSize) {}

    LRUCache(LRUCache&&) = delete;
    LRUCache& operator=(LRUCache&&) = delete;

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
    boost::optional<V> add(const K& key, V entry) {
        // If the key already exists, delete it first.
        auto i = this->_map.find(key);
        if (i != this->_map.end()) {
            this->_list.erase(i->second);
        }

        this->_list.push_front(std::make_pair(key, std::move(entry)));
        this->_map[key] = this->_list.begin();

        // If the store has grown beyond its allowed size,
        // evict the least recently used entry.
        if (this->size() > this->_maxSize) {
            auto pair = std::move(this->_list.back());
            auto result = std::move(pair.second);

            this->_map.erase(pair.first);
            this->_list.pop_back();

            invariant(this->size() <= this->_maxSize);
            return std::move(result);
        }

        invariant(this->size() <= this->_maxSize);
        return boost::none;
    }

    /**
     * Finds an element in the cache by key.
     */
    iterator find(const K& key) {
        return this->promote(key);
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
        auto it = this->_map.find(key);
        return (it == this->_map.end()) ? this->end() : it->second;
    }

    /**
     * Promotes the element matching the given key, if one exists in the cache,
     * to the least recently used element.
     */
    iterator promote(const K& key) {
        auto it = this->_map.find(key);
        return (it == this->_map.end()) ? this->end() : this->promote(it->second);
    }

    /**
     * Promotes the element pointed to by the given iterator to be the least
     * recently used element in the cache.
     */
    iterator promote(const iterator& iter) {
        if (iter == this->_list.end()) {
            return iter;
        }

        this->_list.splice(this->_list.begin(), this->_list, iter);
        return this->_list.begin();
    }

    /**
     * Promotes the element pointed to by the given const_iterator to be the
     * least recently used element in the cache.
     */
    const_iterator promote(const const_iterator& iter) {
        if (iter == this->_list.cend()) {
            return iter;
        }

        this->_list.splice(this->_list.begin(), this->_list, iter);
        return this->_list.begin();
    }

    /**
     * Removes the element in the cache stored for this key, if one
     * exists. Returns the count of elements erased.
     */
    typename Map::size_type erase(const K& key) {
        auto it = this->_map.find(key);
        if (it == this->_map.end()) {
            return 0;
        }

        this->_list.erase(it->second);
        this->_map.erase(it);
        return 1;
    }

    /**
     * Removes the element pointed to by the given iterator from this
     * cache, and returns an iterator to the next least recently used
     * element, or the end iterator, if no such element exists.
     */
    iterator erase(iterator it) {
        invariant(it != this->_list.end());
        invariant(this->_map.erase(it->first) == 1);
        return this->_list.erase(it);
    }

    /**
     * Removes all items from the cache.
     */
    void clear() {
        this->_map.clear();
        this->_list.clear();
    }

    /**
     * If the given key has a matching element stored in the cache, returns true.
     * Otherwise, returns false.
     */
    bool hasKey(const K& key) const {
        return _map.find(key) != this->_map.end();
    }

    /**
     * Returns the number of elements currently in the cache.
     */
    std::size_t size() const {
        return this->_list.size();
    }

    bool empty() const {
        return this->_list.empty();
    }

    /**
     * Returns an iterator pointing to the most recently used element in the cache.
     */
    iterator begin() {
        return this->_list.begin();
    }

    /**
     * Returns an iterator pointing past the least recently used element in the cache.
     */
    iterator end() {
        return this->_list.end();
    }

    /**
     * Returns a const_iterator pointing to the most recently used element in the cache.
     */
    const_iterator begin() const {
        return this->_list.begin();
    }

    /**
     * Returns a const_iterafor pointing past the least recently used element in the cache.
     */
    const_iterator end() const {
        return this->_list.end();
    }

    /**
     * Returns a const_iterator pointing to the most recently used element in the cache.
     */
    const_iterator cbegin() const {
        return this->_list.cbegin();
    }

    /**
     * Returns a const_iterator pointing past the least recently used element in the cache.
     */
    const_iterator cend() const {
        return this->_list.cend();
    }

    typename Map::size_type count(const K& key) const {
        return this->_map.count(key);
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
