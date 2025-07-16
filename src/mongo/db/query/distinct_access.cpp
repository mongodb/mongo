/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/query/distinct_access.h"

#include "mongo/db/exec/index_path_projection.h"
#include "mongo/db/exec/projection_executor_utils.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution_helpers.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/planner_analysis.h"
#include "mongo/db/query/planner_wildcard_helpers.h"
#include "mongo/db/query/query_planner.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

namespace {

/**
 * Check if an index is suitable for the DISTINCT_SCAN transition. The function represents the
 * extracted condition used by `getIndexEntriesForDistinct()` which generates all the suitable
 * indexes in case of not multiplanning.
 */
bool isIndexSuitableForDistinct(const IndexEntry& index,
                                const std::string& field,
                                const OrderedPathSet& projectionFields,
                                const BSONObj& filter,
                                bool flipDistinctScanDirection,
                                bool strictDistinctOnly,
                                bool hasSort) {
    // If the caller did not request a "strict" distinct scan then we may choose a plan which
    // unwinds arrays and treats each element in an array as its own key.
    const bool mayUnwindArrays = !strictDistinctOnly;

    if (index.keyPattern.hasField(field)) {
        // This handles regular fields of Compound Wildcard Indexes as well.
        if (flipDistinctScanDirection && index.multikey) {
            // This CanonicalDistinct was generated as a result of transforming a $group with
            // $last accumulators using the GroupFromFirstTransformation. We cannot use a
            // DISTINCT_SCAN if $last is being applied to an indexed field which is multikey,
            // even if the 'canonicalDistinct' key does not include multikey paths. This is
            // because changing the sort direction also changes the comparison semantics for
            // arrays, which means that flipping the scan may not exactly flip the order that we
            // see documents in. In the case of using DISTINCT_SCAN for $group, that would mean
            // that $first of the flipped scan may not be the same document as $last from the
            // user's requested sort order.
            return false;
        }
        if (!mayUnwindArrays &&
            isAnyComponentOfPathOrProjectionMultikey(index.keyPattern,
                                                     index.multikey,
                                                     index.multikeyPaths,
                                                     field,
                                                     projectionFields,
                                                     hasSort)) {
            // If the caller requested "strict" distinct that does not "pre-unwind" arrays,
            // then an index which is multikey on the distinct field may not be used. This is
            // because when indexing an array each element gets inserted individually. Any plan
            // which involves scanning the index will have effectively "unwound" all arrays.
            return false;
        }
        return true;
    } else if (index.type == IndexType::INDEX_WILDCARD && !filter.isEmpty()) {
        // Check whether the $** projection captures the field over which we are distinct-ing.
        if (index.indexPathProjection != nullptr &&
            projection_executor_utils::applyProjectionToOneField(index.indexPathProjection->exec(),
                                                                 field)) {
            return true;
        }
        // It is not necessary to do any checks about 'mayUnwindArrays' in this case, because:
        // 1) If there is no predicate on the distinct(), a wildcard indices may not be used.
        // 2) distinct() _with_ a predicate may not be answered with a DISTINCT_SCAN on _any_
        // multikey index.

        // So, we will not distinct scan a wildcard index that's multikey on the distinct()
        // field, regardless of the value of 'mayUnwindArrays'.
    }
    return false;
}

bool isAFullIndexScanPreferable(const IndexEntry& index,
                                const std::string& field,
                                const CollatorInterface* collator) {
    // Skip indices with non-matching collator.
    if (!CollatorInterface::collatorsMatch(index.collator, collator)) {
        return true;
    }
    // Skip partial indices.
    if (index.filterExpr) {
        return true;
    }
    // Skip indices where the first key is not 'field'.
    auto firstIndexField = index.keyPattern.firstElement();
    if (firstIndexField.fieldNameStringData() != StringData(field)) {
        return true;
    }
    // Skip the index if the first key is a "plugin" such as "hashed", "2dsphere", and so on.
    if (!firstIndexField.isNumber()) {
        return true;
    }
    // Compound hashed indexes can use distinct scan if the first field is 1 or -1. For the
    // other special indexes, the 1 or -1 index fields may be stored as a function of the data
    // rather than the raw data itself. Storing f(d) instead of 'd' precludes the distinct_scan
    // due to the possibility that f(d1) == f(d2).  Therefore, after fetching the base data,
    // either d1 or d2 would be incorrectly missing from the result set.
    auto indexPluginName = IndexNames::findPluginName(index.keyPattern);
    switch (IndexNames::nameToType(indexPluginName)) {
        case IndexType::INDEX_BTREE:
        case IndexType::INDEX_HASHED:
            break;
        default:
            // All other index types are not eligible.
            return true;
    }
    return false;
}

bool indexCoversProjection(const IndexEntry& index, const OrderedPathSet& projFields) {
    if (projFields.empty()) {
        return false;
    }
    size_t coveredFieldsCount = 0;
    for (const auto& field : index.keyPattern) {
        if (projFields.find(field.fieldNameStringData()) != projFields.end()) {
            coveredFieldsCount++;
        }
    }
    return coveredFieldsCount == projFields.size();
}

/**
 * Returns true if indices contains an index that can be used with DistinctNode (the "fast
 * distinct hack" node, which can be used only if there is an empty query predicate). Sets
 * indexOut to the array index of PlannerParams::indices. Criteria for suitable index is that
 * the index should be of type BTREE or HASHED and the index cannot be a partial index.
 *
 * If there is a projection and at least one index that covers all its fields, the smallest such
 * index is selected. Otherwise, select the index with the fewest total fields.
 *
 * Multikey indices are not suitable for DistinctNode when the projection is on an array
 * element. Arrays are flattened in a multikey index which makes it impossible for the distinct
 * scan stage (plan stage generated from DistinctNode) to select the requested element by array
 * index.
 *
 * Multikey indices cannot be used for the fast distinct hack if the field is dotted. Currently
 * the solution generated for the distinct hack includes a projection stage and the projection
 * stage cannot be covered with a dotted field.
 */
bool getDistinctNodeIndex(const std::vector<IndexEntry>& indices,
                          const std::string& key,
                          const OrderedPathSet& projectionFields,
                          const CollatorInterface* collator,
                          bool flipDistinctScanDirection,
                          bool strictDistinctOnly,
                          bool hasSort,
                          size_t* indexOut) {
    tassert(951520, "indexOut must be initialized", indexOut);
    size_t minIndexFields = Ordering::kMaxCompoundIndexKeys + 1;
    bool someIndexCoversProj = false;
    for (size_t i = 0; i < indices.size(); ++i) {
        // If we're here, it means the query does not have a filter.
        if (!isIndexSuitableForDistinct(indices[i],
                                        key,
                                        projectionFields,
                                        {} /*filter*/,
                                        flipDistinctScanDirection,
                                        strictDistinctOnly,
                                        hasSort)) {
            continue;
        }
        if (isAFullIndexScanPreferable(indices[i], key, collator)) {
            continue;
        }
        const size_t nFields = indices[i].keyPattern.nFields();
        bool currIndexCoversProj = indexCoversProjection(indices[i], projectionFields);
        if (currIndexCoversProj && (!someIndexCoversProj || nFields < minIndexFields)) {
            // Pick this index if it's the first covering index or it has fewer fields than the
            // current smallest covering index.
            minIndexFields = nFields;
            *indexOut = i;
            someIndexCoversProj = true;
        } else if (!currIndexCoversProj && !someIndexCoversProj && nFields < minIndexFields) {
            // No covering index found yet, so pick this one if it's smaller than the current
            // smallest.
            minIndexFields = nFields;
            *indexOut = i;
        }
    }
    return minIndexFields <= Ordering::kMaxCompoundIndexKeys;
}
}  // namespace

