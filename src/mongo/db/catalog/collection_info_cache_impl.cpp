
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/collection_info_cache_impl.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/wildcard_access_method.h"
#include "mongo/db/index_legacy.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/planner_ixselect.h"
#include "mongo/db/service_context.h"
#include "mongo/db/ttl_collection_cache.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/log.h"

namespace mongo {

CollectionInfoCacheImpl::CollectionInfoCacheImpl(Collection* collection, const NamespaceString& ns)
    : _collection(collection),
      _ns(ns),
      _keysComputed(false),
      _planCache(stdx::make_unique<PlanCache>(ns.ns())),
      _querySettings(stdx::make_unique<QuerySettings>()),
      _indexUsageTracker(getGlobalServiceContext()->getPreciseClockSource()) {}

CollectionInfoCacheImpl::~CollectionInfoCacheImpl() {
    // Necessary because the collection cache will not explicitly get updated upon database drop.
    if (_hasTTLIndex) {
        TTLCollectionCache& ttlCollectionCache = TTLCollectionCache::get(getGlobalServiceContext());
        ttlCollectionCache.unregisterCollection(_ns);
    }
}

const UpdateIndexData& CollectionInfoCacheImpl::getIndexKeys(OperationContext* opCtx) const {
    // This requires "some" lock, and MODE_IS is an expression for that, for now.
    dassert(opCtx->lockState()->isCollectionLockedForMode(_collection->ns().ns(), MODE_IS));
    invariant(_keysComputed);
    return _indexedPaths;
}

void CollectionInfoCacheImpl::computeIndexKeys(OperationContext* opCtx) {
    _indexedPaths.clear();

    bool hadTTLIndex = _hasTTLIndex;
    _hasTTLIndex = false;

    std::unique_ptr<IndexCatalog::IndexIterator> it =
        _collection->getIndexCatalog()->getIndexIterator(opCtx, true);
    while (it->more()) {
        const IndexCatalogEntry* entry = it->next();
        const IndexDescriptor* descriptor = entry->descriptor();
        const IndexAccessMethod* iam = entry->accessMethod();

        if (descriptor->getAccessMethodName() == IndexNames::WILDCARD) {
            // Obtain the projection used by the $** index's key generator.
            const auto* pathProj =
                static_cast<const WildcardAccessMethod*>(iam)->getProjectionExec();
            // If the projection is an exclusion, then we must check the new document's keys on all
            // updates, since we do not exhaustively know the set of paths to be indexed.
            if (pathProj->getType() == ProjectionExecAgg::ProjectionType::kExclusionProjection) {
                _indexedPaths.allPathsIndexed();
            } else {
                // If a subtree was specified in the keyPattern, or if an inclusion projection is
                // present, then we need only index the path(s) preserved by the projection.
                for (const auto& path : pathProj->getExhaustivePaths()) {
                    _indexedPaths.addPath(path);
                }
            }
        } else if (descriptor->getAccessMethodName() == IndexNames::TEXT) {
            fts::FTSSpec ftsSpec(descriptor->infoObj());

            if (ftsSpec.wildcard()) {
                _indexedPaths.allPathsIndexed();
            } else {
                for (size_t i = 0; i < ftsSpec.numExtraBefore(); ++i) {
                    _indexedPaths.addPath(FieldRef(ftsSpec.extraBefore(i)));
                }
                for (fts::Weights::const_iterator it = ftsSpec.weights().begin();
                     it != ftsSpec.weights().end();
                     ++it) {
                    _indexedPaths.addPath(FieldRef(it->first));
                }
                for (size_t i = 0; i < ftsSpec.numExtraAfter(); ++i) {
                    _indexedPaths.addPath(FieldRef(ftsSpec.extraAfter(i)));
                }
                // Any update to a path containing "language" as a component could change the
                // language of a subdocument.  Add the override field as a path component.
                _indexedPaths.addPathComponent(ftsSpec.languageOverrideField());
            }
        } else {
            BSONObj key = descriptor->keyPattern();
            const BSONObj& infoObj = descriptor->infoObj();
            if (infoObj.hasField("expireAfterSeconds")) {
                _hasTTLIndex = true;
            }
            BSONObjIterator j(key);
            while (j.more()) {
                BSONElement e = j.next();
                _indexedPaths.addPath(FieldRef(e.fieldName()));
            }
        }

        // handle partial indexes
        const MatchExpression* filter = entry->getFilterExpression();
        if (filter) {
            stdx::unordered_set<std::string> paths;
            QueryPlannerIXSelect::getFields(filter, &paths);
            for (auto it = paths.begin(); it != paths.end(); ++it) {
                _indexedPaths.addPath(FieldRef(*it));
            }
        }
    }

    TTLCollectionCache& ttlCollectionCache = TTLCollectionCache::get(getGlobalServiceContext());

    if (_hasTTLIndex != hadTTLIndex) {
        if (_hasTTLIndex) {
            ttlCollectionCache.registerCollection(_collection->ns());
        } else {
            ttlCollectionCache.unregisterCollection(_collection->ns());
        }
    }

    _keysComputed = true;
}

void CollectionInfoCacheImpl::notifyOfQuery(OperationContext* opCtx,
                                            const std::set<std::string>& indexesUsed) {
    // Record indexes used to fulfill query.
    for (auto it = indexesUsed.begin(); it != indexesUsed.end(); ++it) {
        // This index should still exist, since the PlanExecutor would have been killed if the
        // index was dropped (and we would not get here).
        dassert(NULL != _collection->getIndexCatalog()->findIndexByName(opCtx, *it));

        _indexUsageTracker.recordIndexAccess(*it);
    }
}

void CollectionInfoCacheImpl::clearQueryCache() {
    LOG(1) << _collection->ns().ns() << ": clearing plan cache - collection info cache reset";
    if (NULL != _planCache.get()) {
        _planCache->clear();
    }
}

PlanCache* CollectionInfoCacheImpl::getPlanCache() const {
    return _planCache.get();
}

QuerySettings* CollectionInfoCacheImpl::getQuerySettings() const {
    return _querySettings.get();
}

void CollectionInfoCacheImpl::updatePlanCacheIndexEntries(OperationContext* opCtx) {
    std::vector<CoreIndexInfo> indexCores;

    // TODO We shouldn't need to include unfinished indexes, but we must here because the index
    // catalog may be in an inconsistent state.  SERVER-18346.
    const bool includeUnfinishedIndexes = true;
    std::unique_ptr<IndexCatalog::IndexIterator> ii =
        _collection->getIndexCatalog()->getIndexIterator(opCtx, includeUnfinishedIndexes);
    while (ii->more()) {
        const IndexCatalogEntry* ice = ii->next();
        indexCores.emplace_back(indexInfoFromIndexCatalogEntry(*ice));
    }

    _planCache->notifyOfIndexUpdates(indexCores);
}

void CollectionInfoCacheImpl::init(OperationContext* opCtx) {
    // Requires exclusive collection lock.
    invariant(opCtx->lockState()->isCollectionLockedForMode(_collection->ns().ns(), MODE_X));

    const bool includeUnfinishedIndexes = false;
    std::unique_ptr<IndexCatalog::IndexIterator> ii =
        _collection->getIndexCatalog()->getIndexIterator(opCtx, includeUnfinishedIndexes);
    while (ii->more()) {
        const IndexDescriptor* desc = ii->next()->descriptor();
        _indexUsageTracker.registerIndex(desc->indexName(), desc->keyPattern());
    }

    rebuildIndexData(opCtx);
}

void CollectionInfoCacheImpl::addedIndex(OperationContext* opCtx, const IndexDescriptor* desc) {
    // Requires exclusive collection lock.
    invariant(opCtx->lockState()->isCollectionLockedForMode(_collection->ns().ns(), MODE_X));
    invariant(desc);

    rebuildIndexData(opCtx);

    _indexUsageTracker.registerIndex(desc->indexName(), desc->keyPattern());
}

void CollectionInfoCacheImpl::droppedIndex(OperationContext* opCtx, StringData indexName) {
    // Requires exclusive collection lock.
    invariant(opCtx->lockState()->isCollectionLockedForMode(_collection->ns().ns(), MODE_X));

    rebuildIndexData(opCtx);
    _indexUsageTracker.unregisterIndex(indexName);
}

void CollectionInfoCacheImpl::rebuildIndexData(OperationContext* opCtx) {
    clearQueryCache();

    _keysComputed = false;
    computeIndexKeys(opCtx);
    updatePlanCacheIndexEntries(opCtx);
}

CollectionIndexUsageMap CollectionInfoCacheImpl::getIndexUsageStats() const {
    return _indexUsageTracker.getUsageStats();
}

void CollectionInfoCacheImpl::setNs(NamespaceString ns) {
    auto oldNs = _ns;
    _ns = std::move(ns);

    _planCache->setNs(_ns);

    // Update the TTL collection cache.
    if (_hasTTLIndex) {
        auto& ttlCollectionCache = TTLCollectionCache::get(getGlobalServiceContext());
        ttlCollectionCache.unregisterCollection(oldNs);
        ttlCollectionCache.registerCollection(_ns);
    }
}

}  // namespace mongo
