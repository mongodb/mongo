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

#include "query_planner_params.h"

#include "mongo/db/exec/projection_executor_utils.h"
#include "mongo/db/global_catalog/shard_key_pattern_query_util.h"
#include "mongo/db/index/multikey_metadata_access_stats.h"
#include "mongo/db/index/wildcard_access_method.h"
#include "mongo/db/query/compiler/stats/collection_statistics_impl.h"
#include "mongo/db/query/distinct_access.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/planner_ixselect.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/query/query_settings_decoration.h"
#include "mongo/db/query/query_utils.h"
#include "mongo/db/query/wildcard_multikey_paths.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/s/shard_targeting_helpers.h"
#include "mongo/util/assert_util.h"

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

MONGO_FAIL_POINT_DEFINE(pauseAfterFillingOutIndexEntries);

namespace {
/**
 * Converts the catalog metadata for an index into an IndexEntry, which is a format that is meant to
 * be consumed by the query planner. This function can perform index reads and should not be called
 * unless access to the storage engine is permitted.
 *
 * When 'canonicalQuery' is not null, only multikey metadata paths that intersect with the query
 * field set will be retrieved for a multikey wildcard index. Otherwise all multikey metadata paths
 * will be retrieved.
 */
IndexEntry indexEntryFromIndexCatalogEntry(OperationContext* opCtx,
                                           const CollectionPtr& collection,
                                           const IndexCatalogEntry& ice,
                                           const CanonicalQuery& canonicalQuery) {
    auto desc = ice.descriptor();
    invariant(desc);

    if (desc->isIdIndex()) {
        // _id indexes are guaranteed to be non-multikey. Determining whether the index is multikey
        // has a small cost associated with it, so we skip that here to make _id lookups faster.
        return {desc->keyPattern(),
                desc->getIndexType(),
                desc->version(),
                false, /* isMultikey */
                {},    /* MultikeyPaths */
                {},    /* multikey Pathset */
                desc->isSparse(),
                desc->unique(),
                IndexEntry::Identifier{desc->indexName()},
                ice.getFilterExpression(),
                desc->infoObj(),
                ice.getCollator(),
                nullptr /* wildcard projection */};
    }

    auto accessMethod = ice.accessMethod();
    invariant(accessMethod);

    const bool isMultikey = ice.isMultikey(opCtx, collection);

    const WildcardProjection* wildcardProjection = nullptr;
    std::set<FieldRef> multikeyPathSet;
    if (desc->getIndexType() == IndexType::INDEX_WILDCARD) {
        wildcardProjection =
            static_cast<const WildcardAccessMethod*>(accessMethod)->getWildcardProjection();
        if (isMultikey) {
            MultikeyMetadataAccessStats mkAccessStats;

            RelevantFieldIndexMap fieldIndexProps;
            QueryPlannerIXSelect::getFields(canonicalQuery.getPrimaryMatchExpression(),
                                            &fieldIndexProps);
            stdx::unordered_set<std::string> projectedFields;
            for (auto&& [fieldName, _] : fieldIndexProps) {
                if (projection_executor_utils::applyProjectionToOneField(wildcardProjection->exec(),
                                                                         fieldName)) {
                    projectedFields.insert(fieldName);
                }
            }

            multikeyPathSet =
                getWildcardMultikeyPathSet(opCtx, &ice, projectedFields, &mkAccessStats);

            LOGV2_DEBUG(20920,
                        2,
                        "Multikey path metadata range index scan stats",
                        "index"_attr = desc->indexName(),
                        "numSeeks"_attr = mkAccessStats.keysExamined,
                        "keysExamined"_attr = mkAccessStats.keysExamined);
        }
    }

    return {desc->keyPattern(),
            desc->getIndexType(),
            desc->version(),
            isMultikey,
            // The fixed-size vector of multikey paths stored in the index catalog.
            ice.getMultikeyPaths(opCtx, collection),
            // The set of multikey paths from special metadata keys stored in the index itself.
            // Indexes that have these metadata keys do not store a fixed-size vector of multikey
            // metadata in the index catalog. Depending on the index type, an index uses one of
            // these mechanisms (or neither), but not both.
            std::move(multikeyPathSet),
            desc->isSparse(),
            desc->unique(),
            IndexEntry::Identifier{desc->indexName()},
            ice.getFilterExpression(),
            desc->infoObj(),
            ice.getCollator(),
            wildcardProjection,
            ice.shared_from_this()};
}

void fillOutIndexEntries(OperationContext* opCtx,
                         const CanonicalQuery& canonicalQuery,
                         const CollectionPtr& collection,
                         std::vector<IndexEntry>& entries) {
    bool apiStrict = APIParameters::get(opCtx).getAPIStrict().value_or(false);

    std::vector<const IndexCatalogEntry*> indexCatalogEntries;
    auto ii =
        collection->getIndexCatalog()->getIndexIterator(IndexCatalog::InclusionPolicy::kReady);
    while (ii->more()) {
        const IndexCatalogEntry* ice = ii->next();

        // Indexes excluded from API version 1 should _not_ be used for planning if apiStrict is
        // set to true.
        auto indexType = ice->descriptor()->getIndexType();
        if (apiStrict &&
            (indexType == IndexType::INDEX_HAYSTACK || indexType == IndexType::INDEX_TEXT ||
             ice->descriptor()->isSparse())) {
            continue;
        }

        // Skip the addition of hidden indexes to prevent use in query planning.
        if (ice->descriptor()->hidden()) {
            continue;
        }

        indexCatalogEntries.push_back(ice);
    }

    entries.reserve(indexCatalogEntries.size());
    for (auto ice : indexCatalogEntries) {
        entries.emplace_back(
            indexEntryFromIndexCatalogEntry(opCtx, collection, *ice, canonicalQuery));
    }
    pauseAfterFillingOutIndexEntries.pauseWhileSet();
}

void fillOutPlannerCollectionInfo(OperationContext* opCtx,
                                  const CollectionPtr& collection,
                                  PlannerCollectionInfo* out,
                                  bool includeSizeStats) {
    out->isTimeseries = static_cast<bool>(collection->getTimeseriesOptions());
    if (includeSizeStats) {
        // We only include these sometimes, since they are slightly expensive to compute.
        auto recordStore = collection->getRecordStore();
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
        out->noOfRecords = recordStore->numRecords();
        out->approximateDataSizeBytes = recordStore->dataSize();
        out->storageSizeBytes = recordStore->storageSize(ru);
    }
}

std::vector<IndexHint> transformTimeseriesHints(std::vector<IndexHint> qsIndexHints,
                                                const TimeseriesOptions& timeseriesOptions) {
    for (auto&& hint : qsIndexHints) {
        if (auto indexKeyPattern = hint.getIndexKeyPattern()) {
            auto timeSeriesKeyPattern = timeseries::createBucketsIndexSpecFromTimeseriesIndexSpec(
                timeseriesOptions, *indexKeyPattern);
            if (!timeSeriesKeyPattern.isOK()) {
                LOGV2_INFO(8699600,
                           "Couldn't convert index hint to time-series format.",
                           "hint"_attr = hint.getIndexKeyPattern()->toString());
                dassert(false);
                continue;
            }
            hint = IndexHint{timeSeriesKeyPattern.getValue()};
        }
    }
    return qsIndexHints;
}
}  // namespace

