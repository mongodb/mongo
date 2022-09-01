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


#include "mongo/db/pipeline/dependencies.h"
#include "mongo/platform/basic.h"

#include <boost/optional.hpp>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/catalog/clustered_collection_util.h"
#include "mongo/db/exec/bucket_unpacker.h"
#include "mongo/db/exec/projection_executor_utils.h"
#include "mongo/db/index/wildcard_key_generator.h"
#include "mongo/db/index_names.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_text.h"
#include "mongo/db/matcher/match_expression_dependencies.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/classic_plan_cache.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/index_entry.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/plan_enumerator.h"
#include "mongo/db/query/planner_access.h"
#include "mongo/db/query/planner_analysis.h"
#include "mongo/db/query/planner_ixselect.h"
#include "mongo/db/query/projection_parser.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/util/set_util.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util_core.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {
namespace log_detail {
void logSubplannerIndexEntry(const IndexEntry& entry, size_t childIndex) {
    LOGV2_DEBUG(20598,
                5,
                "Subplanner: index number and entry",
                "indexNumber"_attr = childIndex,
                "indexEntry"_attr = entry);
}

void logCachedPlanFound(size_t numChildren, size_t childIndex) {
    LOGV2_DEBUG(20599,
                5,
                "Subplanner: cached plan found",
                "childIndex"_attr = childIndex,
                "numChildren"_attr = numChildren);
}

void logCachedPlanNotFound(size_t numChildren, size_t childIndex) {
    LOGV2_DEBUG(20600,
                5,
                "Subplanner: planning child",
                "childIndex"_attr = childIndex,
                "numChildren"_attr = numChildren);
}

void logNumberOfSolutions(size_t numSolutions) {
    LOGV2_DEBUG(20601, 5, "Subplanner: number of solutions", "numSolutions"_attr = numSolutions);
}
}  // namespace log_detail

namespace {
/**
 * On success, applies the index tags from 'branchCacheData' (which represent the winning
 * plan for 'orChild') to 'compositeCacheData'.
 */
Status tagOrChildAccordingToCache(PlanCacheIndexTree* compositeCacheData,
                                  SolutionCacheData* branchCacheData,
                                  MatchExpression* orChild,
                                  const std::map<IndexEntry::Identifier, size_t>& indexMap) {
    invariant(compositeCacheData);

    // We want a well-formed *indexed* solution.
    if (nullptr == branchCacheData) {
        // For example, we don't cache things for 2d indices.
        str::stream ss;
        ss << "No cache data for subchild " << orChild->debugString();
        return Status(ErrorCodes::NoQueryExecutionPlans, ss);
    }

    if (SolutionCacheData::USE_INDEX_TAGS_SOLN != branchCacheData->solnType) {
        str::stream ss;
        ss << "No indexed cache data for subchild " << orChild->debugString();
        return Status(ErrorCodes::NoQueryExecutionPlans, ss);
    }

    // Add the index assignments to our original query.
    Status tagStatus =
        QueryPlanner::tagAccordingToCache(orChild, branchCacheData->tree.get(), indexMap);

    if (!tagStatus.isOK()) {
        str::stream ss;
        ss << "Failed to extract indices from subchild " << orChild->debugString();
        return tagStatus.withContext(ss);
    }

    // Add the child's cache data to the cache data we're creating for the main query.
    compositeCacheData->children.push_back(branchCacheData->tree->clone());

    return Status::OK();
}

/**
 * Returns whether the hint matches the given index. When hinting by index name, 'hintObj' takes the
 * shape of {$hint: <indexName>}. When hinting by key pattern, 'hintObj' represents the actual key
 * pattern (eg: {_id: 1}).
 */
bool hintMatchesNameOrPattern(const BSONObj& hintObj,
                              StringData indexName,
                              BSONObj indexKeyPattern) {

    BSONElement firstHintElt = hintObj.firstElement();
    if (firstHintElt.fieldNameStringData() == "$hint"_sd &&
        firstHintElt.type() == BSONType::String) {
        // An index name is provided by the hint.
        return indexName == firstHintElt.valueStringData();
    }

    // An index spec is provided by the hint.
    return hintObj.woCompare(indexKeyPattern) == 0;
}

/**
 * Returns whether the hintedIndex matches the cluster key.
 */
bool hintMatchesClusterKey(const boost::optional<ClusteredCollectionInfo>& clusteredInfo,
                           const BSONObj& hintObj) {
    if (!clusteredInfo) {
        // The collection isn't clustered.
        return false;
    }

    auto clusteredIndexSpec = clusteredInfo->getIndexSpec();

    // The clusteredIndex's name should always be filled in with a default value when not
    // specified upon creation.
    tassert(6012100,
            "clusteredIndex's 'ne' field should be filled in by default after creation",
            clusteredIndexSpec.getName());
    return hintMatchesNameOrPattern(
        hintObj, clusteredIndexSpec.getName().value(), clusteredIndexSpec.getKey());
}

/**
 * Returns whether the hintedIndex matches the columnstore index.
 */
bool hintMatchesColumnStoreIndex(const BSONObj& hintObj, const ColumnIndexEntry& columnStoreIndex) {
    // TODO SERVER-68400: Should be possible to have some other keypattern.
    return hintMatchesNameOrPattern(
        hintObj, columnStoreIndex.identifier.catalogName, columnStoreIndex.keyPattern);
}

/**
 * Returns the dependencies for the CanoncialQuery, split by those needed to answer the filter,
 * and those needed for "everything else" which is the project and sort.
 */
std::pair<DepsTracker, DepsTracker> computeDeps(const QueryPlannerParams& params,
                                                const CanonicalQuery& query) {
    DepsTracker filterDeps;
    match_expression::addDependencies(query.root(), &filterDeps);
    DepsTracker outputDeps;
    if (!query.getProj() || query.getProj()->requiresDocument()) {
        outputDeps.needWholeDocument = true;
        return {std::move(filterDeps), std::move(outputDeps)};
    }
    outputDeps.fields = query.getProj()->getRequiredFields();
    if (auto sortPattern = query.getSortPattern()) {
        sortPattern->addDependencies(&outputDeps);
    }
    if (params.options & QueryPlannerParams::INCLUDE_SHARD_FILTER) {
        for (auto&& field : params.shardKey) {
            outputDeps.fields.emplace(field.fieldNameStringData());
        }
    }
    // There's no known way a sort would depend on the whole document, and we already verified
    // that the projection doesn't depend on the whole document.
    tassert(6430503, "Unexpectedly required entire object", !outputDeps.needWholeDocument);
    return {std::move(filterDeps), std::move(outputDeps)};
}

Status computeColumnScanIsPossibleStatus(const CanonicalQuery& query,
                                         const QueryPlannerParams& params) {
    if (params.columnStoreIndexes.empty()) {
        return {ErrorCodes::InvalidOptions, "No columnstore indexes available"};
    }
    if (!query.isSbeCompatible()) {
        return {ErrorCodes::NotImplemented,
                "A columnstore index can only be used with queries in the SBE engine. The given "
                "query is not eligible for this engine (yet)"};
    }
    if (query.getForceClassicEngine()) {
        return {ErrorCodes::InvalidOptions,
                "A columnstore index can only be used with queries in the SBE engine, but the "
                "query specified to force the classic engine"};
    }
    return Status::OK();
}

bool columnScanIsPossible(const CanonicalQuery& query, const QueryPlannerParams& params) {
    return computeColumnScanIsPossibleStatus(query, params).isOK();
}

std::unique_ptr<QuerySolution> makeColumnScanPlan(
    const CanonicalQuery& query,
    const QueryPlannerParams& params,
    const ColumnIndexEntry& columnStoreIndex,
    DepsTracker filterDeps,
    DepsTracker outputDeps,
    OrderedPathSet allFieldsReferenced,
    StringMap<std::unique_ptr<MatchExpression>> filterSplitByColumn,
    std::unique_ptr<MatchExpression> residualPredicate) {
    dassert(columnScanIsPossible(query, params));

    return QueryPlannerAnalysis::analyzeDataAccess(
        query,
        params,
        std::make_unique<ColumnIndexScanNode>(columnStoreIndex,
                                              std::move(outputDeps.fields),
                                              std::move(filterDeps.fields),
                                              std::move(allFieldsReferenced),
                                              std::move(filterSplitByColumn),
                                              std::move(residualPredicate)));
}

/**
 * A helper function which applies a heuristic to determine if a COLUMN_SCAN plan would examine few
 * enough fields to be considered faster than a COLLSCAN.
 */
Status checkColumnScanFieldLimits(
    size_t nReferencedFields,
    const StringMap<std::unique_ptr<MatchExpression>>& filterSplitByColumn) {

    const int maxNumFields = filterSplitByColumn.size() > 0
        ? internalQueryMaxNumberOfFieldsToChooseFilteredColumnScan.load()
        : internalQueryMaxNumberOfFieldsToChooseUnfilteredColumnScan.load();
    if (static_cast<int>(nReferencedFields) > maxNumFields) {
        return Status{ErrorCodes::Error{6430508},
                      str::stream() << "referenced too many fields. nReferenced="
                                    << nReferencedFields << ", limit=" << maxNumFields};
    }
    return Status::OK();
}

bool checkProjectionCoversQuery(OrderedPathSet& fields, const ColumnIndexEntry& columnStoreIndex) {
    const auto projectedFields = projection_executor_utils::applyProjectionToFields(
        columnStoreIndex.indexPathProjection->exec(), fields);
    // If the number of fields is equal to the number of fields preserved, then the projection
    // covers the query.
    return projectedFields.size() == fields.size();
}

/**
 * A helper function that returns the number of column store indexes that cover the query,
 * as well as an arbitary, valid column store index for the column scan.
 */
std::pair<int, const ColumnIndexEntry*> getValidColumnIndex(
    OrderedPathSet& fields, const std::vector<ColumnIndexEntry>& columnStoreIndexes) {
    const ColumnIndexEntry* chosenIndex;
    int numValid = 0;
    for (const auto& columnStoreIndex : columnStoreIndexes) {
        if (checkProjectionCoversQuery(fields, columnStoreIndex)) {
            chosenIndex = numValid == 0 ? &columnStoreIndex : chosenIndex;
            ++numValid;
        }
    }
    return {numValid, chosenIndex};
}

/**
 * Attempts to build a plan using a column store index. Returns a non-OK status if it can't build
 * one with the code and message indicating the problem - or a QuerySolution if it can.
 */
StatusWith<std::unique_ptr<QuerySolution>> tryToBuildColumnScan(
    const QueryPlannerParams& params,
    const CanonicalQuery& query,
    const boost::optional<ColumnIndexEntry>& hintedIndex = boost::none) {
    if (auto status = computeColumnScanIsPossibleStatus(query, params); !status.isOK()) {
        return status;
    }

    invariant(params.columnStoreIndexes.size() >= 1);

    auto [filterDeps, outputDeps] = computeDeps(params, query);
    auto allFieldsReferenced = set_util::setUnion(filterDeps.fields, outputDeps.fields);
    if (filterDeps.needWholeDocument || outputDeps.needWholeDocument) {
        // TODO SERVER-66284 Would like to enable a plan when hinted, even if we need the whole
        // document. Something like COLUMN_SCAN -> FETCH.
        return {ErrorCodes::Error{6298501},
                "cannot use column store index because the query requires seeing the entire "
                "document"};
    } else if (!hintedIndex && expression::containsOverlappingPaths(allFieldsReferenced)) {
        // The query needs a path and a parent or ancestor path. For example, the query needs to
        // access both "a" and "a.b". This is a heuristic, but generally we would not expect this to
        // benefit from the column store index. This kind of dependency pattern is probably an
        // indication that the parent/ancestor path will be an object or array of objects, which
        // will require us to fall back to the rowstore and remove any benefit of using the index.
        return {ErrorCodes::Error{6726400},
                str::stream() << "cannot use columnstore index because the query requires paths "
                                 "which are a prefix of each other: "
                              << set_util::setToString(allFieldsReferenced)};
    }

    // Ensures that hinted index is eligible for the column scan.
    if (hintedIndex && !checkProjectionCoversQuery(allFieldsReferenced, *hintedIndex)) {
        return {ErrorCodes::Error{6714002},
                "the hinted column store index cannot be used because it does not cover the query"};
    }

    // Check that union of the dependency fields can be successfully projected by at least one
    // column store index.
    auto [numValid, selectedColumnStoreIndex] =
        getValidColumnIndex(allFieldsReferenced, params.columnStoreIndexes);

    // If not columnar index can support the projection, we will not use column scan.
    if (numValid == 0) {
        return {ErrorCodes::Error{6714001},
                "cannot use column store index because there exists no column store index for this "
                "collection that covers the query"};
    }
    invariant(selectedColumnStoreIndex);

    if (!hintedIndex && numValid > 1) {
        LOGV2_DEBUG(6298500,
                    2,
                    "Multiple column store indexes present. Selecting the first "
                    "one arbitrarily",
                    "indexName"_attr = selectedColumnStoreIndex->identifier.catalogName);
    }

    const auto& columnStoreIndex = hintedIndex.value_or(*selectedColumnStoreIndex);
    std::unique_ptr<MatchExpression> residualPredicate;
    StringMap<std::unique_ptr<MatchExpression>> filterSplitByColumn;
    std::tie(filterSplitByColumn, residualPredicate) =
        expression::splitMatchExpressionForColumns(query.root());
    auto fieldLimitStatus =
        checkColumnScanFieldLimits(allFieldsReferenced.size(), filterSplitByColumn);

    if (fieldLimitStatus.isOK() || hintedIndex) {
        // We have a hint, or few enough dependencies that we suspect a column scan is still
        // better than a collection scan. Build it and return it.
        return makeColumnScanPlan(query,
                                  params,
                                  columnStoreIndex,
                                  std::move(filterDeps),
                                  std::move(outputDeps),
                                  std::move(allFieldsReferenced),
                                  std::move(filterSplitByColumn),
                                  std::move(residualPredicate));
    }
    return Status{ErrorCodes::Error{6298502}, "columnstore index is not applicable for this query"};
}
}  // namespace

