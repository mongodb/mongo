// collection_info_cache.cpp

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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/collection_info_cache.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_legacy.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/planner_ixselect.h"
#include "mongo/db/service_context.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/log.h"

namespace mongo {

CollectionInfoCache::CollectionInfoCache(Collection* collection)
    : _collection(collection),
      _keysComputed(false),
      _planCache(new PlanCache(collection->ns().ns())),
      _querySettings(new QuerySettings()),
      _indexUsageTracker(getGlobalServiceContext()->getPreciseClockSource()) {}


const UpdateIndexData& CollectionInfoCache::getIndexKeys(OperationContext* txn) const {
    // This requires "some" lock, and MODE_IS is an expression for that, for now.
    dassert(txn->lockState()->isCollectionLockedForMode(_collection->ns().ns(), MODE_IS));
    invariant(_keysComputed);
    return _indexedPaths;
}

void CollectionInfoCache::computeIndexKeys(OperationContext* txn) {
    _indexedPaths.clear();

    IndexCatalog::IndexIterator i = _collection->getIndexCatalog()->getIndexIterator(txn, true);
    while (i.more()) {
        IndexDescriptor* descriptor = i.next();

        if (descriptor->getAccessMethodName() != IndexNames::TEXT) {
            BSONObj key = descriptor->keyPattern();
            BSONObjIterator j(key);
            while (j.more()) {
                BSONElement e = j.next();
                _indexedPaths.addPath(e.fieldName());
            }
        } else {
            fts::FTSSpec ftsSpec(descriptor->infoObj());

            if (ftsSpec.wildcard()) {
                _indexedPaths.allPathsIndexed();
            } else {
                for (size_t i = 0; i < ftsSpec.numExtraBefore(); ++i) {
                    _indexedPaths.addPath(ftsSpec.extraBefore(i));
                }
                for (fts::Weights::const_iterator it = ftsSpec.weights().begin();
                     it != ftsSpec.weights().end();
                     ++it) {
                    _indexedPaths.addPath(it->first);
                }
                for (size_t i = 0; i < ftsSpec.numExtraAfter(); ++i) {
                    _indexedPaths.addPath(ftsSpec.extraAfter(i));
                }
                // Any update to a path containing "language" as a component could change the
                // language of a subdocument.  Add the override field as a path component.
                _indexedPaths.addPathComponent(ftsSpec.languageOverrideField());
            }
        }

        // handle partial indexes
        const IndexCatalogEntry* entry = i.catalogEntry(descriptor);
        const MatchExpression* filter = entry->getFilterExpression();
        if (filter) {
            unordered_set<std::string> paths;
            QueryPlannerIXSelect::getFields(filter, "", &paths);
            for (auto it = paths.begin(); it != paths.end(); ++it) {
                _indexedPaths.addPath(*it);
            }
        }
    }

    _keysComputed = true;
}

void CollectionInfoCache::notifyOfQuery(OperationContext* txn,
                                        const std::set<std::string>& indexesUsed) {
    // Record indexes used to fulfill query.
    for (auto it = indexesUsed.begin(); it != indexesUsed.end(); ++it) {
        // This index should still exist, since the PlanExecutor would have been killed if the
        // index was dropped (and we would not get here).
        dassert(NULL != _collection->getIndexCatalog()->findIndexByName(txn, *it));

        _indexUsageTracker.recordIndexAccess(*it);
    }
}

void CollectionInfoCache::clearQueryCache() {
    LOG(1) << _collection->ns().ns() << ": clearing plan cache - collection info cache reset";
    if (NULL != _planCache.get()) {
        _planCache->clear();
    }
}

PlanCache* CollectionInfoCache::getPlanCache() const {
    return _planCache.get();
}

QuerySettings* CollectionInfoCache::getQuerySettings() const {
    return _querySettings.get();
}

void CollectionInfoCache::updatePlanCacheIndexEntries(OperationContext* txn) {
    std::vector<IndexEntry> indexEntries;

    // TODO We shouldn't need to include unfinished indexes, but we must here because the index
    // catalog may be in an inconsistent state.  SERVER-18346.
    const bool includeUnfinishedIndexes = true;
    IndexCatalog::IndexIterator ii =
        _collection->getIndexCatalog()->getIndexIterator(txn, includeUnfinishedIndexes);
    while (ii.more()) {
        const IndexDescriptor* desc = ii.next();
        const IndexCatalogEntry* ice = ii.catalogEntry(desc);
        indexEntries.emplace_back(desc->keyPattern(),
                                  desc->getAccessMethodName(),
                                  desc->isMultikey(txn),
                                  ice->getMultikeyPaths(txn),
                                  desc->isSparse(),
                                  desc->unique(),
                                  desc->indexName(),
                                  ice->getFilterExpression(),
                                  desc->infoObj(),
                                  ice->getCollator());
    }

    _planCache->notifyOfIndexEntries(indexEntries);
}

void CollectionInfoCache::init(OperationContext* txn) {
    // Requires exclusive collection lock.
    invariant(txn->lockState()->isCollectionLockedForMode(_collection->ns().ns(), MODE_X));

    const bool includeUnfinishedIndexes = false;
    IndexCatalog::IndexIterator ii =
        _collection->getIndexCatalog()->getIndexIterator(txn, includeUnfinishedIndexes);
    while (ii.more()) {
        const IndexDescriptor* desc = ii.next();
        _indexUsageTracker.registerIndex(desc->indexName(), desc->keyPattern());
    }

    rebuildIndexData(txn);
}

void CollectionInfoCache::addedIndex(OperationContext* txn, const IndexDescriptor* desc) {
    // Requires exclusive collection lock.
    invariant(txn->lockState()->isCollectionLockedForMode(_collection->ns().ns(), MODE_X));
    invariant(desc);

    rebuildIndexData(txn);

    _indexUsageTracker.registerIndex(desc->indexName(), desc->keyPattern());
}

void CollectionInfoCache::droppedIndex(OperationContext* txn, StringData indexName) {
    // Requires exclusive collection lock.
    invariant(txn->lockState()->isCollectionLockedForMode(_collection->ns().ns(), MODE_X));

    rebuildIndexData(txn);
    _indexUsageTracker.unregisterIndex(indexName);
}

void CollectionInfoCache::rebuildIndexData(OperationContext* txn) {
    clearQueryCache();

    _keysComputed = false;
    computeIndexKeys(txn);
    updatePlanCacheIndexEntries(txn);
}

CollectionIndexUsageMap CollectionInfoCache::getIndexUsageStats() const {
    return _indexUsageTracker.getUsageStats();
}
}