void QueryPlannerParams::applyQuerySettingsIndexHintsForCollection(
    const CanonicalQuery& canonicalQuery,
    const std::vector<mongo::IndexHint>& allowedIndexes,
    std::vector<IndexEntry>& indexes) {
    // Checks if index entry is present in the 'allowedIndexes' list.
    auto notInAllowedIndexes = [&](const IndexEntry& indexEntry) {
        return std::none_of(
            allowedIndexes.begin(), allowedIndexes.end(), [&](const IndexHint& allowedIndex) {
                return visit(OverloadedVisitor{
                                 [&](const mongo::IndexKeyPattern& indexKeyPattern) {
                                     return indexKeyPattern.woCompare(indexEntry.keyPattern) == 0;
                                 },
                                 [&](const mongo::IndexName& indexName) {
                                     return indexName == indexEntry.identifier.catalogName;
                                 },
                                 [](const mongo::NaturalOrderHint&) { return false; },
                             },
                             allowedIndex.getHint());
            });
    };

    // Remove indices from the planner parameters if the index is not in the 'allowedIndexes'
    // list.
    indexes.erase(std::remove_if(indexes.begin(), indexes.end(), notInAllowedIndexes),
                  indexes.end());
}

// Handle the '$natural' and cluster key (for clustered indexes) cases. Iterate over the
// 'allowedIndexes' list and resolve the allowed directions for performing collection scans.
// If no '$natural' hint is present then collection scans are forbidden.
//
// The possible cases for '$natural' allowed indexes are:
//     * [] - All collection scans are forbidden.
//        * Sets the 'NO_TABLE_SCAN' planner parameter flag to 'true'.
//        * Sets the 'collscanDirection' planner parameter to 'boost::none'.
//
//    * [{$natural: 1}] - Only forward collection scans are allowed.
//        * Unsets the 'NO_TABLE_SCAN' planner parameter flag.
//        * Sets the 'collscanDirection' to 'NaturalOrderHint::Direction::kForward'.
//
//    * [{$natural: -1}] - Only backward collection scans are allowed.
//        * Unsets the 'NO_TABLE_SCAN' planner parameter flag.
//        * Sets the 'collscanDirection' to 'NaturalOrderHint::Direction::kBackward'.
//
//    * [{$natural: 1}, {$natural: -1}] - All collection scan directions are allowed.
//        * Unsets the 'NO_TABLE_SCAN' planner parameter flag.
//        * Sets the 'collscanDirection' planner parameter to 'boost::none'.
void QueryPlannerParams::applyQuerySettingsNaturalHintsForCollection(
    const CanonicalQuery& canonicalQuery,
    const std::vector<mongo::IndexHint>& allowedIndexes,
    CollectionInfo& collectionInfo) {
    bool forwardAllowed = false;
    bool backwardAllowed = false;
    for (const auto& allowedIndex : allowedIndexes) {
        visit(OverloadedVisitor{
                  // If the collection is clustered, then allow both collection scan directions
                  // when the provided index key pattern matches the cluster key.
                  [&](const mongo::IndexKeyPattern& indexKeyPattern) {
                      if (!clusteredInfo) {
                          return;
                      }
                      const auto& clusteredKeyPattern = clusteredInfo->getIndexSpec().getKey();
                      if (indexKeyPattern.woCompare(clusteredKeyPattern) == 0) {
                          forwardAllowed = backwardAllowed = true;
                      }
                  },
                  // Similarly, if the collection is clustered and the provided index name matches
                  // the clustered index name then allow both collection scan directions.
                  [&](const mongo::IndexName& indexName) {
                      if (!clusteredInfo) {
                          return;
                      }

                      const auto& clusteredIndexName = clusteredInfo->getIndexSpec().getName();
                      tassert(7923300,
                              "clusteredIndex's name should be filled in by default after creation",
                              clusteredIndexName.has_value());
                      if (indexName == *clusteredIndexName) {
                          forwardAllowed = backwardAllowed = true;
                      }
                  },
                  // Allow only the direction specified by the explicit '$natural' hint.
                  [&](const mongo::NaturalOrderHint& hint) {
                      switch (hint.direction) {
                          case NaturalOrderHint::Direction::kForward:
                              forwardAllowed = true;
                              break;
                          case NaturalOrderHint::Direction::kBackward:
                              backwardAllowed = true;
                              break;
                      }
                  },
              },
              allowedIndex.getHint());
    }

    constexpr auto strictNoTableScan = (QueryPlannerParams::Options::NO_TABLE_SCAN |
                                        QueryPlannerParams::Options::STRICT_NO_TABLE_SCAN);
    if (!forwardAllowed && !backwardAllowed) {
        // No '$natural' or cluster key hint present. Ensure that table scans are forbidden.
        collectionInfo.options |= strictNoTableScan;
    } else {
        // At least one direction is allowed. Clear out the 'NO_TABLE_SCAN' and
        // 'STRICT_NO_TABLE_SCAN' flags if they exist, as query settings should have a higher
        // precedence over server parameters.
        collectionInfo.options &= ~strictNoTableScan;

        // Enforce the scan direction if needed.
        const bool bothDirectionsAllowed = forwardAllowed && backwardAllowed;
        if (!bothDirectionsAllowed) {
            collectionInfo.collscanDirection = forwardAllowed
                ? NaturalOrderHint::Direction::kForward
                : NaturalOrderHint::Direction::kBackward;
        }
    }
}