using std::numeric_limits;
using std::unique_ptr;

namespace dps = ::mongo::dotted_path_support;
// Copied verbatim from db/index.h
static bool isIdIndex(const BSONObj& pattern) {
    BSONObjIterator i(pattern);
    BSONElement e = i.next();
    //_id index must have form exactly {_id : 1} or {_id : -1}.
    // Allows an index of form {_id : "hashed"} to exist but
    // do not consider it to be the primary _id index
    if (!(strcmp(e.fieldName(), "_id") == 0 && (e.numberInt() == 1 || e.numberInt() == -1)))
        return false;
    return i.next().eoo();
}

static bool is2DIndex(const BSONObj& pattern) {
    BSONObjIterator it(pattern);
    while (it.more()) {
        BSONElement e = it.next();
        if (String == e.type() && (e.valueStringData() == "2d")) {
            return true;
        }
    }
    return false;
}

string optionString(size_t options) {
    str::stream ss;

    if (QueryPlannerParams::DEFAULT == options) {
        ss << "DEFAULT ";
    }
    while (options) {
        // The expression (x & (x - 1)) yields x with the lowest bit cleared.  Then the
        // exclusive-or of the result with the original yields the lowest bit by itself.
        size_t new_options = options & (options - 1);
        QueryPlannerParams::Options opt = QueryPlannerParams::Options(new_options ^ options);
        options = new_options;
        switch (opt) {
            case QueryPlannerParams::NO_TABLE_SCAN:
                ss << "NO_TABLE_SCAN ";
                break;
            case QueryPlannerParams::INCLUDE_COLLSCAN:
                ss << "INCLUDE_COLLSCAN ";
                break;
            case QueryPlannerParams::INCLUDE_SHARD_FILTER:
                ss << "INCLUDE_SHARD_FILTER ";
                break;
            case QueryPlannerParams::INDEX_INTERSECTION:
                ss << "INDEX_INTERSECTION ";
                break;
            case QueryPlannerParams::IS_COUNT:
                ss << "IS_COUNT ";
                break;
            case QueryPlannerParams::GENERATE_COVERED_IXSCANS:
                ss << "GENERATE_COVERED_IXSCANS ";
                break;
            case QueryPlannerParams::TRACK_LATEST_OPLOG_TS:
                ss << "TRACK_LATEST_OPLOG_TS ";
                break;
            case QueryPlannerParams::OPLOG_SCAN_WAIT_FOR_VISIBLE:
                ss << "OPLOG_SCAN_WAIT_FOR_VISIBLE ";
                break;
            case QueryPlannerParams::STRICT_DISTINCT_ONLY:
                ss << "STRICT_DISTINCT_ONLY ";
                break;
            case QueryPlannerParams::PRESERVE_RECORD_ID:
                ss << "PRESERVE_RECORD_ID ";
                break;
            case QueryPlannerParams::ASSERT_MIN_TS_HAS_NOT_FALLEN_OFF_OPLOG:
                ss << "ASSERT_MIN_TS_HAS_NOT_FALLEN_OFF_OPLOG ";
                break;
            case QueryPlannerParams::ENUMERATE_OR_CHILDREN_LOCKSTEP:
                ss << "ENUMERATE_OR_CHILDREN_LOCKSTEP ";
                break;
            case QueryPlannerParams::RETURN_OWNED_DATA:
                ss << "RETURN_OWNED_DATA ";
                break;
            case QueryPlannerParams::GENERATE_PER_COLUMN_FILTERS:
                ss << "GENERATE_PER_COLUMN_FILTERS ";
                break;
            case QueryPlannerParams::DEFAULT:
                MONGO_UNREACHABLE;
                break;
        }
    }

    return ss;
}

static BSONObj getKeyFromQuery(const BSONObj& keyPattern, const BSONObj& query) {
    return query.extractFieldsUndotted(keyPattern);
}

