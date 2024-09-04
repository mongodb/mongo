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
#include "mongo/db/query/planner_analysis.h"
#include "mongo/db/query/planner_wildcard_helpers.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/stage_types.h"

namespace mongo {

namespace {

/**
 * Check if an index is suitable for the DISTINCT_SCAN transition. The function represents the
 * extracted condition used by `getIndexEntriesForDistinct()` which generates all the suitable
 * indexes in case of not multiplanning.
 */
bool isIndexSuitableForDistinct(const CanonicalQuery& canonicalQuery,
                                IndexEntry& index,
                                const std::string& field,
                                bool flipDistinctScanDirection,
                                bool strictDistinctOnly) {
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
            isAnyComponentOfPathMultikey(
                index.keyPattern, index.multikey, index.multikeyPaths, field)) {
            // If the caller requested "strict" distinct that does not "pre-unwind" arrays,
            // then an index which is multikey on the distinct field may not be used. This is
            // because when indexing an array each element gets inserted individually. Any plan
            // which involves scanning the index will have effectively "unwound" all arrays.
            return false;
        }
        return true;
    } else if (index.type == IndexType::INDEX_WILDCARD &&
               !canonicalQuery.getFindCommandRequest().getFilter().isEmpty()) {
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

/**
 * Returns true if indices contains an index that can be used with DistinctNode (the "fast distinct
 * hack" node, which can be used only if there is an empty query predicate).  Sets indexOut to the
 * array index of PlannerParams::indices.  Look for the index for the fewest fields.  Criteria for
 * suitable index is that the index should be of type BTREE or HASHED and the index cannot be a
 * partial index.
 *
 * Multikey indices are not suitable for DistinctNode when the projection is on an array element.
 * Arrays are flattened in a multikey index which makes it impossible for the distinct scan stage
 * (plan stage generated from DistinctNode) to select the requested element by array index.
 *
 * Multikey indices cannot be used for the fast distinct hack if the field is dotted.  Currently the
 * solution generated for the distinct hack includes a projection stage and the projection stage
 * cannot be covered with a dotted field.
 */
bool getDistinctNodeIndex(const std::vector<IndexEntry>& indices,
                          const std::string& field,
                          const CollatorInterface* collator,
                          size_t* indexOut) {
    invariant(indexOut);
    int minFields = std::numeric_limits<int>::max();
    for (size_t i = 0; i < indices.size(); ++i) {
        if (isAFullIndexScanPreferable(indices[i], field, collator)) {
            continue;
        }

        int nFields = indices[i].keyPattern.nFields();
        // Pick the index with the lowest number of fields.
        if (nFields < minFields) {
            minFields = nFields;
            *indexOut = i;
        }
    }
    return minFields != std::numeric_limits<int>::max();
}

}  // namespace

std::unique_ptr<QuerySolution> createDistinctScanSolution(const CanonicalQuery& canonicalQuery,
                                                          const QueryPlannerParams& plannerParams,
                                                          bool isDistinctMultiplanningEnabled,
                                                          bool flipDistinctScanDirection) {
    const CanonicalDistinct& canonicalDistinct = *canonicalQuery.getDistinct();
    if (canonicalQuery.getFindCommandRequest().getFilter().isEmpty() &&
        !canonicalQuery.getSortPattern()) {
        // If a query has neither a filter nor a sort, the query planner won't attempt to use an
        // index for it even if the index could provide the distinct semantics on the key from the
        // 'canonicalDistinct'. So, we create the solution "manually" from a suitable index.
        // The direction of the index doesn't matter in this case.
        size_t distinctNodeIndex = 0;
        auto collator = canonicalQuery.getCollator();
        if (getDistinctNodeIndex(plannerParams.mainCollectionInfo.indexes,
                                 canonicalDistinct.getKey(),
                                 collator,
                                 &distinctNodeIndex)) {
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
            auto soln = QueryPlannerAnalysis::analyzeDataAccess(
                canonicalQuery,
                // TODO SERVER-87683 Investigate why empty parameters are used instead of
                // 'plannerParams'.
                QueryPlannerParams{QueryPlannerParams::ArgsForTest{}},
                std::move(solnRoot));
            uassert(8404000, "Failed to finalize a DISTINCT_SCAN plan", soln);
            return soln;
        }
    } else if (!isDistinctMultiplanningEnabled) {
        // If multiplanning for distinct is disabled, we will keep the old functionality of
        // returning the first query solution eligible for DISTINCT_SCAN. Otherwise we prefer to
        // fallback to a find command.

        // Ask the QueryPlanner for a list of solutions that scan one of the indexes from
        // 'plannerParams' (i.e., the indexes that include the distinct field). Then try to convert
        // one of these plans to a DISTINCT_SCAN.
        auto multiPlanSolns = QueryPlanner::plan(canonicalQuery, plannerParams);
        if (multiPlanSolns.isOK()) {
            auto& solutions = multiPlanSolns.getValue();
            const bool strictDistinctOnly = (plannerParams.mainCollectionInfo.options &
                                             QueryPlannerParams::STRICT_DISTINCT_ONLY);

            for (size_t i = 0; i < solutions.size(); ++i) {
                if (turnIxscanIntoDistinctScan(canonicalQuery,
                                               solutions[i].get(),
                                               canonicalDistinct.getKey(),
                                               strictDistinctOnly,
                                               flipDistinctScanDirection)) {
                    // The first suitable distinct scan is as good as any other.
                    return std::move(solutions[i]);
                }
            }
        }
    }
    return nullptr;  // no suitable solution has been found
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

bool turnIxscanIntoDistinctScan(const CanonicalQuery& canonicalQuery,
                                QuerySolution* soln,
                                const std::string& field,
                                bool strictDistinctOnly,
                                bool flipDistinctScanDirection) {
    auto root = soln->root();

    // Temporarily check if the plan already contains a distinct scan. That happens in the
    // aggregation code path where this function is called twice for a place. The only possible
    // plans are FETCH + DISTINCT or PROJECT + DISTINCT.
    //
    // TODO SERVER-92615: Remove this.
    if (root->children.size() > 0 && root->children[0]->getType() == STAGE_DISTINCT_SCAN) {
        return true;
    }

    // We can attempt to convert a plan if it follows one of these patterns (starting from the
    // root):
    //   1. PROJECT=>FETCH=>IXSCAN
    //   2. FETCH=>IXSCAN
    //   3. PROJECT=>IXSCAN
    QuerySolutionNode* projectNode = nullptr;
    IndexScanNode* indexScanNode = nullptr;
    FetchNode* fetchNode = nullptr;

    switch (root->getType()) {
        case STAGE_PROJECTION_DEFAULT:
        case STAGE_PROJECTION_COVERED:
        case STAGE_PROJECTION_SIMPLE:
            projectNode = root;
            break;
        case STAGE_FETCH:
            fetchNode = static_cast<FetchNode*>(root);
            break;
        default:
            return false;
    }

    if (!fetchNode && (STAGE_FETCH == root->children[0]->getType())) {
        fetchNode = static_cast<FetchNode*>(root->children[0].get());
    }

    if (fetchNode && (STAGE_IXSCAN == fetchNode->children[0]->getType())) {
        indexScanNode = static_cast<IndexScanNode*>(fetchNode->children[0].get());
    } else if (projectNode && (STAGE_IXSCAN == projectNode->children[0]->getType())) {
        indexScanNode = static_cast<IndexScanNode*>(projectNode->children[0].get());
    }

    if (!indexScanNode) {
        return false;
    }

    // When multiplanning for distinct is enabled, this function is reached from the query planner
    // which is also called by the fallback find path when multiplanning is disabled. In the latter
    // case, we have already filtered out indexes which are ineligible for conversion to
    // DISTINCT_SCAN, for e.g. if the distinct key is not part of the index. In the former case, we
    // have not done this check yet, so we filter out ineligible indexes here.
    const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    const bool isDistinctMultiplanningEnabled =
        feature_flags::gFeatureFlagShardFilteringDistinctScan
            .isEnabledUseLastLTSFCVWhenUninitialized(fcvSnapshot);
    if (isDistinctMultiplanningEnabled) {
        if (!isIndexSuitableForDistinct(canonicalQuery,
                                        indexScanNode->index,
                                        field,
                                        flipDistinctScanDirection,
                                        strictDistinctOnly)) {
            return false;
        }
        if (canonicalQuery.getFindCommandRequest().getFilter().isEmpty() &&
            !canonicalQuery.getSortPattern() &&
            isAFullIndexScanPreferable(indexScanNode->index, field, indexScanNode->queryCollator)) {
            return false;
        }
    }

    // If the fetch has a filter, we're out of luck. We can't skip all keys with a given value,
    // since one of them may key a document that passes the filter.
    if (fetchNode && fetchNode->filter) {
        return false;
    }

    if (indexScanNode->index.type == IndexType::INDEX_WILDCARD) {
        // If the query is on a field other than the distinct key, we may have generated a $** plan
        // which does not actually contain the distinct key field.
        if (field != std::next(indexScanNode->index.keyPattern.begin())->fieldName()) {
            return false;
        }
        // If the query includes object bounds, we cannot turn this IXSCAN into a DISTINCT_SCAN.
        // Wildcard indexes contain multiple keys per object, one for each subpath in ascending
        // (Path, Value, RecordId) order. If the distinct fields in two successive documents are
        // objects with the same leaf path values but in different field order, e.g. {a: 1, b: 2}
        // and {b: 2, a: 1}, we would therefore only return the first document and skip the other.
        if (wildcard_planning::isWildcardObjectSubpathScan(indexScanNode)) {
            return false;
        }
    }

    // An additional filter must be applied to the data in the key, so we can't just skip
    // all the keys with a given value; we must examine every one to find the one that (may)
    // pass the filter.
    if (indexScanNode->filter) {
        return false;
    }

    // We only set this when we have special query modifiers (.max() or .min()) or other
    // special cases.  Don't want to handle the interactions between those and distinct.
    // Don't think this will ever really be true but if it somehow is, just ignore this
    // soln.
    if (indexScanNode->bounds.isSimpleRange) {
        return false;
    }

    // Figure out which field we're skipping to the next value of.
    int fieldNo = 0;
    BSONObjIterator it(indexScanNode->index.keyPattern);
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
            invariant(i < indexScanNode->bounds.size());
            if (indexScanNode->bounds.fields[i].intervals.size() != 1 ||
                !indexScanNode->bounds.fields[i].intervals[0].isPoint()) {
                return false;
            }
        }
    }