void QueryPlannerParams::applyQuerySettingsForCollection(
    const CanonicalQuery& canonicalQuery,
    const NamespaceString& nss,
    const query_settings::IndexHintSpecs& hintSpecs,
    CollectionInfo& collectionInfo,
    const boost::optional<TimeseriesOptions>& timeseriesOptions = boost::none) {
    // Retrieving the allowed indexes for the given collection.
    const auto& allowedIndexes = [&]() {
        const bool isTimeseriesColl = timeseriesOptions.has_value();

        // For legacy time series collections (view + buckets), we need to compare time series view
        // namespace instead of the internal one, as query settings should be specified with the
        // view name.
        // TODO SERVER-103011 always use `nss` once 9.0 becomes last LTS. By then only viewless
        // timeseries will exist.
        const auto& namespaceToCompare = isTimeseriesColl && nss.isTimeseriesBucketsCollection()
            ? nss.getTimeseriesViewNamespace()
            : nss;
        auto isHintForCollection = [&](const auto& hint) {
            auto hintNs =
                NamespaceStringUtil::deserialize(*hint.getNs().getDb(), *hint.getNs().getColl());
            return hintNs == namespaceToCompare;
        };
        auto hintIt = std::find_if(hintSpecs.begin(), hintSpecs.end(), isHintForCollection);
        if (hintIt == hintSpecs.end()) {
            return std::vector<mongo::IndexHint>();
        }

        if (isTimeseriesColl) {
            // Time series KeyPatternIndexes hints need to be converted to match the bucket specs.
            return transformTimeseriesHints(hintIt->getAllowedIndexes(), *timeseriesOptions);
        }
        return hintIt->getAllowedIndexes();
    }();

    // Users can not define empty allowedIndexes vector, therefore early exit if no hints are
    // defined for the given collection.
    if (allowedIndexes.empty()) {
        return;
    }

    applyQuerySettingsIndexHintsForCollection(
        canonicalQuery, allowedIndexes, collectionInfo.indexes);
    applyQuerySettingsNaturalHintsForCollection(canonicalQuery, allowedIndexes, collectionInfo);

    querySettingsApplied = true;
}