static bool indexCompatibleMaxMin(const BSONObj& obj,
                                  const CollatorInterface* queryCollator,
                                  const CollatorInterface* indexCollator,
                                  const BSONObj& keyPattern) {
    BSONObjIterator kpIt(keyPattern);
    BSONObjIterator objIt(obj);

    const bool collatorsMatch = CollatorInterface::collatorsMatch(queryCollator, indexCollator);

    for (;;) {
        // Every element up to this point has matched so the KP matches
        if (!kpIt.more() && !objIt.more()) {
            return true;
        }

        // If only one iterator is done, it's not a match.
        if (!kpIt.more() || !objIt.more()) {
            return false;
        }

        // Field names must match and be in the same order.
        BSONElement kpElt = kpIt.next();
        BSONElement objElt = objIt.next();
        if (kpElt.fieldNameStringData() != objElt.fieldNameStringData()) {
            return false;
        }

        // If the index collation doesn't match the query collation, and the min/max obj has a
        // boundary value that needs to respect the collation, then the index is not compatible.
        if (!collatorsMatch && CollationIndexKey::isCollatableType(objElt.type())) {
            return false;
        }
    }
}

static bool indexCompatibleMaxMin(const BSONObj& obj,
                                  const CollatorInterface* queryCollator,
                                  const IndexEntry& indexEntry) {
    // Wildcard indexes should have been filtered out by the time this is called.
    if (indexEntry.type == IndexType::INDEX_WILDCARD) {
        return false;
    }
    return indexCompatibleMaxMin(obj, queryCollator, indexEntry.collator, indexEntry.keyPattern);
}

static BSONObj stripFieldNamesAndApplyCollation(const BSONObj& obj,
                                                const CollatorInterface* collator) {
    BSONObjBuilder bob;
    for (BSONElement elt : obj) {
        CollationIndexKey::collationAwareIndexKeyAppend(elt, collator, &bob);
    }
    return bob.obj();
}

/**
 * "Finishes" the min object for the $min query option by filling in an empty object with
 * MinKey/MaxKey and stripping field names. Also translates keys according to the collation, if
 * necessary.
 *
 * In the case that 'minObj' is empty, we "finish" it by filling in either MinKey or MaxKey
 * instead. Choosing whether to use MinKey or MaxKey is done by comparing against 'maxObj'.
 * For instance, suppose 'minObj' is empty, 'maxObj' is { a: 3 }, and the key pattern is
 * { a: -1 }. According to the key pattern ordering, { a: 3 } < MinKey. This means that the
 * proper resulting bounds are
 *
 *   start: { '': MaxKey }, end: { '': 3 }
 *
 * as opposed to
 *
 *   start: { '': MinKey }, end: { '': 3 }
 *
 * Suppose instead that the key pattern is { a: 1 }, with the same 'minObj' and 'maxObj'
 * (that is, an empty object and { a: 3 } respectively). In this case, { a: 3 } > MinKey,
 * which means that we use range [{'': MinKey}, {'': 3}]. The proper 'minObj' in this case is
 * MinKey, whereas in the previous example it was MaxKey.
 *
 * If 'minObj' is non-empty, then all we do is strip its field names (because index keys always
 * have empty field names).
 */
static BSONObj finishMinObj(const IndexEntry& indexEntry,
                            const BSONObj& minObj,
                            const BSONObj& maxObj) {
    if (minObj.isEmpty()) {
        BSONObjBuilder ret;
        for (auto key : indexEntry.keyPattern) {
            if (!key.isNumber() || key.numberInt() > 0) {
                ret.appendMinKey("");
            } else {
                ret.appendMaxKey("");
            }
        }
        return ret.obj();
    } else {
        return stripFieldNamesAndApplyCollation(minObj, indexEntry.collator);
    }
}

/**
 * "Finishes" the max object for the $max query option by filling in an empty object with
 * MinKey/MaxKey and stripping field names. Also translates keys according to the collation, if
 * necessary.
 *
 * See comment for finishMinObj() for why we need both 'minObj' and 'maxObj'.
 */
static BSONObj finishMaxObj(const IndexEntry& indexEntry,
                            const BSONObj& minObj,
                            const BSONObj& maxObj) {
    if (maxObj.isEmpty()) {
        BSONObjBuilder ret;
        for (auto key : indexEntry.keyPattern) {
            if (!key.isNumber() || key.numberInt() > 0) {
                ret.appendMaxKey("");
            } else {
                ret.appendMinKey("");
            }
        }
        return ret.obj();
    } else {
        return stripFieldNamesAndApplyCollation(maxObj, indexEntry.collator);
    }
}

std::unique_ptr<QuerySolution> buildCollscanSoln(const CanonicalQuery& query,
                                                 bool tailable,
                                                 const QueryPlannerParams& params,
                                                 int direction = 1) {
    std::unique_ptr<QuerySolutionNode> solnRoot(
        QueryPlannerAccess::makeCollectionScan(query, tailable, params, direction));
    return QueryPlannerAnalysis::analyzeDataAccess(query, params, std::move(solnRoot));
}

std::unique_ptr<QuerySolution> buildWholeIXSoln(
    const IndexEntry& index,
    const CanonicalQuery& query,
    const QueryPlannerParams& params,
    const boost::optional<int>& direction = boost::none) {
    tassert(6499400,
            "Cannot pass both an explicit direction and a traversal preference",
            !(direction.has_value() && params.traversalPreference));
    std::unique_ptr<QuerySolutionNode> solnRoot(
        QueryPlannerAccess::scanWholeIndex(index, query, params, direction.value_or(1)));
    return QueryPlannerAnalysis::analyzeDataAccess(query, params, std::move(solnRoot));
}

bool providesSort(const CanonicalQuery& query, const BSONObj& kp) {
    return query.getFindCommandRequest().getSort().isPrefixOf(
        kp, SimpleBSONElementComparator::kInstance);
}

StatusWith<std::unique_ptr<PlanCacheIndexTree>> QueryPlanner::cacheDataFromTaggedTree(
    const MatchExpression* const taggedTree, const vector<IndexEntry>& relevantIndices) {
    if (!taggedTree) {
        return Status(ErrorCodes::BadValue, "Cannot produce cache data: tree is NULL.");
    }

    auto indexTree = std::make_unique<PlanCacheIndexTree>();

    if (taggedTree->getTag() &&
        taggedTree->getTag()->getType() == MatchExpression::TagData::Type::IndexTag) {
        IndexTag* itag = static_cast<IndexTag*>(taggedTree->getTag());
        if (itag->index >= relevantIndices.size()) {
            str::stream ss;
            ss << "Index number is " << itag->index << " but there are only "
               << relevantIndices.size() << " relevant indices.";
            return Status(ErrorCodes::BadValue, ss);
        }

        // Make sure not to cache solutions which use '2d' indices.
        // A 2d index that doesn't wrap on one query may wrap on another, so we have to
        // check that the index is OK with the predicate. The only thing we have to do
        // this for is 2d.  For now it's easier to move ahead if we don't cache 2d.
        //
        // TODO: revisit with a post-cached-index-assignment compatibility check
        if (is2DIndex(relevantIndices[itag->index].keyPattern)) {
            return Status(ErrorCodes::BadValue, "can't cache '2d' index");
        }

        IndexEntry* ientry = new IndexEntry(relevantIndices[itag->index]);
        indexTree->entry.reset(ientry);
        indexTree->index_pos = itag->pos;
        indexTree->canCombineBounds = itag->canCombineBounds;
    } else if (taggedTree->getTag() &&
               taggedTree->getTag()->getType() == MatchExpression::TagData::Type::OrPushdownTag) {
        OrPushdownTag* orPushdownTag = static_cast<OrPushdownTag*>(taggedTree->getTag());

        if (orPushdownTag->getIndexTag()) {
            const IndexTag* itag = static_cast<const IndexTag*>(orPushdownTag->getIndexTag());

            if (is2DIndex(relevantIndices[itag->index].keyPattern)) {
                return Status(ErrorCodes::BadValue, "can't cache '2d' index");
            }

            std::unique_ptr<IndexEntry> indexEntry =
                std::make_unique<IndexEntry>(relevantIndices[itag->index]);
            indexTree->entry = std::move(indexEntry);
            indexTree->index_pos = itag->pos;
            indexTree->canCombineBounds = itag->canCombineBounds;
        }

        for (const auto& dest : orPushdownTag->getDestinations()) {
            IndexTag* indexTag = static_cast<IndexTag*>(dest.tagData.get());
            PlanCacheIndexTree::OrPushdown orPushdown{relevantIndices[indexTag->index].identifier,
                                                      indexTag->pos,
                                                      indexTag->canCombineBounds,
                                                      dest.route};
            indexTree->orPushdowns.push_back(std::move(orPushdown));
        }
    }

    for (size_t i = 0; i < taggedTree->numChildren(); ++i) {
        MatchExpression* taggedChild = taggedTree->getChild(i);
        auto statusWithTree = cacheDataFromTaggedTree(taggedChild, relevantIndices);
        if (!statusWithTree.isOK()) {
            return statusWithTree.getStatus();
        }
        indexTree->children.push_back(std::move(statusWithTree.getValue()));
    }

    return {std::move(indexTree)};
}

