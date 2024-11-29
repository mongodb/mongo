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

#include "mongo/db/query/collection_query_info.h"

#include <map>
#include <set>
#include <string>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/optional/optional.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/aggregated_index_usage_tracker.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/collection_index_usage_tracker.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/index_path_projection.h"
#include "mongo/db/exec/projection_executor.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/wildcard_access_method.h"
#include "mongo/db/index_names.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/transformer_interface.h"
#include "mongo/db/query/collection_index_usage_tracker_decoration.h"
#include "mongo/db/query/index_entry.h"
#include "mongo/db/query/plan_cache/classic_plan_cache.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/planner_ixselect.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/update_index_data.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util_core.h"

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

void recordCollectionIndexUsage(const CollectionPtr& coll,
                                long long collectionScans,
                                long long collectionScansNonTailable,
                                const std::set<std::string>& indexesUsed) {
    const auto& collectionIndexUsageTracker =
        CollectionIndexUsageTrackerDecoration::get(coll.get());

    collectionIndexUsageTracker.recordCollectionScans(collectionScans);
    collectionIndexUsageTracker.recordCollectionScansNonTailable(collectionScansNonTailable);

    // Record indexes used to fulfill query.
    for (auto it = indexesUsed.begin(); it != indexesUsed.end(); ++it) {
        collectionIndexUsageTracker.recordIndexAccess(*it);
    }
}

}  // namespace

CollectionQueryInfo::PlanCacheState::PlanCacheState()
    : classicPlanCache{static_cast<size_t>(internalQueryCacheMaxEntriesPerCollection.load())} {}

CollectionQueryInfo::PlanCacheState::PlanCacheState(OperationContext* opCtx,
                                                    const Collection* collection)
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

CollectionQueryInfo::CollectionQueryInfo() : _planCacheState{std::make_shared<PlanCacheState>()} {}

void CollectionQueryInfo::computeUpdateIndexData(const IndexCatalogEntry* entry,
                                                 const IndexAccessMethod* accessMethod,
                                                 UpdateIndexData* outData) {
    const IndexDescriptor* descriptor = entry->descriptor();
    if (descriptor->getAccessMethodName() == IndexNames::WILDCARD) {
        // Obtain the projection used by the $** index's key generator.
        const auto* pathProj = static_cast<const IndexPathProjection*>(
            static_cast<const WildcardAccessMethod*>(accessMethod)->getWildcardProjection());
        // If the projection is an exclusion, then we must check the new document's keys on all
        // updates, since we do not exhaustively know the set of paths to be indexed.
        if (pathProj->exec()->getType() ==
            TransformerInterface::TransformerType::kExclusionProjection) {
            outData->setAllPathsIndexed();
        } else {
            // If a subtree was specified in the keyPattern, or if an inclusion projection is
            // present, then we need only index the path(s) preserved by the projection.
            const auto& exhaustivePaths = pathProj->exhaustivePaths();
            invariant(exhaustivePaths);
            for (const auto& path : *exhaustivePaths) {
                outData->addPath(path);
            }

            // Handle regular index fields of Compound Wildcard Index.
            BSONObj key = descriptor->keyPattern();
            BSONObjIterator j(key);
            while (j.more()) {
                StringData fieldName(j.next().fieldName());
                if (!fieldName.endsWith("$**"_sd)) {
                    outData->addPath(FieldRef{fieldName});
                }
            }
        }
    } else if (descriptor->getAccessMethodName() == IndexNames::TEXT) {
        fts::FTSSpec ftsSpec(descriptor->infoObj());

        if (ftsSpec.wildcard()) {
            outData->setAllPathsIndexed();
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

void CollectionQueryInfo::notifyOfQuery(OperationContext* opCtx,
                                        const CollectionPtr& coll,
                                        const PlanSummaryStats& summaryStats) const {
    recordCollectionIndexUsage(coll,
                               summaryStats.collectionScans,
                               summaryStats.collectionScansNonTailable,
                               summaryStats.indexesUsed);
}

void CollectionQueryInfo::notifyOfQuery(const CollectionPtr& coll, const OpDebug& debug) const {
    recordCollectionIndexUsage(
        coll, debug.collectionScans, debug.collectionScansNonTailable, debug.indexesUsed);
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

        updatePlanCacheIndexEntries(opCtx, coll.get());
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
                                                      const Collection* coll) {
    _planCacheState = std::make_shared<PlanCacheState>(opCtx, coll);
}

PlanCache* CollectionQueryInfo::getPlanCache() const {
    return &_planCacheState->classicPlanCache;
}

size_t CollectionQueryInfo::getPlanCacheInvalidatorVersion() const {
    return _planCacheState->planCacheInvalidator.versionNumber();
}

const PlanCacheIndexabilityState& CollectionQueryInfo::getPlanCacheIndexabilityState() const {
    return _planCacheState->planCacheIndexabilityState;
}

void CollectionQueryInfo::init(OperationContext* opCtx, Collection* coll) {
    // Skip registering the index in a --repair, as the server will terminate after
    // the repair operation completes.
    if (storageGlobalParams.repair) {
        LOGV2_DEBUG(7610901, 1, "In a repair, skipping registering indexes");
        return;
    }

    auto ii =
        coll->getIndexCatalog()->getIndexIterator(opCtx, IndexCatalog::InclusionPolicy::kReady);
    auto& collectionIndexUsageTracker = CollectionIndexUsageTrackerDecoration::write(coll);
    while (ii->more()) {
        const IndexDescriptor* desc = ii->next()->descriptor();
        collectionIndexUsageTracker.registerIndex(
            desc->indexName(),
            desc->keyPattern(),
            IndexFeatures::make(desc, coll->ns().isOnInternalDb()));
    }

    rebuildIndexData(opCtx, coll);
}

void CollectionQueryInfo::rebuildIndexData(OperationContext* opCtx, const Collection* coll) {
    updatePlanCacheIndexEntries(opCtx, coll);
}

}  // namespace mongo