void QueryPlannerParams::applyIndexFilters(const CanonicalQuery& canonicalQuery,
                                           const CollectionPtr& collection) {
    // TODO: SERVER-88503 Remove Index Filters feature.
    auto filterAllowedIndexEntries = [](const AllowedIndicesFilter& allowedIndicesFilter,
                                        std::vector<IndexEntry>& indexEntries) {
        // Filter index entries
        // Check BSON objects in AllowedIndices::_indexKeyPatterns against IndexEntry::keyPattern.
        // Removes index entries that do not match _indexKeyPatterns.
        std::vector<IndexEntry> temp;
        for (std::vector<IndexEntry>::const_iterator i = indexEntries.begin();
             i != indexEntries.end();
             ++i) {
            const IndexEntry& indexEntry = *i;
            if (allowedIndicesFilter.allows(indexEntry)) {
                // Copy index entry into temp vector if found in query settings.
                temp.push_back(indexEntry);
            }
        }

        // Update results.
        temp.swap(indexEntries);
    };

    const auto& querySettings = *QuerySettingsDecoration::get(collection->getSharedDecorations());

    // Filter index catalog if index filters are specified for query.
    // Also, signal to planner that application hint should be ignored.
    if (boost::optional<AllowedIndicesFilter> allowedIndicesFilter =
            querySettings.getAllowedIndicesFilter(canonicalQuery)) {
        filterAllowedIndexEntries(*allowedIndicesFilter, mainCollectionInfo.indexes);
        indexFiltersApplied = true;

        static Rarely sampler;
        if (sampler.tick()) {
            LOGV2_WARNING(
                7923200,
                "Index filters are deprecated, consider using query settings instead. See "
                "https://www.mongodb.com/docs/manual/reference/command/setQuerySettings");
        }
    }
}