    // We should not use a distinct scan if the field over which we are computing the distinct is
    // multikey.
    if (indexScanNode->index.multikey) {
        const auto& multikeyPaths = indexScanNode->index.multikeyPaths;
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

    // Make a new DistinctNode. We will swap this for the ixscan in the provided solution.
    auto distinctNode = std::make_unique<DistinctNode>(indexScanNode->index);
    distinctNode->direction =
        flipDistinctScanDirection ? -indexScanNode->direction : indexScanNode->direction;
    distinctNode->bounds =
        flipDistinctScanDirection ? indexScanNode->bounds.reverse() : indexScanNode->bounds;
    distinctNode->queryCollator = indexScanNode->queryCollator;
    distinctNode->fieldNo = fieldNo;

    if (fetchNode) {
        // If the original plan had PROJECT and FETCH stages, we can get rid of the PROJECT
        // transforming the plan from PROJECT=>FETCH=>IXSCAN to FETCH=>DISTINCT_SCAN.
        if (projectNode) {
            invariant(projectNode == root);
            invariant(fetchNode == root->children[0].get());
            invariant(STAGE_FETCH == root->children[0]->getType());
            invariant(STAGE_IXSCAN == root->children[0]->children[0]->getType());
            // Make the fetch the new root. This destroys the project stage.
            soln->setRoot(std::move(root->children[0]));
        }

        // Attach the distinct node in the index scan's place.
        fetchNode->children[0] = std::move(distinctNode);
    } else {
        // There is no fetch node. The PROJECT=>IXSCAN tree should become PROJECT=>DISTINCT_SCAN.
        invariant(projectNode == root);
        invariant(STAGE_IXSCAN == root->children[0]->getType());

        // Attach the distinct node in the index scan's place.
        root->children[0] = std::move(distinctNode);
    }

    return true;
}

}  // namespace mongo
