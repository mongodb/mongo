/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <boost/container/small_vector.hpp>
// IWYU pragma: no_include "boost/intrusive/detail/iterator.hpp"
#include "mongo/db/query/compiler/metadata/path_arrayness.h"
#include "mongo/db/query/plan_cache/classic_plan_cache.h"
#include "mongo/db/query/plan_cache/plan_cache_indexability.h"
#include "mongo/db/query/plan_cache/plan_cache_invalidator.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/platform/rwmutex.h"
#include "mongo/util/decorable.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>


namespace mongo {

class OperationContext;

/**
 * Query information for a particular point-in-time view of a collection.
 *
 * Decorates a Collection instance. Lifecycle is the same as the Collection instance.
 */
class MONGO_MOD_PUBLIC CollectionQueryInfo {
public:
    CollectionQueryInfo();

    inline static const auto getCollectionQueryInfo =
        Collection::declareDecoration<CollectionQueryInfo>();

    static const CollectionQueryInfo& get(const CollectionPtr& collection) {
        return CollectionQueryInfo::getCollectionQueryInfo(collection.get());
    }
    static CollectionQueryInfo& get(Collection* collection) {
        return CollectionQueryInfo::getCollectionQueryInfo(collection);
    }

    /**
     * Gets the PlanCache for this collection.
     */
    PlanCache* getPlanCache() const;

    /**
     * Gets the number of the current collection version used for Plan Cache invalidation.
     */
    size_t getPlanCacheInvalidatorVersion() const;

    /**
     * Gets the "indexability discriminators" used in the PlanCache for generating plan cache keys.
     */
    const PlanCacheIndexabilityState& getPlanCacheIndexabilityState() const;

    /**
     * Builds internal cache state based on the current state of the Collection's IndexCatalog.
     */
    void init(OperationContext* opCtx, Collection* coll);

    /**
     * Rebuilds cached index information. Must be called when an index is modified or an index is
     * dropped/created.
     *
     * Must be called under exclusive collection lock.
     */
    void rebuildIndexData(OperationContext* opCtx, const Collection* coll);

    /**
     * Removes all cached query plans after ensuring that the PlanCache is uniquely owned. The
     * PlanCache is made uniquely owned by creating a new instance and thus detaching from the
     * shared instance.
     */
    void clearQueryCache(OperationContext* opCtx, const CollectionPtr& coll);

    /**
     * Removes all cached query plans without ensuring that the PlanCache is uniquely owned, only
     * allowed when setting an index to multikey. Setting an index to multikey can only go one way
     * and has its own concurrency handling.
     */
    void clearQueryCacheForSetMultikey(const CollectionPtr& coll) const;

    /**
     * Traverses the 'IndexCatalogEntry's of the Collection instance and populates a new
     * PathArrayness instance with information from index metadata.
     *
     * When this runs:
     * - DDL/startup (create collection, create/drop index, refreshEntry, repair index
     * (forceSetMultikey())): invoked inside a WriteUnitOfWork (WUOW) that modifies the
     * IndexCatalog. Publishing occurs at WUOW commit, atomically with the catalog change
     *
     * We assume this method will be called in a thread-safe manner since no other thread can
     * observe in-flight mutations to the Collection instance cloned for copy-on-write.
     */
    void rebuildPathArrayness(OperationContext* opCtx, const Collection* coll);

    /**
     * Updates the PathArrayness instance in response to a multikeyness change in indexes.
     *
     * When this runs:
     * - Multikey flips from write (no Collection copy-on-write): invoked under the write's WUOW and
     * thus we do not have atomic catalog change. We ensure atomic updates to the shared
     * PathArrayness via a WriteRarelyRWMutex. This path of code will be called rarely when an
     * document insert or update flips  the multikeyness of indexes. This flip in multikeyness can
     * only go in one direction, non-multikey to multikey.
     *
     * TODO: SERVER-115001: Explain const-ness of function when we implement.
     */
    void updatePathArraynessForSetMultikey(OperationContext* opCtx, const Collection* coll) const;

    std::shared_ptr<const PathArrayness> getPathArrayness() const;

private:
    /**
     * Stores Clasic and SBE PlanCache-related state. Classic Plan Cache is stored per collection
     * and represented by a mongo::PlanCache object. SBE PlanCache is stored in a process-global
     * object, therefore, it is represented here as a PlanCacheInvalidator which knows what
     * collection version to invalidate.
     */
    struct PlanCacheState {
        PlanCacheState();

        PlanCacheState(OperationContext* opCtx, const Collection* collection);

        /**
         * Clears classic and SBE cache entries with the current collection version.
         */
        void clearPlanCache();

        // Per collection version classic plan cache.
        PlanCache classicPlanCache;

        // SBE PlanCacheInvalidator which can invalidate cache entries associated with a particular
        // version of a collection.
        PlanCacheInvalidator planCacheInvalidator;

        // Holds computed information about the collection's indexes. Used for generating plan
        // cache keys.
        PlanCacheIndexabilityState planCacheIndexabilityState;
    };

    /**
     * Wrapper around the underlying 'PathArrayness' instance. This allows us to modify the
     * underlying 'PathArrayness' instance despite the const-ness guarantees needed by callers.
     */
    struct PathArraynessState {
        PathArraynessState();

        // All clones of CollectionQueryInfo will initially point to the same PathArrayness
        // instance.
        std::shared_ptr<PathArrayness> pathArrayness;
    };

    void updatePlanCacheIndexEntries(OperationContext* opCtx, const Collection* coll);

    std::shared_ptr<PlanCacheState> _planCacheState;

    PathArraynessState _pathArraynessState;
};

}  // namespace mongo