void QueryPlannerParams::applyQuerySettingsOrIndexFiltersForMainCollection(
    const CanonicalQuery& canonicalQuery, const MultipleCollectionAccessor& collections) {
    // If 'querySettings' has no index hints specified, then there are no settings to be applied
    // to this query.
    auto indexHintSpecs = canonicalQuery.getExpCtx()->getQuerySettings().getIndexHints();
    const bool shouldIgnoreQuerySettings = mainCollectionInfo.options & IGNORE_QUERY_SETTINGS;
    if (indexHintSpecs && !shouldIgnoreQuerySettings) {
        const auto& timeseriesOptions = collections.getMainCollection()->getTimeseriesOptions();
        const NamespaceString& targetNss = collections.getMainCollection()->ns();
        applyQuerySettingsForCollection(
            canonicalQuery, targetNss, *indexHintSpecs, mainCollectionInfo, timeseriesOptions);
    }

    // Try to apply index filters only if query settings were not applied.
    if (!querySettingsApplied) {
        applyIndexFilters(canonicalQuery, collections.getMainCollection());
    }
}

void QueryPlannerParams::fillOutSecondaryCollectionsPlannerParams(
    OperationContext* opCtx,
    const CanonicalQuery& canonicalQuery,
    const MultipleCollectionAccessor& collections) {
    if (canonicalQuery.cqPipeline().empty()) {
        return;
    }
    auto fillOutSecondaryInfo = [&](const NamespaceString& nss,
                                    const CollectionPtr& secondaryColl) {
        CollectionInfo secondaryInfo;
        secondaryInfo.options = providedOptions;
        if (secondaryColl) {
            fillOutIndexEntries(opCtx, canonicalQuery, secondaryColl, secondaryInfo.indexes);
            fillOutPlannerCollectionInfo(
                opCtx, secondaryColl, &secondaryInfo.stats, true /* include size stats */);
        } else {
            secondaryInfo.exists = false;
        }
        secondaryCollectionsInfo.emplace(nss, std::move(secondaryInfo));
    };
    for (auto& [collName, secondaryColl] : collections.getSecondaryCollections()) {
        fillOutSecondaryInfo(collName, secondaryColl);
    }

    // In the event of a self $lookup, we must have an entry for the main collection in the map
    // of secondary collections.
    if (collections.hasMainCollection()) {
        const auto& mainColl = collections.getMainCollection();
        fillOutSecondaryInfo(mainColl->ns(), mainColl);
    }

    auto indexHintSpecs = canonicalQuery.getExpCtx()->getQuerySettings().getIndexHints();
    const bool shouldIgnoreQuerySettings = mainCollectionInfo.options & IGNORE_QUERY_SETTINGS;
    if (!indexHintSpecs || shouldIgnoreQuerySettings) {
        return;
    }

    for (const auto& [name, coll] : collections.getSecondaryCollections()) {
        if (coll) {
            auto& collInfo = secondaryCollectionsInfo[name];
            applyQuerySettingsForCollection(canonicalQuery,
                                            coll->ns(),
                                            *indexHintSpecs,
                                            collInfo,
                                            coll->getTimeseriesOptions());
        }
    }
}

