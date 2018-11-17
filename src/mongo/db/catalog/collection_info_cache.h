
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
 * life cycle is managed for you from inside Collection.
 */
class CollectionInfoCache {
public:
    virtual ~CollectionInfoCache() = default;

    /**
     * Builds internal cache state based on the current state of the Collection's IndexCatalog.
     */
    virtual void init(OperationContext* const opCtx) = 0;

    /**
     * Get the PlanCache for this collection.
     */
    virtual PlanCache* getPlanCache() const = 0;

    /**
     * Get the QuerySettings for this collection.
     */
    virtual QuerySettings* getQuerySettings() const = 0;

    /* get set of index keys for this namespace.  handy to quickly check if a given
       field is indexed (Note it might be a secondary component of a compound index.)
    */
    virtual const UpdateIndexData& getIndexKeys(OperationContext* const opCtx) const = 0;

    /**
     * Returns cached index usage statistics for this collection.  The map returned will contain
     * entry for each index in the collection along with both a usage counter and a timestamp
     * representing the date/time the counter is valid from.
     *
     * Note for performance that this method returns a copy of a StringMap.
     */
    virtual CollectionIndexUsageMap getIndexUsageStats() const = 0;

    /**
     * Register a newly-created index with the cache.  Must be called whenever an index is
     * built on the associated collection.
     *
     * Must be called under exclusive collection lock.
     */
    virtual void addedIndex(OperationContext* const opCtx, const IndexDescriptor* const desc) = 0;

    /**
     * Deregister a newly-dropped index with the cache.  Must be called whenever an index is
     * dropped on the associated collection.
     *
     * Must be called under exclusive collection lock.
     */
    virtual void droppedIndex(OperationContext* const opCtx, const StringData indexName) = 0;

    /**
     * Removes all cached query plans.
     */
    virtual void clearQueryCache() = 0;

    /**
     * Signal to the cache that a query operation has completed.  'indexesUsed' should list the
     * set of indexes used by the winning plan, if any.
     */
    virtual void notifyOfQuery(OperationContext* const opCtx,
                               const std::set<std::string>& indexesUsed) = 0;

    virtual void setNs(NamespaceString ns) = 0;
};
}  // namespace mongo
