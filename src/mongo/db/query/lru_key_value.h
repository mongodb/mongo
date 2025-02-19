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
#include <cstddef>
#include <fmt/format.h>
#include <functional>
#include <list>
#include <memory>
#include <utility>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"

namespace mongo {

/**
 * 'InsertionEvictionListener' class to use with 'LRUBudgetTracker' that will always noop.
 */
class NoopInsertionEvictionListener {
public:
    // Called when a key-value pair is being inserted. Parameters are the key-value pair and its
    // estimated size.
    template <class K, class V>
    void onInsert(const K&, const V&, size_t) {}

    // Called when a key-value pair is being evicted. Parameters are the key-value pair and its
    // estimated size.
    template <class K, class V>
    void onEvict(const K&, const V&, size_t) {}

    // Called when the cache is being cleared. Parameter is the estimated size of the key-value
    // pairs in the cache before it was cleared.
    void onClear(size_t) {}
};

/**
 * This class tracks a size of entries in 'LRUKeyValue'.
 * The size can be understood as a number of the entries, an amount of memory they occupied,
 * or any other value defined by the template parameter 'Estimator'.
 * The 'Estimator' must be deterministic and always return the same value for the same entry.
 * The 'InsertionEvictionListener' will be called on every insertion and eviction as well as when
 * the cache is cleared.
 */
template <class K, class V, typename Estimator, typename InsertionEvictionListener>
class LRUBudgetTracker {
public:
    LRUBudgetTracker(size_t maxBudget) : _max(maxBudget), _current(0) {}

    void onAdd(const K& k, const V& v) {
        size_t budget = _estimator(k, v);
        _current += budget;
        _listener.onInsert(k, v, budget);
    }

    void onRemove(const K& k, const V& v) {
        using namespace fmt::literals;
        size_t budget = _estimator(k, v);
        tassert(5968300,
                "LRU budget underflow: current={}, budget={} "_format(_current, budget),
                _current >= budget);
        _current -= budget;
        _listener.onEvict(k, v, budget);
    }

    void onClear() {
        _listener.onClear(_current);
        _current = 0;
    }

    // Returns true if the cache runs over budget.
    bool isOverBudget() const {
        return _current > _max;
    }

    size_t currentBudget() const {
        return _current;
    }

    void reset(size_t newMaxSize) {
        _max = newMaxSize;
    }

private:
    size_t _max;
    size_t _current;
    Estimator _estimator;
    InsertionEvictionListener _listener;
};

/**
 * A key-value store structure with a least recently used (LRU) replacement
 * policy. The size allowed in the kv-store is controlled by 'LRUBudgetTracker'
 * set in the constructor.
 *
 * An 'InsertionEvictionListener' may optionally be specified to track the insertion and eviction of
 * each key-value pair.
 *
 * Caveat:
 * This kv-store is NOT thread safe! The client to this utility is responsible
 * for protecting concurrent access to the LRU store if used in a threaded
 * context.
 *
 * Implemented as a doubly-linked list with a hash map for quickly locating the kv-store entries.
 * The add(), get(), and remove() operations are all O(1).
 *
 * TODO: We could move this into the util/ directory and do any cleanup necessary to make it
 * fully general.
 */
template <class K,
          class V,
          class KeyValueBudgetEstimator,
          class InsertionEvictionListener = NoopInsertionEvictionListener,
          class KeyHasher = std::hash<K>,
          class KeyEq = std::equal_to<K>>
class LRUKeyValue {
public:
    /** A hasher wrapper class that converts references to pointers and enables 'KVMap' to store
     * pointers.
     */
    template <typename Hasher>
    struct HasherWrapper {
        std::size_t operator()(const K* key) const {
            return hasher(*key);
        }

        Hasher hasher;
    };

    /**
     * An equality wrapper class that converts references to pointers and enables 'KVMap' to store
     * pointers.
     */
    template <typename Eq>
    struct EqWrapper {
        bool operator()(const K* lhs, const K* rhs) const {
            return eq(*lhs, *rhs);
        }

        Eq eq;
    };

    typedef std::pair<K, V> KVListEntry;

    typedef std::list<KVListEntry> KVList;
    typedef typename KVList::iterator KVListIt;
    typedef typename KVList::const_iterator KVListConstIt;

    typedef stdx::unordered_map<const K*, KVListIt, HasherWrapper<KeyHasher>, EqWrapper<KeyEq>>
        KVMap;
    typedef typename KVMap::const_iterator KVMapConstIt;

    // These type declarations are required by the 'Partitioned' utility.
    using key_type = K;
    using mapped_type = typename KVMap::mapped_type;
    using value_type = typename KVMap::value_type;

    LRUKeyValue(size_t maxSize) : _budgetTracker{maxSize} {}