// static
Status QueryPlanner::tagAccordingToCache(MatchExpression* filter,
                                         const PlanCacheIndexTree* const indexTree,
                                         const map<IndexEntry::Identifier, size_t>& indexMap) {
    if (nullptr == filter) {
        return Status(ErrorCodes::NoQueryExecutionPlans, "Cannot tag tree: filter is NULL.");
    }
    if (nullptr == indexTree) {
        return Status(ErrorCodes::NoQueryExecutionPlans, "Cannot tag tree: indexTree is NULL.");
    }

    // We're tagging the tree here, so it shouldn't have
    // any tags hanging off yet.
    verify(nullptr == filter->getTag());

    if (filter->numChildren() != indexTree->children.size()) {
        str::stream ss;
        ss << "Cache topology and query did not match: "
           << "query has " << filter->numChildren() << " children "
           << "and cache has " << indexTree->children.size() << " children.";
        return Status(ErrorCodes::NoQueryExecutionPlans, ss);
    }

    // Continue the depth-first tree traversal.
    for (size_t i = 0; i < filter->numChildren(); ++i) {
        Status s = tagAccordingToCache(filter->getChild(i), indexTree->children[i].get(), indexMap);
        if (!s.isOK()) {
            return s;
        }
    }

    if (!indexTree->orPushdowns.empty()) {
        filter->setTag(new OrPushdownTag());
        OrPushdownTag* orPushdownTag = static_cast<OrPushdownTag*>(filter->getTag());
        for (const auto& orPushdown : indexTree->orPushdowns) {
            auto index = indexMap.find(orPushdown.indexEntryId);
            if (index == indexMap.end()) {
                return Status(ErrorCodes::NoQueryExecutionPlans,
                              str::stream() << "Did not find index: " << orPushdown.indexEntryId);
            }
            OrPushdownTag::Destination dest;
            dest.route = orPushdown.route;
            dest.tagData = std::make_unique<IndexTag>(
                index->second, orPushdown.position, orPushdown.canCombineBounds);
            orPushdownTag->addDestination(std::move(dest));
        }
    }

    if (indexTree->entry.get()) {
        const auto got = indexMap.find(indexTree->entry->identifier);
        if (got == indexMap.end()) {
            str::stream ss;
            ss << "Did not find index with name: " << indexTree->entry->identifier.catalogName;
            return Status(ErrorCodes::NoQueryExecutionPlans, ss);
        }
        if (filter->getTag()) {
            OrPushdownTag* orPushdownTag = static_cast<OrPushdownTag*>(filter->getTag());
            orPushdownTag->setIndexTag(
                new IndexTag(got->second, indexTree->index_pos, indexTree->canCombineBounds));
        } else {
            filter->setTag(
                new IndexTag(got->second, indexTree->index_pos, indexTree->canCombineBounds));
        }
    }

    return Status::OK();
}

StatusWith<std::unique_ptr<QuerySolution>> QueryPlanner::planFromCache(
    const CanonicalQuery& query,
    const QueryPlannerParams& params,
    const CachedSolution& cachedSoln) {
    invariant(cachedSoln.cachedPlan);

    // A query not suitable for caching should not have made its way into the cache.
    invariant(shouldCacheQuery(query));

    // Look up winning solution in cached solution's array.
    const auto& winnerCacheData = *cachedSoln.cachedPlan;

    if (SolutionCacheData::WHOLE_IXSCAN_SOLN == winnerCacheData.solnType) {
        // The solution can be constructed by a scan over the entire index.
        auto soln = buildWholeIXSoln(
            *winnerCacheData.tree->entry, query, params, winnerCacheData.wholeIXSolnDir);
        if (!soln) {
            return Status(ErrorCodes::NoQueryExecutionPlans,
                          "plan cache error: soln that uses index to provide sort");
        } else {
            return {std::move(soln)};
        }
    } else if (SolutionCacheData::COLLSCAN_SOLN == winnerCacheData.solnType) {
        // The cached solution is a collection scan. We don't cache collscans
        // with tailable==true, hence the false below.
        auto soln = buildCollscanSoln(query, false, params);
        if (!soln) {
            return Status(ErrorCodes::NoQueryExecutionPlans,
                          "plan cache error: collection scan soln");
        } else {
            return {std::move(soln)};
        }
    }

    // SolutionCacheData::USE_TAGS_SOLN == cacheData->solnType
    // If we're here then this is neither the whole index scan or collection scan
    // cases, and we proceed by using the PlanCacheIndexTree to tag the query tree.

    // Create a copy of the expression tree.  We use cachedSoln to annotate this with indices.
    unique_ptr<MatchExpression> clone = query.root()->shallowClone();

    LOGV2_DEBUG(20963,
                5,
                "Tagging the match expression according to cache data",
                "filter"_attr = redact(clone->debugString()),
                "cacheData"_attr = redact(winnerCacheData.toString()));

    stdx::unordered_set<string> fields;
    QueryPlannerIXSelect::getFields(query.root(), &fields);
    std::vector<IndexEntry> expandedIndexes =
        QueryPlannerIXSelect::expandIndexes(fields, params.indices);

    // Map from index name to index number.
    map<IndexEntry::Identifier, size_t> indexMap;
    for (size_t i = 0; i < expandedIndexes.size(); ++i) {
        const IndexEntry& ie = expandedIndexes[i];
        const auto insertionRes = indexMap.insert(std::make_pair(ie.identifier, i));
        // Be sure the key was not already in the map.
        invariant(insertionRes.second);
        LOGV2_DEBUG(20964,
                    5,
                    "Index mapping: number and identifier",
                    "indexNumber"_attr = i,
                    "id"_attr = ie.identifier);
    }

    Status s = tagAccordingToCache(clone.get(), winnerCacheData.tree.get(), indexMap);
    if (!s.isOK()) {
        return s;
    }

    // The MatchExpression tree is in canonical order. We must order the nodes for access
    // planning.
    prepareForAccessPlanning(clone.get());

    LOGV2_DEBUG(20965, 5, "Tagged tree", "tree"_attr = redact(clone->debugString()));

    // Use the cached index assignments to build solnRoot.
    std::unique_ptr<QuerySolutionNode> solnRoot(QueryPlannerAccess::buildIndexedDataAccess(
        query, std::move(clone), expandedIndexes, params));

    if (!solnRoot) {
        return Status(ErrorCodes::NoQueryExecutionPlans,
                      str::stream() << "Failed to create data access plan from cache. Query: "
                                    << query.toStringShort());
    }

    auto soln = QueryPlannerAnalysis::analyzeDataAccess(query, params, std::move(solnRoot));
    if (!soln) {
        return Status(ErrorCodes::NoQueryExecutionPlans,
                      str::stream()
                          << "Failed to analyze plan from cache. Query: " << query.toStringShort());
    }

    LOGV2_DEBUG(20966,
                5,
                "Planner: solution constructed from the cache",
                "solution"_attr = redact(soln->toString()));
    return {std::move(soln)};
}

/**
 * For some reason this type is hard to construct inline and keep the compiler happy. Convenience
 * helper to do so since we do it a couple times.
 */
StatusWith<std::vector<std::unique_ptr<QuerySolution>>> singleSolution(
    std::unique_ptr<QuerySolution> soln) {
    std::vector<std::unique_ptr<QuerySolution>> out;
    out.push_back(std::move(soln));
    return {std::move(out)};
}

bool canTableScan(const QueryPlannerParams& params) {
    return !(params.options & QueryPlannerParams::NO_TABLE_SCAN);
}

StatusWith<std::vector<std::unique_ptr<QuerySolution>>> attemptCollectionScan(
    const CanonicalQuery& query, bool isTailable, const QueryPlannerParams& params) {
    if (!canTableScan(params)) {
        return Status(ErrorCodes::NoQueryExecutionPlans,
                      "not allowed to output a collection scan because 'notablescan' is enabled");
    }
    if (auto soln = buildCollscanSoln(query, isTailable, params)) {
        return singleSolution(std::move(soln));
    }
    return Status(ErrorCodes::NoQueryExecutionPlans, "Failed to build collection scan soln");
}

StatusWith<std::vector<std::unique_ptr<QuerySolution>>> handleNaturalHint(
    const CanonicalQuery& query,
    const QueryPlannerParams& params,
    BSONElement naturalHint,
    bool isTailable) {
    // The hint can be {$natural: +/-1}. If this happens, output a collscan. We expect any
    // $natural sort to have been normalized to a $natural hint upstream. Additionally, if
    // the hint matches the collection's cluster key, we also output a collscan utilizing
    // the cluster key.

    // Perform validation specific to $natural.
    LOGV2_DEBUG(20969, 5, "Forcing a table scan due to hinted $natural");
    if (!query.getFindCommandRequest().getMin().isEmpty() ||
        !query.getFindCommandRequest().getMax().isEmpty()) {
        return Status(ErrorCodes::NoQueryExecutionPlans,
                      "min and max are incompatible with $natural");
    }
    auto result = attemptCollectionScan(query, isTailable, params);
    if (result.isOK()) {
        return result;
    }
    return result.getStatus().withContext("could not force a collection scan with a $natural hint");
}

