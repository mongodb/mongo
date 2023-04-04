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


#include "mongo/platform/basic.h"

#include "mongo/db/query/collection_query_info.h"

#include <memory>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/collection_index_usage_tracker.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/curop_metrics.h"
#include "mongo/db/exec/projection_executor.h"
#include "mongo/db/exec/projection_executor_utils.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/index/columns_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/wildcard_access_method.h"
#include "mongo/db/query/classic_plan_cache.h"
#include "mongo/db/query/collection_index_usage_tracker_decoration.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/planner_ixselect.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/clock_source.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

namespace {

CoreIndexInfo indexInfoFromIndexCatalogEntry(const IndexCatalogEntry& ice) {
    auto desc = ice.descriptor();
    invariant(desc);

    auto accessMethod = ice.accessMethod();
    invariant(accessMethod);

    const WildcardProjection* projExec = nullptr;
    if (desc->getIndexType() == IndexType::INDEX_WILDCARD)
        projExec = static_cast<const WildcardAccessMethod*>(accessMethod)->getWildcardProjection();

    return {desc->keyPattern(),
            desc->getIndexType(),
            desc->isSparse(),
            IndexEntry::Identifier{desc->indexName()},
            ice.getFilterExpression(),
            ice.getCollator(),
            projExec};
}

}  // namespace

CollectionQueryInfo::PlanCacheState::PlanCacheState()
    : classicPlanCache{static_cast<size_t>(internalQueryCacheMaxEntriesPerCollection.load())} {}

CollectionQueryInfo::PlanCacheState::PlanCacheState(OperationContext* opCtx,
                                                    const CollectionPtr& collection)
    : classicPlanCache{static_cast<size_t>(internalQueryCacheMaxEntriesPerCollection.load())},
      planCacheInvalidator{collection, opCtx->getServiceContext()} {
    std::vector<CoreIndexInfo> indexCores;

    // TODO We shouldn't need to include unfinished indexes, but we must here because the index
    // catalog may be in an inconsistent state.  SERVER-18346.
    auto ii = collection->getIndexCatalog()->getIndexIterator(
        opCtx, IndexCatalog::InclusionPolicy::kReady | IndexCatalog::InclusionPolicy::kUnfinished);
    while (ii->more()) {
        const IndexCatalogEntry* ice = ii->next();
        if (ice->accessMethod()) {
            indexCores.emplace_back(indexInfoFromIndexCatalogEntry(*ice));
        }
    }

    planCacheIndexabilityState.updateDiscriminators(indexCores);
}

void CollectionQueryInfo::PlanCacheState::clearPlanCache() {
    classicPlanCache.clear();
    planCacheInvalidator.clearPlanCache();
}

CollectionQueryInfo::CollectionQueryInfo()
    : _keysComputed{false}, _planCacheState{std::make_shared<PlanCacheState>()} {}

const UpdateIndexData& CollectionQueryInfo::getIndexKeys(OperationContext* opCtx) const {
    invariant(_keysComputed);
    return _indexedPaths;
}

void CollectionQueryInfo::computeUpdateIndexData(const IndexCatalogEntry* entry,
                                                 const IndexAccessMethod* accessMethod,
                                                 UpdateIndexData* outData) {
    const IndexDescriptor* descriptor = entry->descriptor();
    if (bool isWildcard = (descriptor->getAccessMethodName() == IndexNames::WILDCARD);
        isWildcard || descriptor->getAccessMethodName() == IndexNames::COLUMN) {
        // Obtain the projection used by the $** index's key generator.
        const auto* pathProj = isWildcard
            ? static_cast<const IndexPathProjection*>(
                  static_cast<const WildcardAccessMethod*>(accessMethod)->getWildcardProjection())
            : static_cast<const IndexPathProjection*>(
                  static_cast<const ColumnStoreAccessMethod*>(accessMethod)
                      ->getColumnstoreProjection());
        // If the projection is an exclusion, then we must check the new document's keys on all
        // updates, since we do not exhaustively know the set of paths to be indexed.
        if (pathProj->exec()->getType() ==
            TransformerInterface::TransformerType::kExclusionProjection) {
            outData->allPathsIndexed();
        } else {
            // If a subtree was specified in the keyPattern, or if an inclusion projection is
            // present, then we need only index the path(s) preserved by the projection.
            const auto& exhaustivePaths = pathProj->exhaustivePaths();
            invariant(exhaustivePaths);
            for (const auto& path : *exhaustivePaths) {
                outData->addPath(path);
            }

            // Handle regular index fields of Compound Wildcard Index.
            // (Ignore FCV check): This is intentional because we want clusters which have wildcard
            // indexes
            // still be able to use the feature even if the FCV is downgraded.
            if (isWildcard &&
                feature_flags::gFeatureFlagCompoundWildcardIndexes.isEnabledAndIgnoreFCVUnsafe()) {
                BSONObj key = descriptor->keyPattern();
                BSONObjIterator j(key);
                while (j.more()) {
                    StringData fieldName(j.next().fieldName());
                    if (!fieldName.endsWith("$**"_sd)) {
                        outData->addPath(FieldRef{fieldName});
                    }
                }
            }
        }
    } else if (descriptor->getAccessMethodName() == IndexNames::TEXT) {
        fts::FTSSpec ftsSpec(descriptor->infoObj());

        if (ftsSpec.wildcard()) {
            outData->allPathsIndexed();
        } else {
            for (size_t i = 0; i < ftsSpec.numExtraBefore(); ++i) {
                outData->addPath(FieldRef(ftsSpec.extraBefore(i)));
            }
            for (fts::Weights::const_iterator it = ftsSpec.weights().begin();
                 it != ftsSpec.weights().end();
                 ++it) {
                outData->addPath(FieldRef(it->first));
            }
            for (size_t i = 0; i < ftsSpec.numExtraAfter(); ++i) {
                outData->addPath(FieldRef(ftsSpec.extraAfter(i)));
            }
            // Any update to a path containing "language" as a component could change the
            // language of a subdocument.  Add the override field as a path component.
            outData->addPathComponent(ftsSpec.languageOverrideField());
        }
    } else {
        BSONObj key = descriptor->keyPattern();
        BSONObjIterator j(key);
        while (j.more()) {
            BSONElement e = j.next();
            outData->addPath(FieldRef(e.fieldName()));
        }
    }

    // handle partial indexes
    const MatchExpression* filter = entry->getFilterExpression();
    if (filter) {
        RelevantFieldIndexMap paths;
        QueryPlannerIXSelect::getFields(filter, &paths);
        for (auto it = paths.begin(); it != paths.end(); ++it) {
            outData->addPath(FieldRef(it->first));
        }
    }
}

