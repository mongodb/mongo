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
#include "mongo/db/index/columns_access_method.h"
#include "mongo/db/index/multikey_metadata_access_stats.h"
#include "mongo/db/index/wildcard_access_method.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/planner_ixselect.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/query/query_settings/query_settings_manager.h"
#include "mongo/db/query/query_settings_decoration.h"
#include "mongo/db/query/query_utils.h"
#include "mongo/db/query/wildcard_multikey_paths.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/util/processinfo.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

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
            wildcardProjection};
}

/**
 * Converts the catalog metadata for an index into an ColumnIndexEntry, which is a format that is
 * meant to be consumed by the query planner. This function can perform index reads and should not
 * be called unless access to the storage engine is permitted.
 */
ColumnIndexEntry columnIndexEntryFromIndexCatalogEntry(OperationContext* opCtx,
                                                       const CollectionPtr& collection,
                                                       const IndexCatalogEntry& ice) {

    auto desc = ice.descriptor();
    invariant(desc);

    auto accessMethod = ice.accessMethod();
    invariant(accessMethod);

    auto cam = static_cast<const ColumnStoreAccessMethod*>(accessMethod);
    const auto columnstoreProjection = cam->getColumnstoreProjection();

    return {desc->keyPattern(),
            desc->getIndexType(),
            desc->version(),
            desc->isSparse(),
            desc->unique(),
            ColumnIndexEntry::Identifier{desc->indexName()},
            ice.getFilterExpression(),
            ice.getCollator(),
            columnstoreProjection};
}

void fillOutIndexEntries(OperationContext* opCtx,
                         const CanonicalQuery& canonicalQuery,
                         const CollectionPtr& collection,
                         std::vector<IndexEntry>& entries,
                         std::vector<ColumnIndexEntry>& columnEntries) {
    bool apiStrict = APIParameters::get(opCtx).getAPIStrict().value_or(false);

    std::vector<const IndexCatalogEntry*> columnIndexes, plainIndexes;
    auto ii = collection->getIndexCatalog()->getIndexIterator(
        opCtx, IndexCatalog::InclusionPolicy::kReady);
    while (ii->more()) {
        const IndexCatalogEntry* ice = ii->next();

        // Indexes excluded from API version 1 should _not_ be used for planning if apiStrict is
        // set to true.
        auto indexType = ice->descriptor()->getIndexType();
        if (apiStrict &&
            (indexType == IndexType::INDEX_HAYSTACK || indexType == IndexType::INDEX_TEXT ||
             indexType == IndexType::INDEX_COLUMN || ice->descriptor()->isSparse())) {
            continue;
        }

        // Skip the addition of hidden indexes to prevent use in query planning.
        if (ice->descriptor()->hidden()) {
            continue;
        }

        if (indexType == IndexType::INDEX_COLUMN) {
            columnIndexes.push_back(ice);
        } else {
            plainIndexes.push_back(ice);
        }
    }
    columnEntries.reserve(columnIndexes.size());
    for (auto ice : columnIndexes) {
        columnEntries.emplace_back(columnIndexEntryFromIndexCatalogEntry(opCtx, collection, *ice));
    }
    entries.reserve(plainIndexes.size());
    for (auto ice : plainIndexes) {
        entries.emplace_back(
            indexEntryFromIndexCatalogEntry(opCtx, collection, *ice, canonicalQuery));
    }
}

void fillOutPlannerCollectionInfo(OperationContext* opCtx,
                                  const CollectionPtr& collection,
                                  PlannerCollectionInfo* out,
                                  bool includeSizeStats) {
    out->isTimeseries = static_cast<bool>(collection->getTimeseriesOptions());
    if (includeSizeStats) {
        // We only include these sometimes, since they are slightly expensive to compute.
        auto recordStore = collection->getRecordStore();
        out->noOfRecords = recordStore->numRecords(opCtx);
        out->approximateDataSizeBytes = recordStore->dataSize(opCtx);
        out->storageSizeBytes = recordStore->storageSize(opCtx);
    }
}
}  // namespace

void QueryPlannerParams::applyQuerySettingsIndexHintsForCollection(
    const CanonicalQuery& canonicalQuery,
    const CollectionPtr& collection,
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
    const CollectionPtr& collection,
    const std::vector<mongo::IndexHint>& allowedIndexes,
    std::vector<IndexEntry>& indexes) {
    // TODO SERVER-86400: Add support for $natural hints on secondary collections.
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

    if (!forwardAllowed && !backwardAllowed) {
        // No '$natural' or cluster key hint present. Ensure that table scans are forbidden.
        options |= QueryPlannerParams::Options::NO_TABLE_SCAN;
    } else {
        // At least one direction is allowed. Clear out the 'NO_TABLE_SCAN' flag if it exists,
        // as query settings should have a higher precedence over server parameters.
        options &= ~QueryPlannerParams::Options::NO_TABLE_SCAN;

        // Enforce the scan direction if needed.
        const bool bothDirectionsAllowed = forwardAllowed && backwardAllowed;
        if (!bothDirectionsAllowed) {
            collscanDirection = forwardAllowed ? NaturalOrderHint::Direction::kForward
                                               : NaturalOrderHint::Direction::kBackward;
        }
    }
}