StatusWith<std::vector<std::unique_ptr<QuerySolution>>> handleClusteredScanHint(
    const CanonicalQuery& query, const QueryPlannerParams& params, bool isTailable) {
    // Perform validation specific to hinting on a cluster key.
    BSONObj minObj = query.getFindCommandRequest().getMin();
    BSONObj maxObj = query.getFindCommandRequest().getMax();

    const auto clusterKey = params.clusteredInfo->getIndexSpec().getKey();

    // Check if the query collator is compatible with the collection collator for the
    // provided min and max values.
    if ((!minObj.isEmpty() &&
         !indexCompatibleMaxMin(
             minObj, query.getCollator(), params.clusteredCollectionCollator, clusterKey)) ||
        (!maxObj.isEmpty() &&
         !indexCompatibleMaxMin(
             maxObj, query.getCollator(), params.clusteredCollectionCollator, clusterKey))) {
        return Status(ErrorCodes::Error(6137400),
                      "The clustered index is not compatible with the values provided "
                      "for min/max due to the query collation");
    }

    auto wellSorted = [&minObj, &maxObj, collator = query.getCollator()]() {
        if (collator) {
            auto min = stripFieldNamesAndApplyCollation(minObj, collator);
            auto max = stripFieldNamesAndApplyCollation(maxObj, collator);
            return min.woCompare(max) < 0;
        } else {
            return minObj.woCompare(maxObj) < 0;
        }
    };
    if (!minObj.isEmpty() && !maxObj.isEmpty() && !wellSorted()) {
        return Status(ErrorCodes::Error(6137401), "max() must be greater than min()");
    }
    return attemptCollectionScan(query, isTailable, params);
}