std::unique_ptr<QuerySolution> constructCoveredDistinctScan(
    const CanonicalQuery& canonicalQuery,
    const QueryPlannerParams& plannerParams,
    const CanonicalDistinct& canonicalDistinct) {
    size_t distinctNodeIndex = 0;
    auto collator = canonicalQuery.getCollator();
    const bool strictDistinctOnly =
        plannerParams.mainCollectionInfo.options & QueryPlannerParams::STRICT_DISTINCT_ONLY;

    if (getDistinctNodeIndex(plannerParams.mainCollectionInfo.indexes,
                             canonicalDistinct.getKey(),
                             canonicalQuery.getProj()
                                 ? canonicalQuery.getProj()->getRequiredFields()
                                 : OrderedPathSet{},
                             collator,
                             canonicalDistinct.isDistinctScanDirectionFlipped(),
                             strictDistinctOnly,
                             canonicalQuery.getDistinct()->getSortRequirement().has_value() ||
                                 canonicalQuery.getSortPattern(),
                             &distinctNodeIndex)) {
        // Hand-construct a distinct scan plan. Note that this is not a valid plan yet.
        // 'analyzeDataAccess()' will add additional stages as needed and call
        // 'finalizeDistinctScan()', which finalizes the plan by pushing FETCH and SHARDING_FILTER
        // stages to the distinct scan.
        auto dn = std::make_unique<DistinctNode>(
            plannerParams.mainCollectionInfo.indexes[distinctNodeIndex]);
        dn->direction = 1;
        IndexBoundsBuilder::allValuesBounds(
            dn->index.keyPattern, &dn->bounds, dn->index.collator != nullptr);
        dn->queryCollator = collator;
        dn->fieldNo = 0;

        // An index with a non-simple collation requires a FETCH stage.
        std::unique_ptr<QuerySolutionNode> solnRoot = std::move(dn);
        if (plannerParams.mainCollectionInfo.indexes[distinctNodeIndex].collator) {
            if (!solnRoot->fetched()) {
                auto fetch = std::make_unique<FetchNode>();
                fetch->children.push_back(std::move(solnRoot));
                solnRoot = std::move(fetch);
            }
        }

        // While on this path there are no sort or filter, the solution still needs to create
        // the projection and 'analyzeDataAccess()' would do that. NB: whether other aspects of
        // data access are important, it's hard to say, this code has been like this since long
        // ago (and it has always passed in new 'QueryPlannerParams').
        std::unique_ptr<QuerySolution> soln;
        if (canonicalQuery.getExpCtx()->isFeatureFlagShardFilteringDistinctScanEnabled()) {
            soln = QueryPlannerAnalysis::analyzeDataAccess(
                canonicalQuery, plannerParams, std::move(solnRoot));
        } else {
            soln = QueryPlannerAnalysis::analyzeDataAccess(
                canonicalQuery,
                // TODO SERVER-87683 Investigate why empty parameters are used instead of
                // 'plannerParams'.
                QueryPlannerParams{QueryPlannerParams::ArgsForTest{}},
                std::move(solnRoot));
        }
        uassert(8404000, "Failed to finalize a DISTINCT_SCAN plan", soln);
        return soln;
    }
    return nullptr;
}