    ~LRUKeyValue() {
        clear();
    }

    /**
     * Add an (K, V) pair to the store, where 'key' can be used to retrieve value 'entry' from the
     * store. If 'key' already exists in the kv-store, 'entry' will simply replace what is already
     * there. If after the add() operation the kv-store exceeds its budget, then the least recently
     * used entries will be evicted until the size is again under-budget. Returns the number of
     * evicted entries.
     */
    size_t add(const K& key, V entry) {
        KVMapConstIt i = _kvMap.find(&key);
        if (i != _kvMap.end()) {
            KVListIt found = i->second;
            _budgetTracker.onRemove(key, found->second);
            _kvMap.erase(i);
            _kvList.erase(found);
        }

        _budgetTracker.onAdd(key, entry);
        auto& newEntry = _kvList.emplace_front(std::make_pair(key, std::move(entry)));
        _kvMap[&newEntry.first] = _kvList.begin();

        return evict();
    }

    /**
     * Retrieve the iterator to the value associated with 'key' from the kv-store. Note that this
     * iterator returned is only guaranteed to be valid until the next call to any method in this
     * class. As a side effect, the retrieved entry is promoted to the most recently used.
     */
    StatusWith<KVListIt> get(const K& key) const {
        KVMapConstIt i = _kvMap.find(&key);
        if (i == _kvMap.end()) {
            return Status(ErrorCodes::NoSuchKey, "no such key in LRU key-value store");
        }
        KVListIt found = i->second;

        // Promote the kv-store entry to the front of the list. It is now the most recently used.
        const auto& newEntry = _kvList.emplace_front(key, std::move(found->second));
        _kvMap.erase(i);
        _kvList.erase(found);
        _kvMap[&newEntry.first] = _kvList.begin();

        return _kvList.begin();
    }

    /**
     * Remove the kv-store entry keyed by 'key'.
     * Returns false if there doesn't exist such 'key', otherwise returns true.
     */
    bool erase(const K& key) {
        KVMapConstIt i = _kvMap.find(&key);
        if (i == _kvMap.end()) {
            return false;
        }
        KVListIt found = i->second;
        _budgetTracker.onRemove(key, found->second);
        _kvMap.erase(i);
        _kvList.erase(found);
        return true;
    }

    /**
     * Remove all the entries for keys for which the predicate returns true. Returns the number of
     * removed entries.
     */
    template <typename KeyValuePredicate>
    size_t removeIf(KeyValuePredicate predicate) {
        size_t removed = 0;
        for (auto it = _kvList.begin(); it != _kvList.end();) {
            if (predicate(it->first, *it->second)) {
                _budgetTracker.onRemove(it->first, it->second);
                _kvMap.erase(&it->first);
                it = _kvList.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        return removed;
    }

    /**
     * Deletes all entries in the kv-store.
     */
    void clear() {
        _kvMap.clear();
        _kvList.clear();
        _budgetTracker.onClear();
    }

    /**
     * Reset the kv-store with new budget tracker. Returns the number of evicted entries.
     */
    size_t reset(size_t newMaxSize) {
        _budgetTracker.reset(newMaxSize);
        return evict();
    }

    /**
     * Returns true if entry is found in the kv-store.
     */
    bool hasKey(const K& key) const {
        return _kvMap.find(&key) != _kvMap.end();
    }

    /**
     * Returns the size (current budget) of the kv-store.
     */
    size_t size() const {
        return _budgetTracker.currentBudget();
    }

    /**
     * TODO: The kv-store should implement its own iterator. Calling through to the underlying
     * iterator exposes the internals, and forces the caller to make a horrible type declaration.
     */
    KVListConstIt begin() const {
        return _kvList.begin();
    }

    KVListConstIt end() const {
        return _kvList.end();
    }

private:
    /**
     * If the kv-store is over its budget this function evicts the least recently used entries until
     * the size is again under-budget. Returns the number of evicted entries
     */
    size_t evict() {
        size_t nEvicted = 0;
        while (_budgetTracker.isOverBudget()) {
            invariant(!_kvList.empty());

            _budgetTracker.onRemove(_kvList.back().first, _kvList.back().second);
            _kvMap.erase(&_kvList.back().first);
            _kvList.pop_back();

            ++nEvicted;
        }

        return nEvicted;
    }

    LRUBudgetTracker<K, V, KeyValueBudgetEstimator, InsertionEvictionListener> _budgetTracker;

    // (K, V) pairs are stored in this std::list. They are sorted in order of use, where the front
    // is the most recently used and the back is the least recently used.
    mutable KVList _kvList;

    // Maps from a key to the corresponding std::list entry.
    mutable KVMap _kvMap;
};

}  // namespace mongo