StatusWith<std::vector<std::unique_ptr<QuerySolution>>> QueryPlanner::plan(
    const CanonicalQuery& query, const QueryPlannerParams& params) {
    // It's a little silly to ask for a count and for owned data. This could indicate a bug
    // earlier on.
    tassert(5397500,
            "Count and owned data requested",
            !((params.options & QueryPlannerParams::IS_COUNT) &&
              (params.options & QueryPlannerParams::RETURN_OWNED_DATA)));

    LOGV2_DEBUG(20967,
                5,
                "Beginning planning",
                "options"_attr = optionString(params.options),
                "query"_attr = redact(query.toString()));

    for (size_t i = 0; i < params.indices.size(); ++i) {
        LOGV2_DEBUG(20968,
                    5,
                    "Index number and details",
                    "indexNumber"_attr = i,
                    "index"_attr = params.indices[i].toString());
    }

    const bool isTailable = query.getFindCommandRequest().getTailable();

    // If the query requests a tailable cursor, the only solution is a collscan + filter with
    // tailable set on the collscan.
    if (isTailable) {
        auto collScanResult = attemptCollectionScan(query, isTailable, params);
        if (collScanResult.isOK()) {
            return collScanResult;
        }
        return collScanResult.getStatus().withContext(
            "query is tailable so must do a collection scan");
    }

    // Hints require us to only consider the hinted index. If index filters in the query settings
    // were used to override the allowed indices for planning, we should not use the hinted index
    // requested in the query.
    boost::optional<BSONObj> hintedIndexBson;
    if (!params.indexFiltersApplied) {
        if (auto hintObj = query.getFindCommandRequest().getHint(); !hintObj.isEmpty()) {
            hintedIndexBson = hintObj;
        }
    }

    if (hintedIndexBson) {
        // If we have a hint, check if it matches any "special" index before proceeding.
        const auto& hintObj = *hintedIndexBson;
        if (const auto naturalHint = hintObj[query_request_helper::kNaturalSortField]) {
            return handleNaturalHint(query, params, naturalHint, isTailable);
        } else if (hintMatchesClusterKey(params.clusteredInfo, hintObj)) {
            return handleClusteredScanHint(query, params, isTailable);
        } else {
            for (auto&& columnIndex : params.columnStoreIndexes) {
                if (hintMatchesColumnStoreIndex(hintObj, columnIndex)) {
                    // Hint matches - either build the plan or fail.
                    auto statusWithSoln = tryToBuildColumnScan(params, query, columnIndex);
                    if (!statusWithSoln.isOK()) {
                        return statusWithSoln.getStatus();
                    }
                    return singleSolution(std::move(statusWithSoln.getValue()));
                }
            }
        }
    }

    // Either the list of indices passed in by the caller, or the list of indices filtered
    // according to the hint. This list is later expanded in order to allow the planner to
    // handle wildcard indexes.
    std::vector<IndexEntry> fullIndexList;

    // Will hold a copy of the index entry chosen by the hint.
    boost::optional<IndexEntry> hintedIndexEntry;
    if (!hintedIndexBson) {
        fullIndexList = params.indices;
    } else {
        fullIndexList = QueryPlannerIXSelect::findIndexesByHint(*hintedIndexBson, params.indices);

        if (fullIndexList.empty()) {
            return Status(ErrorCodes::BadValue,
                          "hint provided does not correspond to an existing index");
        }
        if (fullIndexList.size() > 1) {
            return Status(ErrorCodes::IndexNotFound,
                          str::stream()
                              << "Hint matched multiple indexes, "
                              << "must hint by index name. Matched: " << fullIndexList[0].toString()
                              << " and " << fullIndexList[1].toString());
        }

        hintedIndexEntry.emplace(fullIndexList.front());
    }

    // Figure out what fields we care about.
    stdx::unordered_set<string> fields;
    QueryPlannerIXSelect::getFields(query.root(), &fields);
    for (auto&& field : fields) {
        LOGV2_DEBUG(20970, 5, "Predicate over field", "field"_attr = field);
    }

    fullIndexList = QueryPlannerIXSelect::expandIndexes(fields, std::move(fullIndexList));
    std::vector<IndexEntry> relevantIndices;

    if (!hintedIndexEntry) {
        relevantIndices = QueryPlannerIXSelect::findRelevantIndices(fields, fullIndexList);
    } else {
        relevantIndices = fullIndexList;

        // Relevant indices should only ever exceed a size of 1 when there is a hint in the case
        // of
        // $** index.
        if (relevantIndices.size() > 1) {
            for (auto&& entry : relevantIndices) {
                invariant(entry.type == IndexType::INDEX_WILDCARD);
            }
        }
    }

    // Deal with the .min() and .max() query options.  If either exist we can only use an index
    // that matches the object inside.
    if (!query.getFindCommandRequest().getMin().isEmpty() ||
        !query.getFindCommandRequest().getMax().isEmpty()) {

        if (!hintedIndexEntry) {
            return Status(ErrorCodes::Error(51173),
                          "When using min()/max() a hint of which index to use must be provided");
        }

        BSONObj minObj = query.getFindCommandRequest().getMin();
        BSONObj maxObj = query.getFindCommandRequest().getMax();

        if ((!minObj.isEmpty() &&
             !indexCompatibleMaxMin(minObj, query.getCollator(), *hintedIndexEntry)) ||
            (!maxObj.isEmpty() &&
             !indexCompatibleMaxMin(maxObj, query.getCollator(), *hintedIndexEntry))) {
            return Status(ErrorCodes::Error(51174),
                          "The index chosen is not compatible with min/max");
        }
        // Be sure that index expansion didn't do anything. As wildcard indexes are banned for
        // min/max, we expect to find a single hinted index entry.
        invariant(fullIndexList.size() == 1);
        invariant(*hintedIndexEntry == fullIndexList.front());

        // In order to be fully compatible, the min has to be less than the max according to the
        // index key pattern ordering. The first step in verifying this is "finish" the min and
        // max by replacing empty objects and stripping field names.
        BSONObj finishedMinObj = finishMinObj(*hintedIndexEntry, minObj, maxObj);
        BSONObj finishedMaxObj = finishMaxObj(*hintedIndexEntry, minObj, maxObj);

        // Now we have the final min and max. This index is only relevant for the min/max query
        // if min < max.
        if (finishedMinObj.woCompare(finishedMaxObj, hintedIndexEntry->keyPattern, false) >= 0) {
            return Status(ErrorCodes::Error(51175),
                          "The value provided for min() does not come before the value provided "
                          "for max() in the hinted index");
        }

        std::unique_ptr<QuerySolutionNode> solnRoot(QueryPlannerAccess::makeIndexScan(
            *hintedIndexEntry, query, params, finishedMinObj, finishedMaxObj));
        invariant(solnRoot);

        auto soln = QueryPlannerAnalysis::analyzeDataAccess(query, params, std::move(solnRoot));
        if (!soln) {
            return Status(ErrorCodes::NoQueryExecutionPlans,
                          "Sort and covering analysis failed while planning hint/min/max query");
        }
        return singleSolution(std::move(soln));
    }

    for (size_t i = 0; i < relevantIndices.size(); ++i) {
        LOGV2_DEBUG(20971,
                    2,
                    "Relevant index",
                    "indexNumber"_attr = i,
                    "index"_attr = relevantIndices[i].toString());
    }

    // Figure out how useful each index is to each predicate.
    QueryPlannerIXSelect::rateIndices(query.root(), "", relevantIndices, query.getCollator());
    QueryPlannerIXSelect::stripInvalidAssignments(query.root(), relevantIndices);

    // Unless we have GEO_NEAR, TEXT, or a projection, we may be able to apply an optimization
    // in which we strip unnecessary index assignments.
    //
    // Disallowed with projection because assignment to a non-unique index can allow the plan
    // to be covered.
    //
    // TEXT and GEO_NEAR are special because they require the use of a text/geo index in order
    // to be evaluated correctly. Stripping these "mandatory assignments" is therefore invalid.
    if (query.getFindCommandRequest().getProjection().isEmpty() &&
        !QueryPlannerCommon::hasNode(query.root(), MatchExpression::GEO_NEAR) &&
        !QueryPlannerCommon::hasNode(query.root(), MatchExpression::TEXT)) {
        QueryPlannerIXSelect::stripUnneededAssignments(query.root(), relevantIndices);
    }

    // query.root() is now annotated with RelevantTag(s).
    LOGV2_DEBUG(20972, 5, "Rated tree", "tree"_attr = redact(query.root()->debugString()));

    // If there is a GEO_NEAR it must have an index it can use directly.
    const MatchExpression* gnNode = nullptr;
    if (QueryPlannerCommon::hasNode(query.root(), MatchExpression::GEO_NEAR, &gnNode)) {
        // No index for GEO_NEAR?  No query.
        RelevantTag* tag = static_cast<RelevantTag*>(gnNode->getTag());
        if (!tag || (0 == tag->first.size() && 0 == tag->notFirst.size())) {
            LOGV2_DEBUG(20973, 5, "Unable to find index for $geoNear query");
            // Don't leave tags on query tree.
            query.root()->resetTag();
            return Status(ErrorCodes::NoQueryExecutionPlans,
                          "unable to find index for $geoNear query");
        }

        LOGV2_DEBUG(20974,
                    5,
                    "Rated tree after geonear processing",
                    "tree"_attr = redact(query.root()->debugString()));
    }

    // Likewise, if there is a TEXT it must have an index it can use directly.
    const MatchExpression* textNode = nullptr;
    if (QueryPlannerCommon::hasNode(query.root(), MatchExpression::TEXT, &textNode)) {
        RelevantTag* tag = static_cast<RelevantTag*>(textNode->getTag());

        // Exactly one text index required for TEXT.  We need to check this explicitly because
        // the text stage can't be built if no text index exists or there is an ambiguity as to
        // which one to use.
        size_t textIndexCount = 0;
        for (size_t i = 0; i < fullIndexList.size(); i++) {
            if (INDEX_TEXT == fullIndexList[i].type) {
                textIndexCount++;
            }
        }
        if (textIndexCount != 1) {
            // Don't leave tags on query tree.
            query.root()->resetTag();
            return Status(ErrorCodes::NoQueryExecutionPlans,
                          "need exactly one text index for $text query");
        }

        // Error if the text node is tagged with zero indices.
        if (0 == tag->first.size() && 0 == tag->notFirst.size()) {
            // Don't leave tags on query tree.
            query.root()->resetTag();
            return Status(ErrorCodes::NoQueryExecutionPlans,
                          "failed to use text index to satisfy $text query (if text index is "
                          "compound, are equality predicates given for all prefix fields?)");
        }

        // At this point, we know that there is only one text index and that the TEXT node is
        // assigned to it.
        invariant(1 == tag->first.size() + tag->notFirst.size());

        LOGV2_DEBUG(20975,
                    5,
                    "Rated tree after text processing",
                    "tree"_attr = redact(query.root()->debugString()));
    }

    std::vector<std::unique_ptr<QuerySolution>> out;

    // If we have any relevant indices, we try to create indexed plans.
    if (0 < relevantIndices.size()) {
        // The enumerator spits out trees tagged with IndexTag(s).
        PlanEnumeratorParams enumParams;
        enumParams.intersect = params.options & QueryPlannerParams::INDEX_INTERSECTION;
        enumParams.root = query.root();
        enumParams.indices = &relevantIndices;
        enumParams.enumerateOrChildrenLockstep =
            params.options & QueryPlannerParams::ENUMERATE_OR_CHILDREN_LOCKSTEP;

        PlanEnumerator planEnumerator(enumParams);
        uassertStatusOKWithContext(planEnumerator.init(), "failed to initialize plan enumerator");

        unique_ptr<MatchExpression> nextTaggedTree;
        while ((nextTaggedTree = planEnumerator.getNext()) &&
               (out.size() < params.maxIndexedSolutions)) {
            LOGV2_DEBUG(20976,
                        5,
                        "About to build solntree from tagged tree",
                        "tree"_attr = redact(nextTaggedTree->debugString()));

            // Store the plan cache index tree before calling prepareForAccessingPlanning(), so
            // that the PlanCacheIndexTree has the same sort as the MatchExpression used to
            // generate the plan cache key.
            std::unique_ptr<MatchExpression> clone(nextTaggedTree->shallowClone());
            std::unique_ptr<PlanCacheIndexTree> cacheData;
            auto statusWithCacheData = cacheDataFromTaggedTree(clone.get(), relevantIndices);
            if (!statusWithCacheData.isOK()) {
                LOGV2_DEBUG(20977,
                            5,
                            "Query is not cachable",
                            "reason"_attr = redact(statusWithCacheData.getStatus().reason()));
            } else {
                cacheData = std::move(statusWithCacheData.getValue());
            }

            // We have already cached the tree in canonical order, so now we can order the nodes
            // for access planning.
            prepareForAccessPlanning(nextTaggedTree.get());

            // This can fail if enumeration makes a mistake.
            std::unique_ptr<QuerySolutionNode> solnRoot(QueryPlannerAccess::buildIndexedDataAccess(
                query, std::move(nextTaggedTree), relevantIndices, params));

            if (!solnRoot) {
                continue;
            }

            auto soln = QueryPlannerAnalysis::analyzeDataAccess(query, params, std::move(solnRoot));
            if (soln) {
                soln->_enumeratorExplainInfo.merge(planEnumerator._explainInfo);
                LOGV2_DEBUG(20978,
                            5,
                            "Planner: adding solution",
                            "solution"_attr = redact(soln->toString()));
                if (statusWithCacheData.isOK()) {
                    SolutionCacheData* scd = new SolutionCacheData();
                    scd->tree = std::move(cacheData);
                    soln->cacheData.reset(scd);
                }
                out.push_back(std::move(soln));
            }
        }
    }

    // Don't leave tags on query tree.
    query.root()->resetTag();

    LOGV2_DEBUG(20979, 5, "Planner: outputted indexed solutions", "numSolutions"_attr = out.size());

    // Produce legible error message for failed OR planning with a TEXT child.
    // TODO: support collection scan for non-TEXT children of OR.
    if (out.size() == 0 && textNode != nullptr &&
        MatchExpression::OR == query.root()->matchType()) {
        MatchExpression* root = query.root();
        for (size_t i = 0; i < root->numChildren(); ++i) {
            if (textNode == root->getChild(i)) {
                return Status(ErrorCodes::NoQueryExecutionPlans,
                              "Failed to produce a solution for TEXT under OR - "
                              "other non-TEXT clauses under OR have to be indexed as well.");
            }
        }
    }

    // An index was hinted. If there are any solutions, they use the hinted index.  If not, we
    // scan the entire index to provide results and output that as our plan.  This is the
    // desired behavior when an index is hinted that is not relevant to the query. In the case
    // that
    // $** index is hinted, we do not want this behavior.
    if (hintedIndexBson && relevantIndices.size() == 1) {
        if (out.size() > 0) {
            return {std::move(out)};
        }
        if (relevantIndices.front().type == IndexType::INDEX_WILDCARD) {
            return Status(
                ErrorCodes::NoQueryExecutionPlans,
                "$hint: refusing to build whole-index solution, because it's a wildcard index");
        }

        // Return hinted index solution if found.
        if (auto soln = buildWholeIXSoln(relevantIndices.front(), query, params)) {
            LOGV2_DEBUG(20980, 5, "Planner: outputting soln that uses hinted index as scan");
            return singleSolution(std::move(soln));
        }
        return Status(ErrorCodes::NoQueryExecutionPlans,
                      "Failed to build whole-index solution for $hint");
    }

    // If a sort order is requested, there may be an index that provides it, even if that
    // index is not over any predicates in the query.
    //
    if (query.getSortPattern() &&
        !QueryPlannerCommon::hasNode(query.root(), MatchExpression::GEO_NEAR) &&
        !QueryPlannerCommon::hasNode(query.root(), MatchExpression::TEXT)) {
        // See if we have a sort provided from an index already.
        // This is implied by the presence of a non-blocking solution.
        bool usingIndexToSort = false;
        for (size_t i = 0; i < out.size(); ++i) {
            auto soln = out[i].get();
            if (!soln->hasBlockingStage) {
                usingIndexToSort = true;
                break;
            }
        }

        if (!usingIndexToSort) {
            for (size_t i = 0; i < fullIndexList.size(); ++i) {
                const IndexEntry& index = fullIndexList[i];
                // Only a regular index or the non-hashed prefix of a compound hashed index can
                // be used to provide a sort. In addition, the index needs to be a non-sparse
                // index.
                //
                // TODO: Sparse indexes can't normally provide a sort, because non-indexed
                // documents could potentially be missing from the result set.  However, if the
                // query predicate can be used to guarantee that all documents to be returned
                // are indexed, then the index should be able to provide the sort.
                //
                // For example:
                // - Sparse index {a: 1, b: 1} should be able to provide a sort for
                //   find({b: 1}).sort({a: 1}).  SERVER-13908.
                // - Index {a: 1, b: "2dsphere"} (which is "geo-sparse", if
                //   2dsphereIndexVersion=2) should be able to provide a sort for
                //   find({b: GEO}).sort({a:1}).  SERVER-10801.
                if (index.type != INDEX_BTREE && index.type != INDEX_HASHED) {
                    continue;
                }
                if (index.sparse) {
                    continue;
                }

                // If the index collation differs from the query collation, the index should not
                // be used to provide a sort, because strings will be ordered incorrectly.
                if (!CollatorInterface::collatorsMatch(index.collator, query.getCollator())) {
                    continue;
                }

                // Partial indexes can only be used to provide a sort only if the query
                // predicate is compatible.
                if (index.filterExpr && !expression::isSubsetOf(query.root(), index.filterExpr)) {
                    continue;
                }

                const BSONObj kp = QueryPlannerAnalysis::getSortPattern(index.keyPattern);
                if (providesSort(query, kp)) {
                    LOGV2_DEBUG(
                        20981, 5, "Planner: outputting soln that uses index to provide sort");
                    auto soln = buildWholeIXSoln(fullIndexList[i], query, params);
                    if (soln) {
                        PlanCacheIndexTree* indexTree = new PlanCacheIndexTree();
                        indexTree->setIndexEntry(fullIndexList[i]);
                        SolutionCacheData* scd = new SolutionCacheData();
                        scd->tree.reset(indexTree);
                        scd->solnType = SolutionCacheData::WHOLE_IXSCAN_SOLN;
                        scd->wholeIXSolnDir = 1;

                        soln->cacheData.reset(scd);
                        out.push_back(std::move(soln));
                    }
                }
                if (providesSort(query, QueryPlannerCommon::reverseSortObj(kp))) {
                    LOGV2_DEBUG(
                        20982,
                        5,
                        "Planner: outputting soln that uses (reverse) index to provide sort");
                    auto soln = buildWholeIXSoln(fullIndexList[i], query, params, -1);
                    if (soln) {
                        PlanCacheIndexTree* indexTree = new PlanCacheIndexTree();
                        indexTree->setIndexEntry(fullIndexList[i]);
                        SolutionCacheData* scd = new SolutionCacheData();
                        scd->tree.reset(indexTree);
                        scd->solnType = SolutionCacheData::WHOLE_IXSCAN_SOLN;
                        scd->wholeIXSolnDir = -1;

                        soln->cacheData.reset(scd);
                        out.push_back(std::move(soln));
                    }
                }
            }
        }

        // The base index is sorted on some key, so it's possible we might want to use
        // a collection scan to provide the sort requested
        if (params.clusteredInfo) {
            if (CollatorInterface::collatorsMatch(params.clusteredCollectionCollator,
                                                  query.getCollator())) {
                auto kp = clustered_util::getSortPattern(params.clusteredInfo->getIndexSpec());
                int direction = 0;
                if (providesSort(query, kp)) {
                    direction = 1;
                } else if (providesSort(query, QueryPlannerCommon::reverseSortObj(kp))) {
                    direction = -1;
                }

                if (direction != 0) {
                    auto soln = buildCollscanSoln(query, isTailable, params, direction);
                    if (soln) {
                        LOGV2_DEBUG(6082401,
                                    5,
                                    "Planner: outputting soln that uses clustered index to "
                                    "provide sort");
                        SolutionCacheData* scd = new SolutionCacheData();
                        scd->solnType = SolutionCacheData::COLLSCAN_SOLN;
                        scd->wholeIXSolnDir = direction;

                        soln->cacheData.reset(scd);
                        out.push_back(std::move(soln));
                    }
                }
            }
        }
    }

    // If a projection exists, there may be an index that allows for a covered plan, even if
    // none were considered earlier.
    const auto projection = query.getProj();
    if (params.options & QueryPlannerParams::GENERATE_COVERED_IXSCANS && out.size() == 0 &&
        query.getQueryObj().isEmpty() && projection && !projection->requiresDocument()) {

        const auto* indicesToConsider = hintedIndexBson ? &relevantIndices : &fullIndexList;
        for (auto&& index : *indicesToConsider) {
            if (index.type != INDEX_BTREE || index.multikey || index.sparse || index.filterExpr ||
                !CollatorInterface::collatorsMatch(index.collator, query.getCollator())) {
                continue;
            }

            QueryPlannerParams paramsForCoveredIxScan;
            auto soln = buildWholeIXSoln(index, query, paramsForCoveredIxScan);
            if (soln && !soln->root()->fetched()) {
                LOGV2_DEBUG(
                    20983, 5, "Planner: outputting soln that uses index to provide projection");
                PlanCacheIndexTree* indexTree = new PlanCacheIndexTree();
                indexTree->setIndexEntry(index);

                SolutionCacheData* scd = new SolutionCacheData();
                scd->tree.reset(indexTree);
                scd->solnType = SolutionCacheData::WHOLE_IXSCAN_SOLN;
                scd->wholeIXSolnDir = 1;
                soln->cacheData.reset(scd);

                out.push_back(std::move(soln));
                break;
            }
        }
    }

    // Check whether we're eligible to use the columnar index, assuming no other indexes can be
    // used.
    if (out.empty()) {
        auto statusWithSoln = tryToBuildColumnScan(params, query);
        if (statusWithSoln.isOK()) {
            out.emplace_back(std::move(statusWithSoln.getValue()));
        } else {
            LOGV2_DEBUG(
                6726401, 4, "Not using a column scan", "reason"_attr = statusWithSoln.getStatus());
        }
    }

    // The caller can explicitly ask for a collscan.
    bool collscanRequested = (params.options & QueryPlannerParams::INCLUDE_COLLSCAN);

    // No indexed plans?  We must provide a collscan if possible or else we can't run the query.
    bool collScanRequired = 0 == out.size();
    if (collScanRequired && !canTableScan(params)) {
        return Status(ErrorCodes::NoQueryExecutionPlans,
                      "No indexed plans available, and running with 'notablescan'");
    }

    // geoNear and text queries *require* an index.
    // Also, if a hint is specified it indicates that we MUST use it.
    bool possibleToCollscan =
        !QueryPlannerCommon::hasNode(query.root(), MatchExpression::GEO_NEAR) &&
        !QueryPlannerCommon::hasNode(query.root(), MatchExpression::TEXT) && !hintedIndexBson;
    if (collScanRequired && !possibleToCollscan) {
        return Status(ErrorCodes::NoQueryExecutionPlans, "No query solutions");
    }

    if (possibleToCollscan && (collscanRequested || collScanRequired)) {
        auto collscan = buildCollscanSoln(query, isTailable, params);
        if (!collscan && collScanRequired) {
            return Status(ErrorCodes::NoQueryExecutionPlans,
                          "Failed to build collection scan soln");
        }
        if (collscan) {
            LOGV2_DEBUG(20984,
                        5,
                        "Planner: outputting a collection scan",
                        "collectionScan"_attr = redact(collscan->toString()));
            SolutionCacheData* scd = new SolutionCacheData();
            scd->solnType = SolutionCacheData::COLLSCAN_SOLN;
            collscan->cacheData.reset(scd);
            out.push_back(std::move(collscan));
        }
    }

    invariant(out.size() > 0);
    return {std::move(out)};
}

