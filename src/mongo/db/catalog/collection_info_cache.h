// collection_info_cache.h

/**
*    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/collection_index_usage_tracker.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/query_settings.h"
#include "mongo/db/update_index_data.h"

namespace mongo {

class Collection;
class IndexDescriptor;
class OperationContext;

/**
 * this is for storing things that you want to cache about a single collection
 * life cycle is managed for you from inside Collection
 */
class CollectionInfoCache {
public:
    CollectionInfoCache(Collection* collection);

    /**
     * Get the PlanCache for this collection.
     */
    PlanCache* getPlanCache() const;

    /**
     * Get the QuerySettings for this collection.
     */
    QuerySettings* getQuerySettings() const;

    /* get set of index keys for this namespace.  handy to quickly check if a given
       field is indexed (Note it might be a secondary component of a compound index.)
    */
    const UpdateIndexData& getIndexKeys(OperationContext* txn) const;

    /**
     * Returns cached index usage statistics for this collection.  The map returned will contain
     * entry for each index in the collection along with both a usage counter and a timestamp
     * representing the date/time the counter is valid from.
     *
     * Note for performance that this method returns a copy of a StringMap.
     */
    CollectionIndexUsageMap getIndexUsageStats() const;

    /**
     * Builds internal cache state based on the current state of the Collection's IndexCatalog
     */
    void init(OperationContext* txn);

    /**
     * Register a newly-created index with the cache.  Must be called whenever an index is
     * built on the associated collection.
     *
     * Must be called under exclusive collection lock.
     */
    void addedIndex(OperationContext* txn, const IndexDescriptor* desc);

    /**
     * Deregister a newly-dropped index with the cache.  Must be called whenever an index is
     * dropped on the associated collection.
     *
     * Must be called under exclusive collection lock.
     */
    void droppedIndex(OperationContext* txn, StringData indexName);

    /**
     * Removes all cached query plans.
     */
    void clearQueryCache();

    /**
     * Signal to the cache that a query operation has completed.  'indexesUsed' should list the
     * set of indexes used by the winning plan, if any.
     */
    void notifyOfQuery(OperationContext* txn, const std::set<std::string>& indexesUsed);

private:
    Collection* _collection;  // not owned

    // ---  index keys cache
    bool _keysComputed;
    UpdateIndexData _indexedPaths;

    // A cache for query plans.
    std::unique_ptr<PlanCache> _planCache;

    // Query settings.
    // Includes index filters.
    std::unique_ptr<QuerySettings> _querySettings;

    // Tracks index usage statistics for this collection.
    CollectionIndexUsageTracker _indexUsageTracker;

    void computeIndexKeys(OperationContext* txn);
    void updatePlanCacheIndexEntries(OperationContext* txn);

    /**
     * Rebuilds cached information that is dependent on index composition. Must be called
     * when index composition changes.
     */
    void rebuildIndexData(OperationContext* txn);
};

}  // namespace mongo