void QueryPlannerParams::applyQuerySettingsForCollection(
    const CanonicalQuery& canonicalQuery,
    const CollectionPtr& collection,
    const std::variant<std::vector<mongo::query_settings::IndexHintSpec>,
                       mongo::query_settings::IndexHintSpec>& indexHintSpecs,
    std::vector<IndexEntry>& indexes) {
    // Retrieving the allowed indexes for the given collection.
    auto hintToNs = [&collection](const auto& hint) -> boost::optional<NamespaceString> {
        if (!hint.getNs()) {
            return boost::none;
        }
        return NamespaceStringUtil::deserialize(collection->ns().tenantId(),
                                                hint.getNs()->getDb(),
                                                hint.getNs()->getColl(),
                                                SerializationContext::stateDefault());
    };
    auto allowedIndexes = visit(
        OverloadedVisitor{
            [&](const std::vector<mongo::query_settings::IndexHintSpec>& hints) {
                auto isHintForCollection = [&](const mongo::query_settings::IndexHintSpec& hint) {
                    return *hintToNs(hint) == collection->ns();
                };

                if (auto hintIt = std::find_if(hints.begin(), hints.end(), isHintForCollection);
                    hintIt != hints.end()) {
                    return hintIt->getAllowedIndexes();
                }
                return std::vector<mongo::IndexHint>();
            },
            [&](const mongo::query_settings::IndexHintSpec& hint) {
                auto hintNs = hintToNs(hint);
                if (!hintNs || *hintNs == collection->ns()) {
                    return hint.getAllowedIndexes();
                }

                return std::vector<mongo::IndexHint>();
            },
        },
        indexHintSpecs);

    // Users can not define empty allowedIndexes vector, therefore early exit if no hints are
    // defined for the given collection.
    if (allowedIndexes.empty()) {
        return;
    }

    applyQuerySettingsIndexHintsForCollection(canonicalQuery, collection, allowedIndexes, indexes);
    applyQuerySettingsNaturalHintsForCollection(
        canonicalQuery, collection, allowedIndexes, indexes);

    querySettingsApplied = true;
}

void QueryPlannerParams::applyIndexFilters(const CanonicalQuery& canonicalQuery,
                                           const CollectionPtr& collection) {
    auto filterAllowedIndexEntries = [](const AllowedIndicesFilter& allowedIndicesFilter,
                                        std::vector<IndexEntry>& indexEntries) {
        // Filter index entries
        // Check BSON objects in AllowedIndices::_indexKeyPatterns against IndexEntry::keyPattern.
        // Removes IndexEntrys that do not match _indexKeyPatterns.
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
        filterAllowedIndexEntries(*allowedIndicesFilter, indices);
        indexFiltersApplied = true;
    }
}

void QueryPlannerParams::applyQuerySettingsOrIndexFiltersForMainCollection(
    const CanonicalQuery& canonicalQuery,
    const MultipleCollectionAccessor& collections,
    bool shouldIgnoreQuerySettings) {
    // If 'querySettings' has no index hints specified, then there are no settings to be applied
    // to this query.
    auto indexHintSpecs = canonicalQuery.getExpCtx()->getQuerySettings().getIndexHints();
    if (indexHintSpecs && !shouldIgnoreQuerySettings) {
        applyQuerySettingsForCollection(
            canonicalQuery, collections.getMainCollection(), *indexHintSpecs, indices);
    }

    // Try to apply index filters only if query settings were not applied.
    if (!querySettingsApplied) {
        applyIndexFilters(canonicalQuery, collections.getMainCollection());
    }
}