/**
 * The 'query' might contain parts of aggregation pipeline. For now, we plan those separately and
 * later attach the agg portion of the plan to the solution(s) for the "find" part of the query.
 */
std::unique_ptr<QuerySolution> QueryPlanner::extendWithAggPipeline(
    const CanonicalQuery& query,
    std::unique_ptr<QuerySolution>&& solution,
    const std::map<NamespaceString, SecondaryCollectionInfo>& secondaryCollInfos) {
    if (query.pipeline().empty()) {
        return nullptr;
    }

    std::unique_ptr<QuerySolutionNode> solnForAgg = std::make_unique<SentinelNode>();
    for (auto& innerStage : query.pipeline()) {
        auto groupStage = dynamic_cast<DocumentSourceGroup*>(innerStage->documentSource());
        if (groupStage) {
            solnForAgg =
                std::make_unique<GroupNode>(std::move(solnForAgg),
                                            groupStage->getIdExpression(),
                                            groupStage->getAccumulatedFields(),
                                            groupStage->doingMerge(),
                                            innerStage->isLastSource() /* shouldProduceBson */);
            continue;
        }

        auto lookupStage = dynamic_cast<DocumentSourceLookUp*>(innerStage->documentSource());
        if (lookupStage) {
            tassert(6369000,
                    "This $lookup stage should be compatible with SBE",
                    lookupStage->sbeCompatible());
            auto [strategy, idxEntry] = QueryPlannerAnalysis::determineLookupStrategy(
                lookupStage->getFromNs().toString(),
                lookupStage->getForeignField()->fullPath(),
                secondaryCollInfos,
                query.getExpCtx()->allowDiskUse,
                query.getCollator());
            auto eqLookupNode =
                std::make_unique<EqLookupNode>(std::move(solnForAgg),
                                               lookupStage->getFromNs(),
                                               lookupStage->getLocalField()->fullPath(),
                                               lookupStage->getForeignField()->fullPath(),
                                               lookupStage->getAsField().fullPath(),
                                               strategy,
                                               std::move(idxEntry),
                                               innerStage->isLastSource() /* shouldProduceBson */);
            solnForAgg = std::move(eqLookupNode);
            continue;
        }

        tasserted(5842400,
                  "Cannot support pushdown of a stage other than $group or $lookup at the moment");
    }

    solution->extendWith(std::move(solnForAgg));
    return QueryPlannerAnalysis::removeInclusionProjectionBelowGroup(std::move(solution));
}