std::unique_ptr<QuerySolution> createDistinctScanSolution(const CanonicalQuery& canonicalQuery,
                                                          const QueryPlannerParams& plannerParams,
                                                          bool flipDistinctScanDirection) {
    const CanonicalDistinct& canonicalDistinct = *canonicalQuery.getDistinct();
    if (canonicalQuery.getFindCommandRequest().getFilter().isEmpty() &&
        !canonicalQuery.getSortPattern() && !canonicalDistinct.getSortRequirement()) {
        // If a query has neither a filter nor a sort, the query planner won't attempt to use an
        // index for it even if the index could provide the distinct semantics on the key from the
        // 'canonicalDistinct'. So, we create the solution "manually" from a suitable index.
        // The direction of the index doesn't matter in this case.
        return constructCoveredDistinctScan(canonicalQuery, plannerParams, canonicalDistinct);
    } else {
        // Ask the QueryPlanner for a list of solutions that scan one of the indexes from
        // 'plannerParams' (i.e., the indexes that include the distinct field). Then try to convert
        // one of these plans to a DISTINCT_SCAN.
        auto multiPlanSolns = QueryPlanner::plan(canonicalQuery, plannerParams);
        if (multiPlanSolns.isOK()) {
            auto& solutions = multiPlanSolns.getValue();
            for (size_t i = 0; i < solutions.size(); ++i) {
                if (finalizeDistinctScan(canonicalQuery,
                                         plannerParams,
                                         solutions[i].get(),
                                         canonicalDistinct.getKey(),
                                         flipDistinctScanDirection)) {
                    // The first suitable distinct scan is as good as any other.
                    return std::move(solutions[i]);
                }
            }
        }
    }
    return nullptr;  // no suitable solution has been found
}