void QueryPlannerParams::fillOutSecondaryCollectionsPlannerParams(
    OperationContext* opCtx,
    const CanonicalQuery& canonicalQuery,
    const MultipleCollectionAccessor& collections,
    bool shouldIgnoreQuerySettings) {
    if (canonicalQuery.cqPipeline().empty()) {
        return;
    }
    auto fillOutSecondaryInfo = [&](const NamespaceString& nss,
                                    const CollectionPtr& secondaryColl) {
        auto secondaryInfo = SecondaryCollectionInfo();
        if (secondaryColl) {
            fillOutIndexEntries(opCtx,
                                canonicalQuery,
                                secondaryColl,
                                secondaryInfo.indexes,
                                secondaryInfo.columnIndexes);
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
    if (!indexHintSpecs || shouldIgnoreQuerySettings) {
        return;
    }

    for (const auto& [name, coll] : collections.getSecondaryCollections()) {
        if (coll) {
            applyQuerySettingsForCollection(
                canonicalQuery, coll, *indexHintSpecs, secondaryCollectionsInfo[name].indexes);
        }
    }
}

void QueryPlannerParams::fillOutMainCollectionPlannerParams(
    OperationContext* opCtx,
    const CanonicalQuery& canonicalQuery,
    const MultipleCollectionAccessor& collections,
    bool shouldIgnoreQuerySettings) {
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
            options |= QueryPlannerParams::NO_TABLE_SCAN;
        }
    }

    if (internalQueryPlannerEnableIndexIntersection.load()) {
        options |= QueryPlannerParams::INDEX_INTERSECTION;
    }

    if (internalQueryEnumerationPreferLockstepOrEnumeration.load()) {
        options |= QueryPlannerParams::ENUMERATE_OR_CHILDREN_LOCKSTEP;
    }

    if (internalQueryPlannerGenerateCoveredWholeIndexScans.load()) {
        options |= QueryPlannerParams::GENERATE_COVERED_IXSCANS;
    }

    if (shouldWaitForOplogVisibility(
            opCtx, mainColl, canonicalQuery.getFindCommandRequest().getTailable())) {
        options |= QueryPlannerParams::OPLOG_SCAN_WAIT_FOR_VISIBLE;
    }

    // _id queries can skip checking the catalog for indices since they will always use the _id
    // index.
    if (isIdHackEligibleQuery(
            mainColl, canonicalQuery.getFindCommandRequest(), canonicalQuery.getCollator())) {
        return;
    }

    // If it's not NULL, we may have indices. Access the catalog and fill out IndexEntry(s)
    fillOutIndexEntries(opCtx, canonicalQuery, mainColl, indices, columnStoreIndexes);
    applyQuerySettingsOrIndexFiltersForMainCollection(
        canonicalQuery, collections, shouldIgnoreQuerySettings);

    fillOutPlannerCollectionInfo(opCtx,
                                 mainColl,
                                 &collectionStats,
                                 // Only include the full size stats when there's a CSI.
                                 !columnStoreIndexes.empty());

    if (!columnStoreIndexes.empty()) {
        // Only fill this out when a CSI is present.
        const auto kMB = 1024 * 1024;
        availableMemoryBytes = static_cast<long long>(ProcessInfo::getMemSizeMB()) * kMB;
    }
}

namespace {
std::vector<IndexEntry> getIndexEntriesForDistinct(
    const QueryPlannerParams::ArgsForDistinct& distinctArgs) {
    std::vector<IndexEntry> indices;

    auto* opCtx = distinctArgs.opCtx;
    const auto& canonicalQuery = *distinctArgs.canonicalDistinct.getQuery();
    const auto& query = canonicalQuery.getFindCommandRequest().getFilter();
    const auto& key = distinctArgs.canonicalDistinct.getKey();
    const auto& collectionPtr = distinctArgs.collections.getMainCollection();

    // If the caller did not request a "strict" distinct scan then we may choose a plan which
    // unwinds arrays and treats each element in an array as its own key.
    const bool mayUnwindArrays =
        !(distinctArgs.plannerOptions & QueryPlannerParams::STRICT_DISTINCT_ONLY);

    auto ii = collectionPtr->getIndexCatalog()->getIndexIterator(
        opCtx, IndexCatalog::InclusionPolicy::kReady);
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
                isAnyComponentOfPathMultikey(desc->keyPattern(),
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
    options = QueryPlannerParams::NO_TABLE_SCAN | distinctArgs.plannerOptions;

    if (!distinctArgs.collections.hasMainCollection()) {
        return;
    }

    indices = getIndexEntriesForDistinct(distinctArgs);
    const auto& canonicalQuery = *distinctArgs.canonicalDistinct.getQuery();
    applyQuerySettingsOrIndexFiltersForMainCollection(
        canonicalQuery, distinctArgs.collections, distinctArgs.ignoreQuerySettings);

    // If there exists an index filter, we ignore all hints. Else, we only keep the index specified
    // by the hint. Since we cannot have an index with name $natural, that case will clear the
    // plannerParams.indices.
    const BSONObj& hint = canonicalQuery.getFindCommandRequest().getHint();
    if (!indexFiltersApplied && !querySettingsApplied && !hint.isEmpty()) {
        indices = QueryPlannerIXSelect::findIndexesByHint(hint, indices);
    }
}

bool isAnyComponentOfPathMultikey(const BSONObj& indexKeyPattern,
                                  bool isMultikey,
                                  const MultikeyPaths& indexMultikeyInfo,
                                  StringData path) {
    if (!isMultikey) {
        return false;
    }

    size_t keyPatternFieldIndex = 0;
    bool found = false;
    if (indexMultikeyInfo.empty()) {
        // There is no path-level multikey information available, so we must assume 'path' is
        // multikey.
        return true;
    }

    for (auto&& elt : indexKeyPattern) {
        if (elt.fieldNameStringData() == path) {
            found = true;
            break;
        }
        keyPatternFieldIndex++;
    }
    invariant(found);

    invariant(indexMultikeyInfo.size() > keyPatternFieldIndex);
    return !indexMultikeyInfo[keyPatternFieldIndex].empty();
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
    return repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesForDatabase(
        opCtx, DatabaseName::kAdmin);
}

}  // namespace mongo