void QueryPlannerParams::fillOutMainCollectionPlannerParams(
    OperationContext* opCtx,
    const CanonicalQuery& canonicalQuery,
    const MultipleCollectionAccessor& collections,
    QueryPlanRankerModeEnum planRankerMode) {
    const auto& mainColl = collections.getMainCollection();
    // We will not output collection scans unless there are no indexed solutions. NO_TABLE_SCAN
    // overrides this behavior by not outputting a collscan even if there are no indexed
    // solutions.
    if (storageGlobalParams.noTableScan.load()) {
        const auto& nss = canonicalQuery.nss();
        // There are certain cases where we ignore this restriction:
        bool ignore =
            canonicalQuery.getQueryObj().isEmpty() || nss.isSystem() || nss.isOnInternalDb();
        if (!ignore) {
            mainCollectionInfo.options |= QueryPlannerParams::NO_TABLE_SCAN;
        }
    }

    if (internalQueryPlannerEnableIndexIntersection.load()) {
        mainCollectionInfo.options |= QueryPlannerParams::INDEX_INTERSECTION;
    }

    if (internalQueryEnumerationPreferLockstepOrEnumeration.load()) {
        mainCollectionInfo.options |= QueryPlannerParams::ENUMERATE_OR_CHILDREN_LOCKSTEP;
    }

    if (internalQueryPlannerGenerateCoveredWholeIndexScans.load()) {
        mainCollectionInfo.options |= QueryPlannerParams::GENERATE_COVERED_IXSCANS;
    }

    if (shouldWaitForOplogVisibility(
            opCtx, mainColl, canonicalQuery.getFindCommandRequest().getTailable())) {
        mainCollectionInfo.options |= QueryPlannerParams::OPLOG_SCAN_WAIT_FOR_VISIBLE;
    }

    // Populate collection statistics for CBR. In the case of clustered collections, a query may
    // appear to be ID-hack eligible as per 'isIdHackEligibleQuery()', but 'buildIdHackPlan()' fails
    // as there is no _id index. In these cases, we will end up invoking the query planner and CBR,
    // so we need this catalog information.
    if (planRankerMode != QueryPlanRankerModeEnum::kMultiPlanning) {
        mainCollectionInfo.collStats = std::make_unique<stats::CollectionStatisticsImpl>(
            static_cast<double>(mainColl->getRecordStore()->numRecords()), canonicalQuery.nss());
    }

    // _id queries can skip checking the catalog for indices since they will always use the _id
    // index.
    if (isIdHackEligibleQuery(mainColl, canonicalQuery)) {
        return;
    }

    // If it's not NULL, we may have indices. Access the catalog and fill out IndexEntry(s)
    fillOutIndexEntries(opCtx, canonicalQuery, mainColl, mainCollectionInfo.indexes);
    applyQuerySettingsOrIndexFiltersForMainCollection(canonicalQuery, collections);

    fillOutPlannerCollectionInfo(
        opCtx, mainColl, &mainCollectionInfo.stats, false /* includeSizeStats */);
}

void QueryPlannerParams::setTargetSbeStageBuilder(OperationContext* opCtx,
                                                  const CanonicalQuery& canonicalQuery,
                                                  const MultipleCollectionAccessor& collections) {
    // Set 'TARGET_SBE_STAGE_BUILDER' on the main collection and the secondary collections. We
    // also update 'providedOptions' in case fillOutSecondaryCollectionsPlannerParams() hasn't
    // been called yet.
    providedOptions |= QueryPlannerParams::TARGET_SBE_STAGE_BUILDER;
    mainCollectionInfo.options |= QueryPlannerParams::TARGET_SBE_STAGE_BUILDER;

    for (auto it = secondaryCollectionsInfo.begin(); it != secondaryCollectionsInfo.end(); ++it) {
        it->second.options |= QueryPlannerParams::TARGET_SBE_STAGE_BUILDER;
    }
}

