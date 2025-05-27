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

#include <boost/intrusive_ptr.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>
#include <boost/optional.hpp>
#include <boost/tuple/tuple.hpp>
// IWYU pragma: no_include "boost/multi_index/detail/bidir_node_iterator.hpp"
// IWYU pragma: no_include "boost/multi_index/detail/hash_index_iterator.hpp"
// IWYU pragma: no_include "boost/multi_index/detail/index_node_base.hpp"
#include "mongo/base/string_data_comparator.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cstddef>
#include <functional>
#include <iterator>
#include <utility>
#include <vector>

#include <boost/multi_index/indexed_by.hpp>
#include <boost/multi_index_container_fwd.hpp>
#include <boost/operators.hpp>
#include <boost/tuple/tuple.hpp>

namespace mongo {

using boost::multi_index_container;
using boost::multi_index::hashed_unique;
using boost::multi_index::indexed_by;
using boost::multi_index::member;
using boost::multi_index::sequenced;

/**
 * A least-recently-used cache from key to a vector of values. It does not implement any default
 * size limit, but includes the ability to evict down to both a specific number of elements, and
 * down to a specific amount of memory. Memory usage includes only the size of the elements in the
 * cache at the time of insertion, not the overhead incurred by the data structures in use.
 */
class LookupSetCache {
public:
    struct Cached {
        Value key;
        std::vector<Document> docs;

        // This includes the size of both key and the documents.
        size_t approxCacheEntrySize = 0;
    };

    // boost::multi_index_container provides a system for implementing a cache. Here, we create
    // a container of 'Cached', that is both sequenced, and has a unique index on the 'Cached::key'.
    // From this, we are able to evict the least-recently-used member, and maintain key uniqueness.
    using IndexedContainer =
        multi_index_container<Cached,
                              indexed_by<sequenced<>,
                                         hashed_unique<member<Cached, Value, &Cached::key>,
                                                       ValueComparator::Hasher,
                                                       ValueComparator::EqualTo>>>;

    /**
     * Constructs the underlying cache data structure in such a way that respects the
     * ValueComparator. This requires instantiating the multi_index_container with comparison and
     * hasher functions obtained from the comparator.
     */
    explicit LookupSetCache(const ValueComparator& comparator)
        : _container(boost::make_tuple(IndexedContainer::nth_index<0>::type::ctor_args(),
                                       boost::make_tuple(0,
                                                         member<Cached, Value, &Cached::key>(),
                                                         comparator.getHasher(),
                                                         comparator.getEqualTo()))) {}

    /**
     * Insert "value" into the set with key "key". If "key" is already present in the cache, move it
     * to the middle of the cache. Otherwise, insert a new key in the middle of the cache.
     *
     * Note: In this case, "middle" refers to the sequence of the cache, where "first" is the item
     * most recently used, and "last" is the item least recently used.
     *
     * We insert and update in the middle because when a key is updated, we can't assume that it's
     * important to keep in the cache (i.e., that we should put it at the front), but it's also
     * likely we don't want to evict it (i.e., we want to make sure it isn't at the back).
     */
    void insert(Value key, Document doc) {
        // Get an iterator to the middle of the container.
        size_t middle = size() / 2;
        auto it = _container.begin();
        std::advance(it, middle);
        const auto keySize = key.getApproximateSize();
        auto cacheEntrySizeIncreaseBy = doc.getApproximateSize();

        // Find the cache entry, or create one if it doesn't exist yet.
        auto insertionResult = _container.insert(it, {std::move(key), {}, 0});
        if (insertionResult.second) {
            cacheEntrySizeIncreaseBy += keySize;
        } else {
            // We did not insert due to a duplicate key. Update the cached doc, moving it to the
            // middle of the cache.
            _container.relocate(it, insertionResult.first);
        }

        // Add the doc to the cache entry.
        _container.modify(insertionResult.first, [&doc, cacheEntrySizeIncreaseBy](Cached& entry) {
            entry.docs.push_back(std::move(doc));
            entry.approxCacheEntrySize += cacheEntrySizeIncreaseBy;
        });
        _memoryUsage += cacheEntrySizeIncreaseBy;
    }

    /**
     * Evict the least-recently-used item.
     */
    void evictOne() {
        if (_container.empty()) {
            return;
        }

        const auto cacheEntrySize = _container.back().approxCacheEntrySize;
        tassert(5429600,
                str::stream() << "The cache entry size: " << cacheEntrySize
                              << ", cannot be larger than entire cache size: " << _memoryUsage,
                cacheEntrySize <= _memoryUsage);
        _memoryUsage -= cacheEntrySize;

        _container.erase(std::prev(_container.end()));
    }

    /**
     * Evicts from the cache until there are 'num' items remaining.
     */
    void evictUntilSize(size_t num) {
        while (size() > num) {
            evictOne();
        }
    }

    /**
     * Returns the number of elements in the cache.
     */
    size_t size() const {
        return _container.size();
    }

    /**
     * Evict items in LRU order until the cache's size is less than or equal to "maximum".
     */
    void evictDownTo(size_t maximum) {
        while (_memoryUsage > maximum && !_container.empty()) {
            evictOne();
        }
    }

    /**
     * Clear the cache, resetting the memory usage.
     */
    void clear() {
        _container.clear();
        _memoryUsage = 0;
    }

    /**
     * Retrieve the vector of values with key "key". Returns nullptr if not found.
     */
    const std::vector<Document>* operator[](const Value& key) {
        auto it = boost::multi_index::get<1>(_container).find(key);
        if (it != boost::multi_index::get<1>(_container).end()) {
            boost::multi_index::get<0>(_container)
                .relocate(boost::multi_index::get<0>(_container).begin(),
                          boost::multi_index::project<0>(_container, it));
            return &it->docs;
        }
        return nullptr;
    }

    auto getMemoryUsage() const {
        return _memoryUsage;
    }

private:
    IndexedContainer _container;

    size_t _memoryUsage = 0;
};

}  // namespace mongo