void CollectionQueryInfo::computeUpdateIndexData(OperationContext* opCtx,
                                                 const CollectionPtr& coll) {
    _indexedPaths.clear();

    auto it = coll->getIndexCatalog()->getIndexIterator(
        opCtx, IndexCatalog::InclusionPolicy::kReady | IndexCatalog::InclusionPolicy::kUnfinished);
    while (it->more()) {
        const IndexCatalogEntry* entry = it->next();
        computeUpdateIndexData(entry, entry->accessMethod(), &_indexedPaths);
    }

    _keysComputed = true;
}

void CollectionQueryInfo::notifyOfQuery(OperationContext* opCtx,
                                        const CollectionPtr& coll,
                                        const PlanSummaryStats& summaryStats) const {
    auto& collectionIndexUsageTracker =
        CollectionIndexUsageTrackerDecoration::get(coll->getSharedDecorations());

    collectionIndexUsageTracker.recordCollectionScans(summaryStats.collectionScans);
    collectionIndexUsageTracker.recordCollectionScansNonTailable(
        summaryStats.collectionScansNonTailable);

    const auto& indexesUsed = summaryStats.indexesUsed;
    // Record indexes used to fulfill query.
    for (auto it = indexesUsed.begin(); it != indexesUsed.end(); ++it) {
        collectionIndexUsageTracker.recordIndexAccess(*it);
    }
}

void CollectionQueryInfo::clearQueryCache(OperationContext* opCtx, const CollectionPtr& coll) {
    // We are operating on a cloned collection, the use_count can only be 1 if we've created a new
    // PlanCache instance for this collection clone. Checking the refcount can't race as we can't
    // start readers on this collection while it is writable
    if (_planCacheState.use_count() == 1) {
        LOGV2_DEBUG(5014501,
                    1,
                    "Clearing plan cache - collection info cache cleared",
                    logAttrs(coll->ns()));

        _planCacheState->clearPlanCache();
    } else {
        LOGV2_DEBUG(5014502,
                    1,
                    "Clearing plan cache - collection info cache reinstantiated",
                    logAttrs(coll->ns()));

        updatePlanCacheIndexEntries(opCtx, coll);
    }
}

void CollectionQueryInfo::clearQueryCacheForSetMultikey(const CollectionPtr& coll) const {
    LOGV2_DEBUG(5014500,
                1,
                "Clearing plan cache for multikey - collection info cache cleared",
                logAttrs(coll->ns()));
    _planCacheState->clearPlanCache();
}

void CollectionQueryInfo::updatePlanCacheIndexEntries(OperationContext* opCtx,
                                                      const CollectionPtr& coll) {
    _planCacheState = std::make_shared<PlanCacheState>(opCtx, coll);
}

void CollectionQueryInfo::init(OperationContext* opCtx, const CollectionPtr& coll) {
    auto ii =
        coll->getIndexCatalog()->getIndexIterator(opCtx, IndexCatalog::InclusionPolicy::kReady);
    while (ii->more()) {
        const IndexDescriptor* desc = ii->next()->descriptor();
        CollectionIndexUsageTrackerDecoration::get(coll->getSharedDecorations())
            .registerIndex(desc->indexName(),
                           desc->keyPattern(),
                           IndexFeatures::make(desc, coll->ns().isOnInternalDb()));
    }

    rebuildIndexData(opCtx, coll);
}

void CollectionQueryInfo::rebuildIndexData(OperationContext* opCtx, const CollectionPtr& coll) {
    _keysComputed = false;
    computeUpdateIndexData(opCtx, coll);
    updatePlanCacheIndexEntries(opCtx, coll);
}

}  // namespace mongo