namespace {
std::vector<IndexEntry> getIndexEntriesForDistinct(
    const QueryPlannerParams::ArgsForDistinct& distinctArgs) {
    std::vector<IndexEntry> indices;

    auto* opCtx = distinctArgs.opCtx;
    const auto& canonicalQuery = distinctArgs.canonicalQuery;
    const auto& query = canonicalQuery.getFindCommandRequest().getFilter();
    const auto& key = canonicalQuery.getDistinct()->getKey();
    const auto& collectionPtr = distinctArgs.collections.getMainCollection();

    // If the caller did not request a "strict" distinct scan then we may choose a plan which
    // unwinds arrays and treats each element in an array as its own key.
    const bool mayUnwindArrays =
        !(distinctArgs.plannerOptions & QueryPlannerParams::STRICT_DISTINCT_ONLY);

    auto ii =
        collectionPtr->getIndexCatalog()->getIndexIterator(IndexCatalog::InclusionPolicy::kReady);
    while (ii->more()) {
        const IndexCatalogEntry* ice = ii->next();
        const IndexDescriptor* desc = ice->descriptor();

        // Skip the addition of hidden indexes to prevent use in query planning.
        if (desc->hidden()) {
            continue;
        }

        if (desc->keyPattern().hasField(key)) {
            // This handles regular fields of Compound Wildcard Indexes as well.
            if (distinctArgs.flipDistinctScanDirection && ice->isMultikey(opCtx, collectionPtr)) {
                // This CanonicalDistinct was generated as a result of transforming a $group with
                // $last accumulators using the GroupFromFirstTransformation. We cannot use a
                // DISTINCT_SCAN if $last is being applied to an indexed field which is multikey,
                // even if the 'canonicalDistinct' key does not include multikey paths. This is
                // because changing the sort direction also changes the comparison semantics for
                // arrays, which means that flipping the scan may not exactly flip the order that we
                // see documents in. In the case of using DISTINCT_SCAN for $group, that would mean
                // that $first of the flipped scan may not be the same document as $last from the
                // user's requested sort order.
                continue;
            }

            if (!mayUnwindArrays &&
                isAnyComponentOfPathOrProjectionMultikey(
                    desc->keyPattern(),
                    ice->isMultikey(opCtx, collectionPtr),
                    ice->getMultikeyPaths(opCtx, collectionPtr),
                    key)) {
                // If the caller requested "strict" distinct that does not "pre-unwind" arrays,
                // then an index which is multikey on the distinct field may not be used. This is
                // because when indexing an array each element gets inserted individually. Any plan
                // which involves scanning the index will have effectively "unwound" all arrays.
                continue;
            }

            indices.push_back(
                indexEntryFromIndexCatalogEntry(opCtx, collectionPtr, *ice, canonicalQuery));
        } else if (desc->getIndexType() == IndexType::INDEX_WILDCARD && !query.isEmpty()) {
            // Check whether the $** projection captures the field over which we are distinct-ing.
            auto* proj = static_cast<const WildcardAccessMethod*>(ice->accessMethod())
                             ->getWildcardProjection()
                             ->exec();
            if (projection_executor_utils::applyProjectionToOneField(proj, key)) {
                indices.push_back(
                    indexEntryFromIndexCatalogEntry(opCtx, collectionPtr, *ice, canonicalQuery));
            }

            // It is not necessary to do any checks about 'mayUnwindArrays' in this case, because:
            // 1) If there is no predicate on the distinct(), a wildcard indices may not be used.
            // 2) distinct() _with_ a predicate may not be answered with a DISTINCT_SCAN on _any_
            // multikey index.

            // So, we will not distinct scan a wildcard index that's multikey on the distinct()
            // field, regardless of the value of 'mayUnwindArrays'.
        }
    }

    return indices;
}
}  // namespace