StatusWith<std::unique_ptr<QuerySolution>> QueryPlanner::choosePlanForSubqueries(
    const CanonicalQuery& query,
    const QueryPlannerParams& params,
    QueryPlanner::SubqueriesPlanningResult planningResult,
    std::function<StatusWith<std::unique_ptr<QuerySolution>>(
        CanonicalQuery* cq, std::vector<unique_ptr<QuerySolution>>)> multiplanCallback) {
    // This is the skeleton of index selections that is inserted into the cache.
    std::unique_ptr<PlanCacheIndexTree> cacheData(new PlanCacheIndexTree());

    for (size_t i = 0; i < planningResult.orExpression->numChildren(); ++i) {
        auto orChild = planningResult.orExpression->getChild(i);
        auto branchResult = planningResult.branches[i].get();

        if (branchResult->cachedData.get()) {
            // We can get the index tags we need out of the cache.
            Status tagStatus = tagOrChildAccordingToCache(
                cacheData.get(), branchResult->cachedData.get(), orChild, planningResult.indexMap);
            if (!tagStatus.isOK()) {
                return tagStatus;
            }
        } else if (1 == branchResult->solutions.size()) {
            QuerySolution* soln = branchResult->solutions.front().get();
            Status tagStatus = tagOrChildAccordingToCache(
                cacheData.get(), soln->cacheData.get(), orChild, planningResult.indexMap);
            if (!tagStatus.isOK()) {
                return tagStatus;
            }
        } else {
            // N solutions, rank them.

            invariant(!branchResult->solutions.empty());

            auto multiPlanStatus = multiplanCallback(branchResult->canonicalQuery.get(),
                                                     std::move(branchResult->solutions));
            if (!multiPlanStatus.isOK()) {
                return multiPlanStatus;
            }

            auto bestSoln = std::move(multiPlanStatus.getValue());

            // Check that we have good cache data. For example, we don't cache things
            // for 2d indices.

            if (nullptr == bestSoln->cacheData.get()) {
                str::stream ss;
                ss << "No cache data for subchild " << orChild->debugString();
                return Status(ErrorCodes::NoQueryExecutionPlans, ss);
            }

            if (SolutionCacheData::USE_INDEX_TAGS_SOLN != bestSoln->cacheData->solnType) {
                str::stream ss;
                ss << "No indexed cache data for subchild " << orChild->debugString();
                return Status(ErrorCodes::NoQueryExecutionPlans, ss);
            }

            // Add the index assignments to our original query.
            Status tagStatus = QueryPlanner::tagAccordingToCache(
                orChild, bestSoln->cacheData->tree.get(), planningResult.indexMap);
            if (!tagStatus.isOK()) {
                str::stream ss;
                ss << "Failed to extract indices from subchild " << orChild->debugString();
                return tagStatus.withContext(ss);
            }

            cacheData->children.push_back(bestSoln->cacheData->tree->clone());
        }
    }

    // Must do this before using the planner functionality.
    prepareForAccessPlanning(planningResult.orExpression.get());

    // Use the cached index assignments to build solnRoot. Takes ownership of '_orExpression'.
    std::unique_ptr<QuerySolutionNode> solnRoot(QueryPlannerAccess::buildIndexedDataAccess(
        query, std::move(planningResult.orExpression), params.indices, params));

    if (!solnRoot) {
        str::stream ss;
        ss << "Failed to build indexed data path for subplanned query\n";
        return Status(ErrorCodes::NoQueryExecutionPlans, ss);
    }

    LOGV2_DEBUG(
        20602, 5, "Subplanner: fully tagged tree", "solnRoot"_attr = redact(solnRoot->toString()));

    auto compositeSolution =
        QueryPlannerAnalysis::analyzeDataAccess(query, params, std::move(solnRoot));

    if (nullptr == compositeSolution.get()) {
        str::stream ss;
        ss << "Failed to analyze subplanned query";
        return Status(ErrorCodes::NoQueryExecutionPlans, ss);
    }

    LOGV2_DEBUG(20603,
                5,
                "Subplanner: Composite solution",
                "compositeSolution"_attr = redact(compositeSolution->toString()));

    return std::move(compositeSolution);
}

StatusWith<QueryPlanner::SubqueriesPlanningResult> QueryPlanner::planSubqueries(
    OperationContext* opCtx,
    std::function<std::unique_ptr<SolutionCacheData>(
        const CanonicalQuery& cq, const CollectionPtr& coll)> getSolutionCachedData,
    const CollectionPtr& collection,
    const CanonicalQuery& query,
    const QueryPlannerParams& params) {
    invariant(query.root()->matchType() == MatchExpression::OR);
    invariant(query.root()->numChildren(), "Cannot plan subqueries for an $or with no children");

    SubqueriesPlanningResult planningResult{query.root()->shallowClone()};
    for (size_t i = 0; i < params.indices.size(); ++i) {
        const IndexEntry& ie = params.indices[i];
        const auto insertionRes = planningResult.indexMap.insert(std::make_pair(ie.identifier, i));
        // Be sure the key was not already in the map.
        invariant(insertionRes.second);
        log_detail::logSubplannerIndexEntry(ie, i);
    }

    for (size_t i = 0; i < planningResult.orExpression->numChildren(); ++i) {
        // We need a place to shove the results from planning this branch.
        planningResult.branches.push_back(
            std::make_unique<SubqueriesPlanningResult::BranchPlanningResult>());
        auto branchResult = planningResult.branches.back().get();
        auto orChild = planningResult.orExpression->getChild(i);

        // Turn the i-th child into its own query.
        auto statusWithCQ = CanonicalQuery::canonicalize(opCtx, query, orChild);
        if (!statusWithCQ.isOK()) {
            str::stream ss;
            ss << "Can't canonicalize subchild " << orChild->debugString() << " "
               << statusWithCQ.getStatus().reason();
            return Status(ErrorCodes::BadValue, ss);
        }

        branchResult->canonicalQuery = std::move(statusWithCQ.getValue());
        branchResult->canonicalQuery->setSbeCompatible(query.isSbeCompatible());

        // Plan the i-th child. We might be able to find a plan for the i-th child in the plan
        // cache. If there's no cached plan, then we generate and rank plans using the MPS.

        // Populate branchResult->cachedData if an active cachedData entry exists.
        if (getSolutionCachedData) {
            branchResult->cachedData =
                getSolutionCachedData(*branchResult->canonicalQuery, collection);
        }

        if (branchResult->cachedData) {
            log_detail::logCachedPlanFound(planningResult.orExpression->numChildren(), i);
        } else {
            // No CachedSolution found. We'll have to plan from scratch.
            log_detail::logCachedPlanNotFound(planningResult.orExpression->numChildren(), i);

            // We don't set NO_TABLE_SCAN because peeking at the cache data will keep us from
            // considering any plan that's a collscan.
            invariant(branchResult->solutions.empty());
            auto statusWithMultiPlanSolns =
                QueryPlanner::plan(*branchResult->canonicalQuery, params);
            if (!statusWithMultiPlanSolns.isOK()) {
                str::stream ss;
                ss << "Can't plan for subchild " << branchResult->canonicalQuery->toString() << " "
                   << statusWithMultiPlanSolns.getStatus().reason();
                return Status(ErrorCodes::BadValue, ss);
            }
            branchResult->solutions = std::move(statusWithMultiPlanSolns.getValue());

            log_detail::logNumberOfSolutions(branchResult->solutions.size());
        }
    }

    return std::move(planningResult);
}
}  // namespace mongo
