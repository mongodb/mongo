/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include <boost/intrusive_ptr.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>
#include <boost/optional.hpp>
#include <iostream>
#include <unordered_map>
#include <unordered_set>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/db/pipeline/value_comparator.h"
#include "mongo/stdx/functional.h"

namespace mongo {

using boost::multi_index_container;
using boost::multi_index::sequenced;
using boost::multi_index::hashed_unique;
using boost::multi_index::member;
using boost::multi_index::indexed_by;

/**
 * A least-recently-used cache from key to a set of values. It does not implement any default size
 * limit, but includes the ability to evict down to both a specific number of elements, and down to
 * a specific amount of memory. Memory usage includes only the size of the elements in the cache at
 * the time of insertion, not the overhead incurred by the data structures in use.
 *
 * TODO SERVER-23349: This class must make all comparisons of user data using the aggregation
 * operation's collation.
 */
class LookupSetCache {
public:
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
    void insert(Value key, BSONObj value) {
        // Get an iterator to the middle of the container.
        size_t middle = size() / 2;
        auto it = _container.begin();
        std::advance(it, middle);

        auto result = _container.insert(it, {key, {value}});

        if (!result.second) {
            // We did not insert due to a duplicate key.
            auto cached = *result.first;
            // Update the cached value, moving it to the middle of the cache.
            cached.second.insert(value);
            _container.replace(result.first, cached);
            _container.relocate(it, result.first);
        } else {
            _memoryUsage += key.getApproximateSize();
        }
        _memoryUsage += static_cast<size_t>(value.objsize());
    }

    /**
     * Evict the least-recently-used item.
     */
    void evictOne() {
        if (_container.empty()) {
            return;
        }

        const Cached& pair = _container.back();

        size_t keySize = pair.first.getApproximateSize();
        invariant(keySize <= _memoryUsage);
        _memoryUsage -= keySize;

        for (auto&& elem : pair.second) {
            size_t valueSize = static_cast<size_t>(elem.objsize());
            invariant(valueSize <= _memoryUsage);
            _memoryUsage -= valueSize;
        }
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
     * Retrieve the set of values with key "key". If not found, returns boost::none.
     */
    boost::optional<std::unordered_set<BSONObj, BSONObj::Hasher>> operator[](Value key) {
        auto it = boost::multi_index::get<1>(_container).find(key);
        if (it != boost::multi_index::get<1>(_container).end()) {
            boost::multi_index::get<0>(_container)
                .relocate(boost::multi_index::get<0>(_container).begin(),
                          boost::multi_index::project<0>(_container, it));
            return (*it).second;
        }
        return boost::none;
    }

private:
    using Cached = std::pair<Value, std::unordered_set<BSONObj, BSONObj::Hasher>>;

    // boost::multi_index_container provides a system for implementing a cache. Here, we create
    // a container of std::pair<Value, BSONObjSet>, that is both sequenced, and has a unique
    // index on the Value. From this, we are able to evict the least-recently-used member, and
    // maintain key uniqueness.
    using IndexedContainer =
        multi_index_container<Cached,
                              indexed_by<sequenced<>,
                                         hashed_unique<member<Cached, Value, &Cached::first>,
                                                       Value::Hash,
                                                       ValueComparator::EqualTo>>>;

    IndexedContainer _container;

    size_t _memoryUsage = 0;
};

}  // namespace mongo