QueryPlannerParams::QueryPlannerParams(QueryPlannerParams::ArgsForDistinct&& distinctArgs) {
    mainCollectionInfo.options = QueryPlannerParams::NO_TABLE_SCAN | distinctArgs.plannerOptions;

    if (!distinctArgs.collections.hasMainCollection()) {
        return;
    }

    const auto& canonicalQuery = distinctArgs.canonicalQuery;
    if (canonicalQuery.getExpCtx()->isFeatureFlagShardFilteringDistinctScanEnabled()) {
        // When shard filtering for distinct scan is enabled and shard filtering is requested, we
        // need to check if the collection is actually sharded and fill out the shard key if it is.
        fillOutPlannerParamsForExpressQuery(
            distinctArgs.opCtx, canonicalQuery, distinctArgs.collections.getMainCollection());
    }

    mainCollectionInfo.indexes = getIndexEntriesForDistinct(distinctArgs);
    applyQuerySettingsOrIndexFiltersForMainCollection(canonicalQuery, distinctArgs.collections);

    // If there exists an index filter, we ignore all hints. Else, we only keep the index specified
    // by the hint. Since we cannot have an index with name $natural, that case will clear the
    // plannerParams.indices.
    const BSONObj& hint = canonicalQuery.getFindCommandRequest().getHint();
    if (!indexFiltersApplied && !querySettingsApplied && !hint.isEmpty()) {
        mainCollectionInfo.indexes =
            QueryPlannerIXSelect::findIndexesByHint(hint, mainCollectionInfo.indexes);
    }
}

bool shouldWaitForOplogVisibility(OperationContext* opCtx,
                                  const CollectionPtr& collection,
                                  bool tailable) {
    // Only non-tailable cursors on the oplog are affected. Only forward cursors, not reverse
    // cursors, are affected, but this is checked when the cursor is opened.
    if (!collection->ns().isOplog() || tailable) {
        return false;
    }

    // Only primaries should require readers to wait for oplog visibility. In any other replication
    // state, readers read at the most visible oplog timestamp. The reason why readers on primaries
    // need to wait is because multiple optimes can be allocated for operations before their entries
    // are written to the storage engine. "Holes" will appear when an operation with a later optime
    // commits before an operation with an earlier optime, and readers should wait so that all data
    // is consistent.
    //
    // On secondaries, the wait is done while holding a global lock, and the oplog visibility
    // timestamp is updated at the end of every batch on a secondary, signalling the wait to
    // complete. If a replication worker had a global lock and temporarily released it, a reader
    // could acquire the lock to read the oplog. If the secondary reader were to wait for the oplog
    // visibility timestamp to be updated, it would wait for a replication batch that would never
    // complete because it couldn't reacquire its own lock, the global lock held by the waiting
    // reader.
    auto* replCoord = repl::ReplicationCoordinator::get(opCtx);
    return replCoord->canAcceptWritesForDatabase(opCtx, DatabaseName::kAdmin) &&
        replCoord->getSettings().isReplSet();
}

bool QueryPlannerParams::requiresShardFiltering(const CanonicalQuery& canonicalQuery,
                                                const CollectionPtr& collection) {
    if (!(mainCollectionInfo.options & INCLUDE_SHARD_FILTER)) {
        // Shard filter was not requested; cmd may not be from a router.
        return false;
    }
    // If the caller wants a shard filter, make sure we're actually sharded.
    if (!collection.isSharded_DEPRECATED()) {
        // Not actually sharded.
        return false;
    }

    // Check whether the query is running over multiple shards and will require merging.
    const auto expCtx = canonicalQuery.getExpCtx();
    if (expCtx->needsUnsortedMerge() || expCtx->needsSortedMerge()) {
        return true;
    }

    const auto& shardKeyPattern = collection.getShardKeyPattern();
    // Shards cannot own orphans for the key ranges they own, so there is no need
    // to include a shard filtering stage. By omitting the shard filter, it may be
    // possible to get a more efficient plan (for example, a COUNT_SCAN may be used if
    // the query is eligible).
    const BSONObj extractedKey = extractShardKeyFromQuery(shardKeyPattern, canonicalQuery);

    if (extractedKey.isEmpty()) {
        // Couldn't extract all the fields of the shard key from the query,
        // no way to target a single shard.
        return true;
    }

    return !isSingleShardTargetable(
        extractedKey,
        shardKeyPattern,
        CollatorInterface::isSimpleCollator(canonicalQuery.getCollator()));
}
}  // namespace mongo