bool isAnyComponentOfPathOrProjectionMultikey(const BSONObj& indexKeyPattern,
                                              bool isMultikey,
                                              const MultikeyPaths& indexMultikeyInfo,
                                              StringData path,
                                              const OrderedPathSet& projFields,
                                              bool hasSort) {
    if (!isMultikey) {
        return false;
    }

    size_t keyPatternFieldIndex = 0;
    if (indexMultikeyInfo.empty()) {
        // There is no path-level multikey information available, so we must assume 'path' is
        // multikey.
        return true;
    }

    for (auto&& elt : indexKeyPattern) {
        const auto field = elt.fieldNameStringData();
        // If we are checking for a multikey projection and have not specified a sort, plan
        // enumeration will be bypassed. This will skip checks that would usually stop us from
        // creating a covered IXSCAN (and, by extension, a DISTINCT_SCAN), so we need to prevent
        // that case here.
        if (field == path || (!hasSort && projFields.find(field) != projFields.end())) {
            LOGV2_DEBUG(9723800,
                        5,
                        "Checking multikeyness",
                        "keyPatternFieldIndex"_attr = keyPatternFieldIndex,
                        "indexMultikeyInfoSize"_attr = indexMultikeyInfo.size());
            tassert(9723801,
                    "Smaller than expected indexMultikeyInfo size",
                    indexMultikeyInfo.size() > keyPatternFieldIndex);
            if (!indexMultikeyInfo[keyPatternFieldIndex].empty()) {
                return true;
            }
        }
        keyPatternFieldIndex++;
    }
    return false;
}

bool finalizeDistinctScan(const CanonicalQuery& canonicalQuery,
                          const QueryPlannerParams& plannerParams,
                          QuerySolution* soln,
                          const std::string& field,
                          bool flipDistinctScanDirection) {
    auto root = soln->root();

    // When a plan can be converted to use DISTINCT_SCAN, we may expect to see these nodes
    // on top of an IXSCAN.
    ProjectionNode* projectionNode = nullptr;
    FetchNode* fetchNode = nullptr;
    // If present, the sort key generator will remain in place.
    SortKeyGeneratorNode* sortKeyGenNode = nullptr;
    // If present, shard filtering will be pushed into the DISTINCT_SCAN.
    ShardingFilterNode* shardFilterNode = nullptr;
    // If the plan is already using a distinct scan, we may have to finalize the plan by pushing
    // fetch and shard filtering stages to the distinct scan.
    DistinctNode* distinctScanNode = nullptr;
    IndexScanNode* indexScanNode = nullptr;

    // Walk the solution until we find either IXSCAN, DISTINCT_SCAN or an unexpected stage.
    for (QuerySolutionNode* currNode = root;; currNode = currNode->children[0].get()) {
        switch (currNode->getType()) {
            case STAGE_PROJECTION_DEFAULT:
            case STAGE_PROJECTION_COVERED:
            case STAGE_PROJECTION_SIMPLE:
                tassert(9245800, "Didn't expect to find two projections", !projectionNode);
                projectionNode = static_cast<ProjectionNode*>(currNode);
                break;
            case STAGE_FETCH:
                if (fetchNode) {
                    // In some specific circumstances, we generate a second fetch node! For
                    // simplicity, bail out here.
                    return false;
                }
                fetchNode = static_cast<FetchNode*>(currNode);
                break;
            case STAGE_SORT_KEY_GENERATOR:
                tassert(9245802, "Didn't expect to find two sort key generators", !sortKeyGenNode);
                sortKeyGenNode = static_cast<SortKeyGeneratorNode*>(currNode);
                break;
            case STAGE_SHARDING_FILTER:
                tassert(9245803, "Didn't expect to find two sharding filters", !shardFilterNode);
                shardFilterNode = static_cast<ShardingFilterNode*>(currNode);
                break;
            case STAGE_IXSCAN:
                tassert(9245804, "Didn't expect to find two index scans", !indexScanNode);
                indexScanNode = static_cast<IndexScanNode*>(currNode);
                break;
            case STAGE_DISTINCT_SCAN:
                tassert(9245805, "Didn't expect to find two distinct scans", !distinctScanNode);
                distinctScanNode = static_cast<DistinctNode*>(currNode);
                break;
            default:
                return false;
        }
        if (currNode->children.size() != 1) {
            break;
        }
    }

    if (!indexScanNode && !distinctScanNode) {
        return false;
    }

    tassert(9245809,
            "Didn't expect to find both distinct and index scans",
            !indexScanNode || !distinctScanNode);

    // Preserve old behavior when shard filtering for distinct scan is not enabled.
    const bool isShardFilteringDistinctScanEnabled =
        canonicalQuery.getExpCtx()->isFeatureFlagShardFilteringDistinctScanEnabled();
    if ((shardFilterNode || distinctScanNode || sortKeyGenNode) &&
        !isShardFilteringDistinctScanEnabled) {
        return false;
    }

    const auto& indexEntry = indexScanNode ? indexScanNode->index : distinctScanNode->index;
    const auto& indexBounds = indexScanNode ? indexScanNode->bounds : distinctScanNode->bounds;
    const auto queryCollator =
        indexScanNode ? indexScanNode->queryCollator : distinctScanNode->queryCollator;

    const bool strictDistinctOnly =
        (plannerParams.mainCollectionInfo.options & QueryPlannerParams::STRICT_DISTINCT_ONLY);

    const BSONObj& filter = canonicalQuery.getFindCommandRequest().getFilter();
    // If a sort is required to maintain correct query semantics with DISTINCT_SCAN, we need to
    // ensure it is provided by the index.
    const bool hasSortRequirement = canonicalQuery.getDistinct()->getSortRequirement().has_value();
    const bool hasSort = canonicalQuery.getSortPattern() || hasSortRequirement;

    // When multiplanning for distinct is enabled, this function is reached from the query planner
    // which is also called by the fallback find path when multiplanning is disabled. In the latter
    // case, we have already filtered out indexes which are ineligible for conversion to
    // DISTINCT_SCAN, for e.g. if the distinct key is not part of the index. In the former case, we
    // have not done this check yet, so we filter out ineligible indexes here.
    if (isShardFilteringDistinctScanEnabled) {
        if (!isIndexSuitableForDistinct(indexEntry,
                                        field,
                                        OrderedPathSet{},
                                        filter,
                                        flipDistinctScanDirection,
                                        strictDistinctOnly,
                                        !hasSort)) {
            return false;
        }
        if (filter.isEmpty() && !hasSort &&
            isAFullIndexScanPreferable(indexEntry, field, queryCollator)) {
            return false;
        }
    }

    // If the fetch has a filter, we're out of luck. We can't skip all keys with a given value,
    // since one of them may key a document that passes the filter.
    if (fetchNode && fetchNode->filter) {
        return false;
    }

    if (indexEntry.type == IndexType::INDEX_WILDCARD) {
        // If the query is on a field other than the distinct key, we may have generated a $** plan
        // which does not actually contain the distinct key field.
        if (field != std::next(indexEntry.keyPattern.begin())->fieldName()) {
            return false;
        }
        // If the query includes object bounds, we cannot turn this IXSCAN into a DISTINCT_SCAN.
        // Wildcard indexes contain multiple keys per object, one for each subpath in ascending
        // (Path, Value, RecordId) order. If the distinct fields in two successive documents are
        // objects with the same leaf path values but in different field order, e.g. {a: 1, b: 2}
        // and {b: 2, a: 1}, we would therefore only return the first document and skip the other.
        if (wildcard_planning::isWildcardObjectSubpathScan(indexEntry, indexBounds)) {
            return false;
        }
    }

    // An additional filter must be applied to the data in the key, so we can't just skip
    // all the keys with a given value; we must examine every one to find the one that (may)
    // pass the filter.
    if (indexScanNode && indexScanNode->filter) {
        return false;
    }

    // We only set this when we have special query modifiers (.max() or .min()) or other
    // special cases.  Don't want to handle the interactions between those and distinct.
    // Don't think this will ever really be true but if it somehow is, just ignore this
    // soln.
    if (indexBounds.isSimpleRange) {
        return false;
    }

    // Figure out which field we're skipping to the next value of.
    int fieldNo = 0;
    BSONObjIterator it(indexEntry.keyPattern);
    while (it.more()) {
        if (field == it.next().fieldName()) {
            break;
        }
        ++fieldNo;
    }

    if (strictDistinctOnly) {
        // If the "distinct" field is not the first field in the index bounds then the only way we
        // can guarantee that we'll never see duplicate values for the distinct field is to make
        // sure every field before the distinct field has equality bounds. For example, a
        // DISTINCT_SCAN on 'b' over the {a: 1, b: 1} index will scan a particular 'b' value
        // multiple times if that 'b' value exists in documents with different 'a' values. The
        // equality bounds on 'a' prevent the scan from seeing duplicate 'b' values by ensuring the
        // scan is limited to a single value for the 'a' field.
        for (size_t i = 0; i < static_cast<size_t>(fieldNo); ++i) {
            tassert(9245810, "Smaller than expected indexBounds size", i < indexBounds.size());
            if (indexBounds.fields[i].intervals.size() != 1 ||
                !indexBounds.fields[i].intervals[0].isPoint()) {
                return false;
            }
        }
    }

    // In practice, the only query that can be answered by a distinct scan on a multikey field
    // is a distinct() with no filter.
    const bool canScanMultikeyPath = !strictDistinctOnly && filter.isEmpty() && !hasSort;

    // We should not use a distinct scan if the field over which we are computing the distinct is
    // multikey.
    if (indexEntry.multikey && !canScanMultikeyPath) {
        const auto& multikeyPaths = indexEntry.multikeyPaths;
        if (multikeyPaths.empty()) {
            // We don't have path-level multikey information available.
            return false;
        }

        if (!multikeyPaths[fieldNo].empty()) {
            // Path-level multikey information indicates that the distinct key contains at least one
            // array component.
            return false;
        }
    }

    // Multikeyness is currently not taken into account when deciding whether a distinct scan
    // direction can be reversed in 'analyzeNonBlockingSort()'. This is fine, because multikey
    // indexes are not allowed with STRICT_DISTINCT_ONLY.
    tassert(9261503,
            "Expected a strict distinct scan when the query has a sortRequirement",
            !hasSortRequirement || strictDistinctOnly);

    // If there are other factors that affect the scan direction, don't attempt to reverse it. Note
    // that 'flipDistinctScanDirection' is intentionally ignored here, since its purpose is to
    // implement $last/$bottom with a distinct scan by reversing the sort direction of the query.
    const bool reverseScanIfNeededToSatisfySortRequirement =
        !canonicalQuery.getSortPattern() && !plannerParams.traversalPreference;

    if (hasSortRequirement &&
        !QueryPlannerAnalysis::analyzeNonBlockingSort(
            plannerParams,
            canonicalQuery.getDistinct()->getSerializedSortRequirement(),
            canonicalQuery.getFindCommandRequest().getHint(),
            reverseScanIfNeededToSatisfySortRequirement,
            indexScanNode ? static_cast<QuerySolutionNode*>(indexScanNode)
                          : static_cast<QuerySolutionNode*>(distinctScanNode))) {
        return false;
    }

    const int direction = indexScanNode ? indexScanNode->direction : distinctScanNode->direction;
    // Make a new DistinctNode. We will swap this for the ixscan in the provided solution.
    auto distinctNode = std::make_unique<DistinctNode>(indexEntry);
    distinctNode->direction = flipDistinctScanDirection ? -direction : direction;
    distinctNode->bounds = flipDistinctScanDirection ? indexBounds.reverse() : indexBounds;
    distinctNode->queryCollator = queryCollator;
    distinctNode->fieldNo = fieldNo;
    distinctNode->isShardFiltering = shardFilterNode != nullptr;
    distinctNode->isFetching = isShardFilteringDistinctScanEnabled && fetchNode != nullptr;

    // The expected tree structure is (all nodes except IXSCAN can also be absent):
    // PROJECT => SORT_KEY_GENERATOR => SHARDING_FILTER => FETCH => IXSCAN.
    tassert(9245902, "Expected to have either PROJECT or FETCH", fetchNode || projectionNode);
    tassert(
        9245811, "Expected PROJECT to be the root node", !projectionNode || projectionNode == root);
    tassert(9245807,
            "Found SORT_KEY_GENERATOR in an unexpected location",
            !sortKeyGenNode || (!projectionNode && sortKeyGenNode == root) ||
                (projectionNode && sortKeyGenNode == projectionNode->children[0].get()));

    if (fetchNode) {
        // When fetching, everything in the tree except SORT_KEY_GENERATOR (i.e. PROJECT, FETCH,
        // SHARDING_FILTER) can be replaced with a DISTINCT_SCAN.
        if (projectionNode) {
            // Make the SORT_KEY_GENERATOR or FETCH the new root. This destroys the PROJECT
            // stage.
            soln->setRoot(std::move(root->children[0]));
        }
        // If shard filtering for distinct scan is enabled, we can remove the FETCH.
        if (isShardFilteringDistinctScanEnabled) {
            if (sortKeyGenNode) {
                // Replace FETCH (and possibly SHARDING_FILTER) with a DISTINCT_SCAN.
                sortKeyGenNode->children[0] = std::move(distinctNode);
            } else {
                soln->setRoot(std::move(distinctNode));
            }
        } else {
            // If shard filtering for distinct scan is disabled, maintain the old behavior. That is,
            // don't push FETCH into the DISTINCT_SCAN stage.
            fetchNode->children[0] = std::move(distinctNode);
        }
    } else {
        // There is no fetch node. The PROJECT=>IXSCAN tree should become
        // PROJECT=>DISTINCT_SCAN. If there is a shard filter, it will be pushed to the distinct
        // scan itself.
        QuerySolutionNode* parent = sortKeyGenNode ? sortKeyGenNode : root;
        parent->children[0] = std::move(distinctNode);
    }

    return true;
}

}  // namespace mongo
