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


#include <cstring>

#include <s2cellid.h>

#include <absl/container/node_hash_map.h>
#include <absl/container/node_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/index_names.h"
#include "mongo/db/local_catalog/clustered_collection_options_gen.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/matcher/expression_hasher.h"
#include "mongo/db/matcher/expression_text.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_internal_projection.h"
#include "mongo/db/pipeline/document_source_internal_replace_root.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_set_window_fields.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/compiler/ce/exact/exact_cardinality.h"
#include "mongo/db/query/compiler/logical_model/projection/projection.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/db/query/compiler/metadata/index_entry.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/cardinality_estimator.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/cost_estimator.h"
#include "mongo/db/query/compiler/physical_model/query_solution/eof_node_type.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/distinct_access.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/index_tag.h"
#include "mongo/db/query/plan_cache/classic_plan_cache.h"
#include "mongo/db/query/plan_cache/plan_cache_diagnostic_printer.h"
#include "mongo/db/query/plan_enumerator/plan_enumerator.h"
#include "mongo/db/query/plan_enumerator/plan_enumerator_explain_info.h"
#include "mongo/db/query/planner_access.h"
#include "mongo/db/query/planner_analysis.h"
#include "mongo/db/query/planner_ixselect.h"
#include "mongo/db/query/query_knob_configuration.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/search/mongot_cursor.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <deque>
#include <string>
#include <utility>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

namespace {
MONGO_FAIL_POINT_DEFINE(queryPlannerAlwaysFails);
MONGO_FAIL_POINT_DEFINE(planFromCacheAlwaysFails);

/**
 * Attempts to apply the index tags from 'branchCacheData' to 'orChild'. If the index assignments
 * cannot be applied, return the error from the process. Otherwise the tags are applied and success
 * is returned.
 */
Status tagOrChildAccordingToCache(const SolutionCacheData* branchCacheData,
                                  MatchExpression* orChild,
                                  const std::map<IndexEntry::Identifier, size_t>& indexMap) {
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

    return Status::OK();
}

size_t hashTaggedMatchExpression(MatchExpression* expr, const std::vector<IndexEntry>& indexes) {
    const MatchExpressionHasher hash{
        MatchExpression::HashParam{HashValuesOrParams::kHashIndexTags, &indexes}};
    return hash(expr);
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
        firstHintElt.type() == BSONType::string) {
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

bool isSolutionBoundedCollscan(const QuerySolution* querySoln) {
    auto [node, count] = querySoln->getFirstNodeByType(StageType::STAGE_COLLSCAN);
    if (node) {
        const unsigned long numCollscanNodes = count;
        tassert(8186301,
                str::stream() << "Unexpected number of collscan nodes found. Expected: 1. Found: "
                              << numCollscanNodes,
                count == 1);
        auto collscan = static_cast<const CollectionScanNode*>(node);
        return collscan->minRecord || collscan->maxRecord;
    }
    return false;
}

bool canUseClusteredCollScan(QuerySolutionNode* node,
                             std::vector<std::unique_ptr<QuerySolutionNode>> children) {
    if (node->getType() == StageType::STAGE_COLLSCAN) {
        return static_cast<CollectionScanNode*>(node)->doClusteredCollectionScanClassic();
    }

    // We assume we are subplanning the children of an OR expression and therefore should expect one
    // child per node. However, we have to recur down to the child leaf node to check if we can
    // perform a clustered collection scan.
    if (1 == children.size()) {
        QuerySolutionNode* child = children[0].get();
        // Find the leaf node of the solution node.
        while (1 == child->children.size()) {
            child = child->children[0].get();
        }
        if (child->getType() == StageType::STAGE_COLLSCAN) {
            return static_cast<CollectionScanNode*>(child)->doClusteredCollectionScanClassic();
        }
    }
    return false;
}

/**
 * Creates a query solution node for $search plans that are being pushed down into SBE.
 */
StatusWith<std::unique_ptr<QuerySolution>> tryToBuildSearchQuerySolution(
    const QueryPlannerParams& params, const CanonicalQuery& query) {
    if (query.cqPipeline().empty()) {
        static const auto status =
            Status{ErrorCodes::InvalidOptions,
                   "not building $search node because the query pipeline is empty"_sd};
        return status;
    }

    if (query.isSearchQuery()) {
        tassert(7816300,
                "Pushing down $search into SBE but forceClassicEngine is on"_sd,
                !query.getExpCtx()->getQueryKnobConfiguration().isForceClassicEngineEnabled());

        tassert(7816301,
                "Pushing down $search into SBE but featureFlagSearchInSbe is disabled."_sd,
                feature_flags::gFeatureFlagSearchInSbe.isEnabled());

        // Build a SearchNode in order to retrieve the search info.
        auto searchNode = search_helpers::getSearchNode(query.cqPipeline().front().get());

        if (searchNode->searchQuery.getBoolField(mongot_cursor::kReturnStoredSourceArg) ||
            searchNode->isSearchMeta) {
            auto querySoln = std::make_unique<QuerySolution>();
            querySoln->setRoot(std::move(searchNode));
            return std::move(querySoln);
        }
        // Apply shard filter if needed.
        return QueryPlannerAnalysis::analyzeDataAccess(query, params, std::move(searchNode));
    }

    {
        static const auto status =
            Status{ErrorCodes::InvalidOptions, "no search stage found at front of pipeline"_sd};
        return status;
    }
}
}  // namespace

using std::unique_ptr;

static bool is2DIndex(const BSONObj& pattern) {
    BSONObjIterator it(pattern);
    while (it.more()) {
        BSONElement e = it.next();
        if (BSONType::string == e.type() && (e.valueStringData() == "2d")) {
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
            case QueryPlannerParams::STRICT_NO_TABLE_SCAN:
                ss << "STRICT_NO_TABLE_SCAN ";
                break;
            case QueryPlannerParams::IGNORE_QUERY_SETTINGS:
                ss << "IGNORE_QUERY_SETTINGS ";
                break;
            case QueryPlannerParams::TARGET_SBE_STAGE_BUILDER:
                ss << "TARGET_SBE_STAGE_BUILDER ";
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

/**
 * Determine the direction of the scan needed for the query. Defaults to 1 unless this is a
 * clustered collection and we have a sort that can be provided by the clustered index.
 */
int determineCollscanDirection(const CanonicalQuery& query, const QueryPlannerParams& params) {
    return QueryPlannerCommon::determineClusteredScanDirection(
               query, params.clusteredInfo, params.clusteredCollectionCollator)
        .value_or(1);
}

/**
 * Try build EOF solution if applicable.
 *
 * If it is known that this query cannot match any documents, and is not on a "special" collection,
 * we can use an EOF node safely.
 *
 * returns (possibly null) solution
 */
std::unique_ptr<QuerySolution> tryEofSoln(const CanonicalQuery& query) {
    if (!query.getPrimaryMatchExpression()->isTriviallyFalse()) {
        // Query is not trivially false; it could actually match documents.
        return nullptr;
    }
    const auto& nss = query.nss();

    // Return EOF solution for trivially false expressions.
    // Unless the query is against Oplog (change streams) or change collections (serverless
    // change streams) because in such cases we still need the scan to happen to advance the
    // visibility timestamp and resume token.
    if (nss.isOplog() || nss.isChangeCollection()) {
        return nullptr;
    }
    auto soln = std::make_unique<QuerySolution>();
    soln->setRoot(std::make_unique<EofNode>(eof_node::EOFType::PredicateEvalsToFalse));
    return soln;
}

std::unique_ptr<QuerySolution> buildCollscanSoln(const CanonicalQuery& query,
                                                 bool tailable,
                                                 const QueryPlannerParams& params,
                                                 boost::optional<int> direction = boost::none) {
    std::unique_ptr<QuerySolutionNode> solnRoot(QueryPlannerAccess::makeCollectionScan(
        query,
        tailable,
        params,
        direction.value_or(determineCollscanDirection(query, params)),
        query.getPrimaryMatchExpression()));
    return QueryPlannerAnalysis::analyzeDataAccess(query, params, std::move(solnRoot));
}

std::unique_ptr<QuerySolution> buildVirtScanSoln(const std::vector<BSONArray>& docs,
                                                 bool hasRecordId,
                                                 const BSONObj& indexKeyPattern,
                                                 const CanonicalQuery& query,
                                                 const QueryPlannerParams& params) {
    const auto kScanType = indexKeyPattern.isEmpty() ? VirtualScanNode::ScanType::kCollScan
                                                     : VirtualScanNode::ScanType::kIxscan;

    std::unique_ptr<QuerySolutionNode> solnRoot =
        std::make_unique<VirtualScanNode>(docs, kScanType, hasRecordId);
    solnRoot->filter = query.getPrimaryMatchExpression()->clone();

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
        QueryPlannerAccess::scanWholeIndex(index, query, direction.value_or(1)));
    return QueryPlannerAnalysis::analyzeDataAccess(query, params, std::move(solnRoot));
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
            IndexTag* indexTag = checked_cast<IndexTag*>(dest.tagData.get());
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
                                         const std::map<IndexEntry::Identifier, size_t>& indexMap) {
    if (nullptr == filter) {
        return Status(ErrorCodes::NoQueryExecutionPlans, "Cannot tag tree: filter is NULL.");
    }
    if (nullptr == indexTree) {
        return Status(ErrorCodes::NoQueryExecutionPlans, "Cannot tag tree: indexTree is NULL.");
    }

    // We're tagging the tree here, so it shouldn't have
    // any tags hanging off yet.
    MONGO_verify(nullptr == filter->getTag());

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
    const SolutionCacheData& solnCacheData) {
    // Create an RAII object that prints the cached plan in the case of a tassert or crash.
    ScopedDebugInfo planCacheDiagnostics(
        "PlanCacheDiagnostics", diagnostic_printers::PlanCacheDiagnosticPrinter{solnCacheData});

    if (auto scoped = planFromCacheAlwaysFails.scoped(); MONGO_unlikely(scoped.isActive())) {
        tasserted(9319600, "Hit planFromCacheAlwaysFails fail point");
    }

    // A query not suitable for caching should not have made its way into the cache. The exception
    // is if `internalQueryDisablePlanCache` was enabled after a cache entry was made. This knob
    // marks all entries as "should not cache", meaning we would end up in a state where a query
    // should not be cached, but is in the cached. This is why we check the knob.
    dassert(internalQueryDisablePlanCache.load() || shouldCacheQuery(query));

    if (SolutionCacheData::WHOLE_IXSCAN_SOLN == solnCacheData.solnType) {
        // The solution can be constructed by a scan over the entire index.
        auto soln = buildWholeIXSoln(
            *solnCacheData.tree->entry, query, params, solnCacheData.wholeIXSolnDir);
        if (!soln) {
            return Status(ErrorCodes::NoQueryExecutionPlans,
                          "plan cache error: soln that uses index to provide sort");
        } else {
            return {std::move(soln)};
        }
    } else if (SolutionCacheData::COLLSCAN_SOLN == solnCacheData.solnType) {
        // The cached solution is a collection scan. We don't cache collscans
        // with tailable==true, hence the false below.
        auto soln = buildCollscanSoln(query, false, params, solnCacheData.wholeIXSolnDir);
        if (!soln) {
            return Status(ErrorCodes::NoQueryExecutionPlans,
                          "plan cache error: collection scan soln");
        } else {
            return {std::move(soln)};
        }
    } else if (SolutionCacheData::VIRTSCAN_SOLN == solnCacheData.solnType) {
        tassert(9049200,
                "Constructing a virtual scan plan from cache requires 'VirtualScanCacheData",
                solnCacheData.virtualScanData);
        const VirtualScanCacheData& vscd = *solnCacheData.virtualScanData;
        auto soln =
            buildVirtScanSoln(vscd.docs, vscd.hasRecordId, vscd.indexKeyPattern, query, params);
        if (!soln) {
            return Status(ErrorCodes::NoQueryExecutionPlans, "plan cache error: virtual scan soln");
        } else {
            return {std::move(soln)};
        }
    }

    // SolutionCacheData::USE_TAGS_SOLN == cacheData->solnType
    // If we're here then this is neither the whole index scan or collection scan
    // cases, and we proceed by using the PlanCacheIndexTree to tag the query tree.

    // Create a copy of the expression tree.  We use cachedSoln to annotate this with indices.
    unique_ptr<MatchExpression> clone = query.getPrimaryMatchExpression()->clone();

    LOGV2_DEBUG(20963,
                5,
                "Tagging the match expression according to cache data",
                "filter"_attr = redact(clone->debugString()),
                "cacheData"_attr = redact(solnCacheData.toString()));

    RelevantFieldIndexMap fields;
    QueryPlannerIXSelect::getFields(query.getPrimaryMatchExpression(), &fields);
    // We will not cache queries with 'hint'.
    std::vector<IndexEntry> expandedIndexes = QueryPlannerIXSelect::expandIndexes(
        fields, params.mainCollectionInfo.indexes, false /* indexHinted */);

    // Map from index name to index number.
    std::map<IndexEntry::Identifier, size_t> indexMap;
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

    Status s = tagAccordingToCache(clone.get(), solnCacheData.tree.get(), indexMap);
    if (!s.isOK()) {
        return s;
    }

    // Must be performed before nodes are sorted in prepareForAccessPlanning(). See
    // QueryPlanner::plan() for details.
    const auto taggedMatchExpressionHash = hashTaggedMatchExpression(clone.get(), expandedIndexes);

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
                                    << query.toStringShortForErrorMsg());
    }

    auto soln = QueryPlannerAnalysis::analyzeDataAccess(query, params, std::move(solnRoot));
    if (!soln) {
        return Status(ErrorCodes::NoQueryExecutionPlans,
                      str::stream() << "Failed to analyze plan from cache. Query: "
                                    << query.toStringShortForErrorMsg());
    }

    soln->taggedMatchExpressionHash = taggedMatchExpressionHash;

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

// If no table scan option is set the planner may not return any plan containing a collection scan.
// Yet clusteredIdxScans are still allowed as they are not a full collection scan but a bounded
// collection scan.
bool noTableScan(const QueryPlannerParams& params) {
    return (params.mainCollectionInfo.options & QueryPlannerParams::NO_TABLE_SCAN);
}

// Used internally if the planner should also avoid retruning a plan containing a clusteredIDX scan.
bool noTableAndClusteredIDXScan(const QueryPlannerParams& params) {
    return (params.mainCollectionInfo.options & QueryPlannerParams::STRICT_NO_TABLE_SCAN);
}

bool isClusteredScan(QuerySolutionNode* node) {
    if (node->getType() == STAGE_COLLSCAN) {
        auto collectionScanSolnNode = dynamic_cast<CollectionScanNode*>(node);
        return (collectionScanSolnNode->doClusteredCollectionScanClassic() ||
                collectionScanSolnNode->doClusteredCollectionScanSbe());
    }
    return false;
}

// Check if this is a real coll scan or a hidden ClusteredIDX scan.
bool isClusteredIDXScanSoln(QuerySolution* collscanSoln) {
    if (collscanSoln->root()->getType() == STAGE_SHARDING_FILTER) {
        auto child = collscanSoln->root()->children.begin();
        return isClusteredScan(child->get());
    }
    if (collscanSoln->root()->getType() == STAGE_COLLSCAN) {
        return isClusteredScan(collscanSoln->root());
    }
    return false;
}

StatusWith<std::vector<std::unique_ptr<QuerySolution>>> attemptCollectionScan(
    const CanonicalQuery& query, bool isTailable, const QueryPlannerParams& params) {
    if (auto soln = tryEofSoln(query)) {
        return singleSolution(std::move(soln));
    }
    if (noTableScan(params)) {
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
    const CanonicalQuery& query,
    const QueryPlannerParams& params,
    boost::optional<StringSet&> relevantIndexOutput) {
    LOGV2_DEBUG(20967,
                5,
                "Beginning planning",
                "options"_attr = optionString(params.mainCollectionInfo.options),
                "query"_attr = redact(query.toString()));

    if (auto scoped = queryPlannerAlwaysFails.scoped(); MONGO_unlikely(scoped.isActive())) {
        if (!scoped.getData().hasField("namespace") ||
            scoped.getData().getStringField("namespace") ==
                NamespaceStringUtil::serialize(query.nss(), SerializationContext::stateDefault())) {
            tasserted(9656400, "Hit queryPlannerAlwaysFails fail point");
        }
    }

    for (size_t i = 0; i < params.mainCollectionInfo.indexes.size(); ++i) {
        LOGV2_DEBUG(20968,
                    5,
                    "Index number and details",
                    "indexNumber"_attr = i,
                    "index"_attr = params.mainCollectionInfo.indexes[i].toString());
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
    boost::optional<BSONObj> hintedIndexBson = boost::none;
    if (!params.indexFiltersApplied && !params.querySettingsApplied) {
        if (auto hintObj = query.getFindCommandRequest().getHint(); !hintObj.isEmpty()) {
            hintedIndexBson = hintObj;
        }
    }

    // geoNear and text queries *require* an index.
    // Also, if a hint is specified it indicates that we MUST use it.
    bool mustUseIndexedPlan =
        QueryPlannerCommon::hasNode(query.getPrimaryMatchExpression(), MatchExpression::GEO_NEAR) ||
        QueryPlannerCommon::hasNode(query.getPrimaryMatchExpression(), MatchExpression::TEXT) ||
        hintedIndexBson;

    if (hintedIndexBson) {
        // If we have a hint, check if it matches any "special" index before proceeding.
        const auto& hintObj = *hintedIndexBson;
        if (const auto naturalHint = hintObj[query_request_helper::kNaturalSortField]) {
            return handleNaturalHint(query, params, naturalHint, isTailable);
        } else if (hintMatchesClusterKey(params.clusteredInfo, hintObj)) {
            return handleClusteredScanHint(query, params, isTailable);
        }
    }

    // Either the list of indices passed in by the caller, or the list of indices filtered
    // according to the hint. This list is later expanded in order to allow the planner to
    // handle wildcard indexes.
    std::vector<IndexEntry> fullIndexList;

    // Will hold a copy of the index entry chosen by the hint.
    boost::optional<IndexEntry> hintedIndexEntry;
    if (!hintedIndexBson) {
        fullIndexList = params.mainCollectionInfo.indexes;
    } else {
        fullIndexList = QueryPlannerIXSelect::findIndexesByHint(*hintedIndexBson,
                                                                params.mainCollectionInfo.indexes);

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
    RelevantFieldIndexMap fields;
    QueryPlannerIXSelect::getFields(query.getPrimaryMatchExpression(), &fields);
    for (auto&& field : fields) {
        LOGV2_DEBUG(20970, 5, "Predicate over field", "field"_attr = field.first);
    }

    fullIndexList = QueryPlannerIXSelect::expandIndexes(
        fields, std::move(fullIndexList), hintedIndexBson != boost::none);
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
            return Status(ErrorCodes::NoQueryExecutionPlans,
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

        // If the relevantIndexOutput argument is given, then we populate it with the top level
        // field names of all the keys in the set of relevantIndices.
        if (relevantIndexOutput) {
            cost_based_ranker::addFieldsToRelevantIndexOutput(relevantIndices[i].keyPattern,
                                                              relevantIndexOutput.get());
        }
    }

    // Figure out how useful each index is to each predicate.
    QueryPlannerIXSelect::QueryContext queryContext;
    queryContext.collator = query.getCollator();
    queryContext.elemMatchContext = QueryPlannerIXSelect::ElemMatchContext{};
    queryContext.mustUseIndexedPlan = mustUseIndexedPlan;
    QueryPlannerIXSelect::rateIndices(
        query.getPrimaryMatchExpression(), "", relevantIndices, queryContext);
    QueryPlannerIXSelect::stripInvalidAssignments(query.getPrimaryMatchExpression(),
                                                  relevantIndices);

    // Unless we have GEO_NEAR, TEXT, or a projection, we may be able to apply an optimization
    // in which we strip unnecessary index assignments.
    //
    // Disallowed with projection because assignment to a non-unique index can allow the plan
    // to be covered.
    //
    // TEXT and GEO_NEAR are special because they require the use of a text/geo index in order
    // to be evaluated correctly. Stripping these "mandatory assignments" is therefore invalid.
    if (query.getFindCommandRequest().getProjection().isEmpty() &&
        !QueryPlannerCommon::hasNode(query.getPrimaryMatchExpression(),
                                     MatchExpression::GEO_NEAR) &&
        !QueryPlannerCommon::hasNode(query.getPrimaryMatchExpression(), MatchExpression::TEXT)) {
        QueryPlannerIXSelect::stripUnneededAssignments(query.getPrimaryMatchExpression(),
                                                       relevantIndices);
    }

    // query.getPrimaryMatchExpression() is now annotated with RelevantTag(s).
    LOGV2_DEBUG(20972,
                5,
                "Rated tree",
                "tree"_attr = redact(query.getPrimaryMatchExpression()->debugString()));

    // If there is a GEO_NEAR it must have an index it can use directly.
    const MatchExpression* gnNode = nullptr;
    if (QueryPlannerCommon::hasNode(
            query.getPrimaryMatchExpression(), MatchExpression::GEO_NEAR, &gnNode)) {
        // No index for GEO_NEAR?  No query.
        RelevantTag* tag = static_cast<RelevantTag*>(gnNode->getTag());
        if (!tag || (0 == tag->first.size() && 0 == tag->notFirst.size())) {
            LOGV2_DEBUG(20973, 5, "Unable to find index for $geoNear query");
            // Don't leave tags on query tree.
            query.getPrimaryMatchExpression()->resetTag();
            return Status(ErrorCodes::NoQueryExecutionPlans,
                          "unable to find index for $geoNear query");
        }

        LOGV2_DEBUG(20974,
                    5,
                    "Rated tree after geonear processing",
                    "tree"_attr = redact(query.getPrimaryMatchExpression()->debugString()));
    }

    // Likewise, if there is a TEXT it must have an index it can use directly.
    const MatchExpression* textNode = nullptr;
    if (QueryPlannerCommon::hasNode(
            query.getPrimaryMatchExpression(), MatchExpression::TEXT, &textNode)) {
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
            query.getPrimaryMatchExpression()->resetTag();
            return Status(ErrorCodes::NoQueryExecutionPlans,
                          "need exactly one text index for $text query");
        }

        // Error if the text node is tagged with zero indices.
        if (0 == tag->first.size() && 0 == tag->notFirst.size()) {
            // Don't leave tags on query tree.
            query.getPrimaryMatchExpression()->resetTag();
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
                    "tree"_attr = redact(query.getPrimaryMatchExpression()->debugString()));
    }

    const bool isDistinctMultiplanningEnabled =
        query.getExpCtx()->isFeatureFlagShardFilteringDistinctScanEnabled();
    const auto& sortPattern = query.getSortPattern();
    const auto& sortRequirementForDistinct =
        query.getDistinct() ? query.getDistinct()->getSortRequirement() : boost::none;
    tassert(9261501,
            "Expected distinct multiplanning to be enabled when sortRequirementForDistinct is set",
            !sortRequirementForDistinct || isDistinctMultiplanningEnabled);

    std::vector<std::unique_ptr<QuerySolution>> out;

    // If we have any relevant indices, we try to create indexed plans.
    if (!relevantIndices.empty()) {
        // The enumerator spits out trees tagged with IndexTag(s).
        plan_enumerator::PlanEnumeratorParams enumParams;
        enumParams.intersect =
            params.mainCollectionInfo.options & QueryPlannerParams::INDEX_INTERSECTION;
        enumParams.root = query.getPrimaryMatchExpression();
        enumParams.indices = &relevantIndices;
        enumParams.enumerateOrChildrenLockstep =
            params.mainCollectionInfo.options & QueryPlannerParams::ENUMERATE_OR_CHILDREN_LOCKSTEP;
        enumParams.projection = query.getProj();
        // Ensure we don't prune indexes that could be used to satisfy the sort requirement for
        // distinct scan.
        enumParams.sort = sortPattern ? &sortPattern : &sortRequirementForDistinct;
        enumParams.shardKey = params.shardKey;
        enumParams.distinct = query.getDistinct().has_value();
        // TODO SERVER-94155: Enable index pruning for distinct-like queries when feature flag is
        // on.
        enumParams.shouldPruneDistinct =
            !query.getExpCtx()->isFeatureFlagShardFilteringDistinctScanEnabled();

        plan_enumerator::PlanEnumerator planEnumerator(enumParams);
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
            std::unique_ptr<PlanCacheIndexTree> cacheData;
            auto statusWithCacheData =
                cacheDataFromTaggedTree(nextTaggedTree.get(), relevantIndices);
            if (!statusWithCacheData.isOK()) {
                LOGV2_DEBUG(20977,
                            5,
                            "Query is not cachable",
                            "reason"_attr = redact(statusWithCacheData.getStatus().reason()));
            } else {
                cacheData = std::move(statusWithCacheData.getValue());
            }

            // We must hash the tagged MatchExpression tree before sorting it in
            // 'prepareForAccessPlanning()' to be able to distinguish some plans. E.g. {a: 1, a: 2}
            // will be sorted such that the tagged comparison is always in the same place, and since
            // both comparisons have the same type and are on the same path, {(tag)a: 1, a: 2} and
            // {(tag)a: 2, a: 1} will get the same hash when constants are ignored.
            const size_t taggedMatchExpressionHash =
                hashTaggedMatchExpression(nextTaggedTree.get(), relevantIndices);

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
                soln->taggedMatchExpressionHash = taggedMatchExpressionHash;
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
    query.getPrimaryMatchExpression()->resetTag();

    LOGV2_DEBUG(20979, 5, "Planner: outputted indexed solutions", "numSolutions"_attr = out.size());

    // Produce legible error message for failed OR planning with a TEXT child.
    // TODO: support collection scan for non-TEXT children of OR.
    if (out.size() == 0 && textNode != nullptr &&
        MatchExpression::OR == query.getPrimaryMatchExpression()->matchType()) {
        MatchExpression* root = query.getPrimaryMatchExpression();
        for (size_t i = 0; i < root->numChildren(); ++i) {
            if (textNode == root->getChild(i)) {
                return Status(ErrorCodes::NoQueryExecutionPlans,
                              "Failed to produce a solution for TEXT under OR - "
                              "other non-TEXT clauses under OR have to be indexed as well.");
            }
        }
    }

    // Past this point, if an EOF solution is _possible_, it will be
    // used regardless of sort, project, skip, or limit. We explicitly
    // do this before considering a hint. Missing the opportunity for
    // an EOF plan may result in an unbounded index scan where all
    // fetched documents are filtered out by something like
    // $alwaysFalse.
    if (auto soln = tryEofSoln(query)) {
        // A query with a trivially false primary match expression will never have any
        // results, so a simple EOF is all that is required.
        return singleSolution(std::move(soln));
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

        LOGV2_WARNING(
            2658100,
            "Hinted index could not provide a bounded scan, reverting to whole index scan",
            "hint"_attr = redact(hintedIndexBson->toString()));

        // Return hinted index solution if found.
        if (auto soln = buildWholeIXSoln(relevantIndices.front(), query, params)) {
            LOGV2_DEBUG(20980, 5, "Planner: outputting soln that uses hinted index as scan");
            return singleSolution(std::move(soln));
        }
        return Status(ErrorCodes::NoQueryExecutionPlans,
                      "Failed to build whole-index solution for $hint");
    }

    // If a sort order is requested, there may be an index that provides it, even if that
    // index is not over any predicates in the query. When planning a distinct scan query, a
    // sort might be required even when the query doesn't have an actual sort.
    if ((sortPattern || sortRequirementForDistinct) &&
        !QueryPlannerCommon::hasNode(query.getPrimaryMatchExpression(),
                                     MatchExpression::GEO_NEAR) &&
        !QueryPlannerCommon::hasNode(query.getPrimaryMatchExpression(), MatchExpression::TEXT)) {
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

        if (!usingIndexToSort || !sortPattern) {
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
                if (index.filterExpr &&
                    !expression::isSubsetOf(query.getPrimaryMatchExpression(), index.filterExpr)) {
                    continue;
                }

                auto addPlansWithIndexProvidedSort = [&](const BSONObj& kp, const int direction) {
                    const bool providesSort =
                        sortPattern && QueryPlannerCommon::providesSort(query, kp);
                    if (!providesSort &&
                        !QueryPlannerCommon::providesSortRequirementForDistinct(query.getDistinct(),
                                                                                kp)) {
                        return;
                    }

                    LOGV2_DEBUG(
                        20981, 5, "Planner: outputting soln that uses index to provide sort");
                    auto index = fullIndexList[i];
                    auto soln = buildWholeIXSoln(index, query, params, direction);
                    // If the solution was created to satisfy a sort requirement for distinct scan,
                    // ensure we have a distinct scan plan.
                    if (soln && (providesSort || soln->hasNode(STAGE_DISTINCT_SCAN))) {
                        if (relevantIndexOutput) {
                            // We generated a plan that used an index that was not over any of the
                            // predicates in the query and thus not in the list of relevantIndices.
                            // As a result, we need to add the fields of this index to the
                            // relevantIndexOutput set.
                            cost_based_ranker::addFieldsToRelevantIndexOutput(
                                index.keyPattern, relevantIndexOutput.get());
                        }
                        PlanCacheIndexTree* indexTree = new PlanCacheIndexTree();
                        indexTree->setIndexEntry(fullIndexList[i]);
                        SolutionCacheData* scd = new SolutionCacheData();
                        scd->tree.reset(indexTree);
                        scd->solnType = SolutionCacheData::WHOLE_IXSCAN_SOLN;
                        scd->wholeIXSolnDir = direction;

                        soln->cacheData.reset(scd);
                        out.push_back(std::move(soln));
                    }
                };

                const BSONObj kp = QueryPlannerAnalysis::getSortPattern(index.keyPattern);
                addPlansWithIndexProvidedSort(kp, 1);
                addPlansWithIndexProvidedSort(QueryPlannerCommon::reverseSortObj(kp), -1);
            }
        }
    }

    // If a projection exists, there may be an index that allows for a covered plan, even if
    // none were considered earlier.
    const auto projection = query.getProj();
    if (params.mainCollectionInfo.options & QueryPlannerParams::GENERATE_COVERED_IXSCANS &&
        out.size() == 0 && query.getQueryObj().isEmpty() && projection &&
        !projection->requiresDocument()) {

        const auto* indicesToConsider = hintedIndexBson ? &relevantIndices : &fullIndexList;
        for (auto&& index : *indicesToConsider) {
            if (index.type != INDEX_BTREE || index.multikey || index.sparse || index.filterExpr ||
                !CollatorInterface::collatorsMatch(index.collator, query.getCollator())) {
                continue;
            }

            auto soln = buildWholeIXSoln(
                index,
                query,
                // TODO SERVER-87683 Investigate why empty parameters are used instead of 'params'.
                QueryPlannerParams{
                    QueryPlannerParams::ArgsForTest{},
                });
            if (soln && !soln->root()->fetched()) {
                LOGV2_DEBUG(
                    20983, 5, "Planner: outputting soln that uses index to provide projection");
                if (relevantIndexOutput) {
                    // We generated a plan that used an index that was not over any of the
                    // predicates in the query and thus not in the list of relevantIndices used.
                    // As a result, we need to add the fields of this index to the
                    // relevantIndexOutput set.
                    cost_based_ranker::addFieldsToRelevantIndexOutput(index.keyPattern,
                                                                      relevantIndexOutput.get());
                }
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

    // Distinct queries can benefit from an index even without a sort or filter present. Without
    // these, the previous steps don't consider any indexed plans, so we try to generate a covered
    // distinct scan here. The direction of the index doesn't matter in this case.
    if (isDistinctMultiplanningEnabled && query.getDistinct() &&
        query.getFindCommandRequest().getFilter().isEmpty() && !sortPattern &&
        !sortRequirementForDistinct) {

        auto soln = constructCoveredDistinctScan(query, params, *query.getDistinct());
        if (soln) {
            out.push_back(std::move(soln));
        }
    }

    // Create a $search QuerySolution if we are performing a $search.
    if (out.empty()) {
        auto statusWithSoln = tryToBuildSearchQuerySolution(params, query);
        if (statusWithSoln.isOK()) {
            out.emplace_back(std::move(statusWithSoln.getValue()));
        } else {
            LOGV2_DEBUG(7816302,
                        4,
                        "Not pushing down $search into SBE",
                        "reason"_attr = statusWithSoln.getStatus());
        }
    }

    // The caller can explicitly ask for a collscan.
    bool collscanRequested =
        (params.mainCollectionInfo.options & QueryPlannerParams::INCLUDE_COLLSCAN);

    // No indexed plans?  We must provide a collscan if possible or else we can't run the query.
    bool collScanRequired = out.empty();
    if (collScanRequired && noTableAndClusteredIDXScan(params)) {
        return Status(ErrorCodes::NoQueryExecutionPlans,
                      "No indexed plans available, and running with 'notablescan'");
    }

    bool clusteredCollection = params.clusteredInfo.has_value();

    if (collScanRequired && mustUseIndexedPlan) {
        return Status(ErrorCodes::NoQueryExecutionPlans, "No query solutions");
    }

    bool isClusteredIDXScan = false;
    if (!mustUseIndexedPlan && (collscanRequested || collScanRequired || clusteredCollection) &&
        !noTableAndClusteredIDXScan(params)) {
        boost::optional<int> clusteredScanDirection =
            QueryPlannerCommon::determineClusteredScanDirection(
                query, params.clusteredInfo, params.clusteredCollectionCollator);
        int direction = clusteredScanDirection.value_or(1);
        auto collscanSoln = buildCollscanSoln(query, isTailable, params, direction);
        if (!collscanSoln && collScanRequired) {
            return Status(ErrorCodes::NoQueryExecutionPlans,
                          "Failed to build collection scan soln");
        }
        isClusteredIDXScan = isClusteredIDXScanSoln(collscanSoln.get());
        // We consider collection scan in the following cases:
        // 1. collScanRequested - specifically requested by caller.
        // 2. collScanRequired - there are no other possible plans, so we fallback to full scan.
        // 3. collscanIsBounded - collection is clustered and clustered index is used.
        // 4. clusteredScanDirection - collection is clustered and sort, provided by clustered
        // index, is used
        if (collscanSoln &&
            (collscanRequested || collScanRequired ||
             isSolutionBoundedCollscan(collscanSoln.get()) || clusteredScanDirection)) {
            LOGV2_DEBUG(20984,
                        5,
                        "Planner: outputting a collection scan",
                        "collectionScan"_attr = redact(collscanSoln->toString()));
            SolutionCacheData* scd = new SolutionCacheData();
            scd->solnType = SolutionCacheData::COLLSCAN_SOLN;
            scd->wholeIXSolnDir = direction;
            collscanSoln->cacheData.reset(scd);
            out.push_back(std::move(collscanSoln));
        }
    }
    // Make sure to respect the notablescan option. A clustered IDX scan is allowed even under a
    // NOTABLE option. Only in the case of a strict NOTABLE scan option a clustered IDX scan is not
    // allowed. This option is used in mongoS for shardPruning.
    invariant(out.size() > 0);
    if (collScanRequired && noTableScan(params) && !isClusteredIDXScan) {
        return Status(ErrorCodes::NoQueryExecutionPlans,
                      "No indexed plans available, and running with 'notablescan' 2");
    }

    // If CanonicalQuery is distinct-like and we haven't generated a plan that features
    // a DISTINCT_SCAN, we should use SBE or subplanning if possible instead.
    if (isDistinctMultiplanningEnabled && query.getDistinct()) {
        const bool canSubplan =
            MatchExpression::OR == query.getPrimaryMatchExpression()->matchType() &&
            query.getPrimaryMatchExpression()->numChildren() > 0;
        if (query.isSbeCompatible() || canSubplan) {
            const bool noDistinctScans = std::none_of(out.begin(), out.end(), [](const auto& soln) {
                return soln->hasNode(STAGE_DISTINCT_SCAN);
            });
            if (noDistinctScans) {
                return Status(ErrorCodes::NoDistinctScansForDistinctEligibleQuery,
                              "No DISTINCT_SCAN plans available");
            }
        }
    }

    // We must wait until the set of candidates in the "out" vector is
    // complete before choosing a forced plan.
    if (auto solutionHash = query.getForcedPlanSolutionHash()) {
        uassert(ErrorCodes::IllegalOperation,
                "Use of forcedPlanSolutionHash not permitted.",
                internalQueryAllowForcedPlanByHash.load());

        for (auto&& soln : out) {
            if ((int64_t)soln->hash() == *solutionHash) {
                LOGV2_DEBUG(10872500,
                            5,
                            "Forced plan by solution hash",
                            "solution"_attr = redact(soln->toString()));
                return singleSolution(std::move(soln));
            }
        }
        return Status(ErrorCodes::NoQueryExecutionPlans,
                      "Forced plan solution hash not present in candidate plan set.");
    }

    return {std::move(out)};
}  // QueryPlanner::plan

StatusWith<QueryPlanner::CostBasedRankerResult> QueryPlanner::planWithCostBasedRanking(
    const CanonicalQuery& query,
    const QueryPlannerParams& params,
    ce::SamplingEstimator* samplingEstimator,
    const ce::ExactCardinalityEstimator* exactCardinality,
    StatusWith<std::vector<std::unique_ptr<QuerySolution>>> statusWithMultiPlanSolns) {
    using namespace cost_based_ranker;
    auto cbrMode = params.planRankerMode;
    EstimateMap estimates;
    const auto& collInfo = params.mainCollectionInfo;
    tassert(9969001, "CBR received incomplete catalog statistics", collInfo.collStats != nullptr);
    CardinalityEstimator cardEstimator(collInfo, samplingEstimator, estimates, cbrMode);
    CostEstimator costEstimator(estimates);

    std::vector<std::unique_ptr<QuerySolution>> allSoln =
        std::move(statusWithMultiPlanSolns.getValue());
    // This set of plans contains the optimal plan. If 'acceptedSoln' contains a single
    // plan, then that is the optimal (winning) plan chosen by the planner. Otherwise, if there is
    // more than one plan, those plans are sent to the multi-planner to choose the winning plan.
    std::vector<std::unique_ptr<QuerySolution>> acceptedSoln;
    // The set of plans that definitely do not contain the optimal plan. This set is used for
    // explain to show all rejected plans.
    std::vector<std::unique_ptr<QuerySolution>> rejectedSoln;

    CostEstimate bestCost = maxCost;
    std::unique_ptr<QuerySolution> bestSoln;
    for (auto&& soln : allSoln) {
        const auto& ceRes = cbrMode == QueryPlanRankerModeEnum::kExactCE
            ? exactCardinality->calculateExactCardinality(*soln, estimates)
            : cardEstimator.estimatePlan(*soln);
        if (!ceRes.isOK()) {
            // This plan's cardinality cannot be estimated.
            if (cbrMode == QueryPlanRankerModeEnum::kAutomaticCE ||
                ceRes.getStatus().code() == ErrorCodes::UnsupportedCbrNode) {
                // We'll fallback to multi-planning for an inestimable plan if either:
                // * We are in automatic CE mode
                // * The reason for the inestimable plan was an unsupported node
                acceptedSoln.push_back(std::move(soln));
            } else {
                // All other CE modes are considered "strict", that is, when a CE method couldn't
                // be applied, this is considered a user error.
                return ceRes.getStatus();
            }
        } else {
            CostEstimate curCost = costEstimator.estimatePlan(*soln);
            // Note that the cost comparison operators used here are approximate within some
            // epsilon as implemented by the overloaded comparisons for estimates.
            if (curCost < bestCost) {
                if (bestSoln) {
                    rejectedSoln.push_back(std::move(bestSoln));
                }
                bestSoln = std::move(soln);
                bestCost = curCost;
            } else {
                // TODO SERVER-97933: handle equal cost plans in a deterministic way
                // For now, we pick one and put the other in rejected plans.
                rejectedSoln.push_back(std::move(soln));
            }
        }
    }
    if (bestSoln) {
        acceptedSoln.push_back(std::move(bestSoln));
    }
    if (acceptedSoln.size() > 1) {
        // Put the plan with lowest cost (among the estimated plans) first.
        std::swap(acceptedSoln.front(), acceptedSoln.back());
    }
    tassert(9751901,
            "Some plan has fallen into the gray zone between accepted and rejected QSNs.",
            acceptedSoln.size() + rejectedSoln.size() == allSoln.size());

    return QueryPlanner::CostBasedRankerResult{.solutions = std::move(acceptedSoln),
                                               .rejectedPlans = std::move(rejectedSoln),
                                               .estimates = std::move(estimates)};
}

/**
 * If 'query.cqPipeline()' is non-empty, it contains a prefix of the aggregation pipeline that can
 * be pushed down to SBE. For now, we plan this separately here and attach the agg portion of the
 * plan to the solution(s) via the extendWith() call near the end.
 */
std::unique_ptr<QuerySolution> QueryPlanner::extendWithAggPipeline(
    CanonicalQuery& query,
    std::unique_ptr<QuerySolution>&& solution,
    const std::map<NamespaceString, CollectionInfo>& secondaryCollInfos) {
    if (query.cqPipeline().empty()) {
        return std::move(solution);
    }

    std::unique_ptr<QuerySolutionNode> solnForAgg = std::make_unique<SentinelNode>();
    const std::vector<boost::intrusive_ptr<DocumentSource>>& innerPipelineStages =
        query.cqPipeline();
    for (size_t i = 0; i < innerPipelineStages.size(); ++i) {
        bool isLastSource =
            (i + 1 == (innerPipelineStages.size())) && query.containsEntirePipeline();
        const auto innerStage = innerPipelineStages[i].get();

        auto groupStage = dynamic_cast<DocumentSourceGroup*>(innerStage);
        if (groupStage) {
            solnForAgg = std::make_unique<GroupNode>(std::move(solnForAgg),
                                                     groupStage->getIdExpression(),
                                                     groupStage->getAccumulationStatements(),
                                                     groupStage->doingMerge(),
                                                     groupStage->willBeMerged(),
                                                     isLastSource /* shouldProduceBson */);
            continue;
        }

        auto lookupStage = dynamic_cast<DocumentSourceLookUp*>(innerStage);
        if (lookupStage) {
            tassert(6369000,
                    "This $lookup stage should be compatible with SBE",
                    lookupStage->sbeCompatibility() != SbeCompatibility::notCompatible);
            auto [strategy, idxEntry, scanDirection] =
                QueryPlannerAnalysis::determineLookupStrategy(
                    lookupStage->getFromNs(),
                    lookupStage->getForeignField()->fullPath(),
                    secondaryCollInfos,
                    query.getExpCtx()->getAllowDiskUse(),
                    query.getCollator());

            if (!lookupStage->hasUnwindSrc()) {
                solnForAgg =
                    std::make_unique<EqLookupNode>(std::move(solnForAgg),
                                                   lookupStage->getFromNs(),
                                                   lookupStage->getLocalField()->fullPath(),
                                                   lookupStage->getForeignField()->fullPath(),
                                                   lookupStage->getAsField().fullPath(),
                                                   strategy,
                                                   std::move(idxEntry),
                                                   isLastSource /* shouldProduceBson */,
                                                   scanDirection);
            } else {
                const boost::intrusive_ptr<DocumentSourceUnwind>& unwindSrc =
                    lookupStage->getUnwindSource();
                solnForAgg =
                    std::make_unique<EqLookupUnwindNode>(std::move(solnForAgg),
                                                         // Shared data members.
                                                         lookupStage->getAsField().fullPath(),
                                                         // $lookup-specific data members.
                                                         lookupStage->getFromNs(),
                                                         lookupStage->getLocalField()->fullPath(),
                                                         lookupStage->getForeignField()->fullPath(),
                                                         strategy,
                                                         std::move(idxEntry),
                                                         isLastSource /* shouldProduceBson */,
                                                         // $unwind-specific data members.
                                                         unwindSrc->preserveNullAndEmptyArrays(),
                                                         unwindSrc->indexPath(),
                                                         scanDirection);
            }
            continue;
        }

        // 'projectionStage' pushdown pushes both $project and $addFields to SBE, as the latter is
        // implemented as a variant of the former.
        auto projectionStage = dynamic_cast<DocumentSourceInternalProjection*>(innerStage);
        if (projectionStage) {
            solnForAgg = std::make_unique<ProjectionNodeDefault>(
                std::move(solnForAgg), nullptr, projectionStage->projection());
            continue;
        }

        auto unwindStage = dynamic_cast<DocumentSourceUnwind*>(innerStage);
        if (unwindStage) {
            solnForAgg = std::make_unique<UnwindNode>(
                std::move(solnForAgg) /* child */,
                UnwindNode::UnwindSpec{unwindStage->getUnwindPath() /* fieldPath */,
                                       unwindStage->preserveNullAndEmptyArrays(),
                                       unwindStage->indexPath()});
            continue;
        }

        auto replaceRootStage = dynamic_cast<DocumentSourceInternalReplaceRoot*>(innerStage);
        if (replaceRootStage) {
            solnForAgg = std::make_unique<ReplaceRootNode>(std::move(solnForAgg),
                                                           replaceRootStage->newRootExpression());
            continue;
        }

        auto matchStage = dynamic_cast<DocumentSourceMatch*>(innerStage);
        if (matchStage) {
            solnForAgg = std::make_unique<MatchNode>(std::move(solnForAgg),
                                                     matchStage->getMatchExpression()->clone());
            continue;
        }

        auto sortStage = dynamic_cast<DocumentSourceSort*>(innerStage);
        if (sortStage) {
            auto pattern =
                sortStage->getSortKeyPattern()
                    .serialize(SortPattern::SortKeySerialization::kForPipelineSerialization)
                    .toBson();
            auto limit = sortStage->getLimit().get_value_or(0);
            solnForAgg = std::make_unique<SortNodeDefault>(std::move(solnForAgg),
                                                           std::move(pattern),
                                                           limit,
                                                           LimitSkipParameterization::Disabled);
            continue;
        }

        auto limitStage = dynamic_cast<DocumentSourceLimit*>(innerStage);
        if (limitStage) {
            solnForAgg = std::make_unique<LimitNode>(
                std::move(solnForAgg), limitStage->getLimit(), LimitSkipParameterization::Disabled);
            continue;
        }

        auto skipStage = dynamic_cast<DocumentSourceSkip*>(innerStage);
        if (skipStage) {
            solnForAgg = std::make_unique<SkipNode>(
                std::move(solnForAgg), skipStage->getSkip(), LimitSkipParameterization::Disabled);
            continue;
        }

        auto isSearch = search_helpers::isSearchStage(innerStage);
        auto isSearchMeta = search_helpers::isSearchMetaStage(innerStage);
        if (isSearch || isSearchMeta) {
            // In the $search case, we create the $search query solution node in QueryPlanner::Plan
            // instead of here. The empty branch here assures that we don't hit the tassert below
            // and continue in creating the query plan.
            continue;
        }

        auto windowStage = dynamic_cast<DocumentSourceInternalSetWindowFields*>(innerStage);
        if (windowStage) {
            auto windowNode = std::make_unique<WindowNode>(std::move(solnForAgg),
                                                           windowStage->getPartitionBy(),
                                                           windowStage->getSortBy(),
                                                           windowStage->getOutputFields());
            solnForAgg = std::move(windowNode);
            continue;
        }

        auto unpackBucketStage = dynamic_cast<DocumentSourceInternalUnpackBucket*>(innerStage);
        if (unpackBucketStage) {
            const auto& unpacker = unpackBucketStage->bucketUnpacker();

            auto eventFilter = unpackBucketStage->eventFilter()
                ? unpackBucketStage->eventFilter()->clone()
                : nullptr;
            auto wholeBucketFilter = unpackBucketStage->wholeBucketFilter()
                ? unpackBucketStage->wholeBucketFilter()->clone()
                : nullptr;
            solnForAgg = std::make_unique<UnpackTsBucketNode>(std::move(solnForAgg),
                                                              unpacker.bucketSpec(),
                                                              std::move(eventFilter),
                                                              std::move(wholeBucketFilter),
                                                              unpacker.includeMetaField());
            continue;
        }

        tasserted(5842400, "Pipeline contains unsupported stage for SBE pushdown");
    }

    solution->extendWith(std::move(solnForAgg));
    solution = QueryPlannerAnalysis::removeInclusionProjectionBelowGroup(std::move(solution));

    return std::move(solution);
}  // QueryPlanner::extendWithAggPipeline

StatusWith<std::unique_ptr<QuerySolution>> QueryPlanner::choosePlanForSubqueries(
    const CanonicalQuery& query,
    const QueryPlannerParams& params,
    QueryPlanner::SubqueriesPlanningResult planningResult,
    std::function<StatusWith<std::unique_ptr<QuerySolution>>(
        CanonicalQuery* cq, std::vector<unique_ptr<QuerySolution>>)> multiplanCallback) {
    for (size_t i = 0; i < planningResult.orExpression->numChildren(); ++i) {
        auto orChild = planningResult.orExpression->getChild(i);
        auto branchResult = planningResult.branches[i].get();

        if (branchResult->cachedData.get()) {
            // We can get the index tags we need out of the cache.
            Status tagStatus = tagOrChildAccordingToCache(
                branchResult->cachedData.get(), orChild, planningResult.indexMap);
            if (!tagStatus.isOK()) {
                return tagStatus;
            }
        } else if (1 == branchResult->solutions.size()) {
            QuerySolution* soln = branchResult->solutions.front().get();
            Status tagStatus =
                tagOrChildAccordingToCache(soln->cacheData.get(), orChild, planningResult.indexMap);

            // Check if 'soln' is a CLUSTERED_IXSCAN. This branch won't be tagged, and 'tagStatus'
            // will return 'NoQueryExecutionPlans'. However, this plan can be executed by the OR
            // stage.
            QuerySolutionNode* root = soln->root();
            if (!tagStatus.isOK()) {
                const bool allowPlanWithoutTag = tagStatus == ErrorCodes::NoQueryExecutionPlans &&
                    canUseClusteredCollScan(root, std::move(root->children));
                if (!allowPlanWithoutTag) {
                    return tagStatus;
                }
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

            // The cached plan might be an indexed solution or a clustered collection scan.
            SolutionCacheData::SolutionType solnType = bestSoln->cacheData->solnType;
            bool useClusteredCollScan = false;
            if (SolutionCacheData::USE_INDEX_TAGS_SOLN != solnType) {
                if (!(SolutionCacheData::COLLSCAN_SOLN == solnType &&
                      canUseClusteredCollScan(bestSoln->root(),
                                              std::move(bestSoln->root()->children)))) {
                    str::stream ss;
                    ss << "No indexed cache data for subchild " << orChild->debugString();
                    return Status(ErrorCodes::NoQueryExecutionPlans, ss);
                } else {
                    useClusteredCollScan = true;
                }
            }

            // If the cached plan is not a clustered collection scan, add the index assignments to
            // the original query.
            if (!useClusteredCollScan) {
                Status tagStatus = QueryPlanner::tagAccordingToCache(
                    orChild, bestSoln->cacheData->tree.get(), planningResult.indexMap);
                if (!tagStatus.isOK()) {
                    str::stream ss;
                    ss << "Failed to extract indices from subchild " << orChild->debugString();
                    return tagStatus.withContext(ss);
                }
            }
        }
    }

    // We must hash the tagged MatchExpression tree before sorting it in
    // 'prepareForAccessPlanning()' to be able to distinguish some plans.
    const size_t taggedMatchExpressionHash = hashTaggedMatchExpression(
        planningResult.orExpression.get(), params.mainCollectionInfo.indexes);

    // Must do this before using the planner functionality.
    prepareForAccessPlanning(planningResult.orExpression.get());

    // Use the cached index assignments to build solnRoot. Takes ownership of '_orExpression'.
    std::unique_ptr<QuerySolutionNode> solnRoot(QueryPlannerAccess::buildIndexedDataAccess(
        query, std::move(planningResult.orExpression), params.mainCollectionInfo.indexes, params));

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

    compositeSolution->taggedMatchExpressionHash = taggedMatchExpressionHash;

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
    const QueryPlannerParams& params,
    ce::SamplingEstimator* samplingEstimator,
    const ce::ExactCardinalityEstimator* exactCardinality,
    boost::optional<StringSet&> topLevelSampleFieldNames) {
    invariant(query.getPrimaryMatchExpression()->matchType() == MatchExpression::OR);
    invariant(query.getPrimaryMatchExpression()->numChildren(),
              "Cannot plan subqueries for an $or with no children");

    SubqueriesPlanningResult planningResult{query.getPrimaryMatchExpression()->clone()};
    for (size_t i = 0; i < params.mainCollectionInfo.indexes.size(); ++i) {
        const IndexEntry& ie = params.mainCollectionInfo.indexes[i];
        const auto insertionRes = planningResult.indexMap.insert(std::make_pair(ie.identifier, i));
        // Be sure the key was not already in the map.
        invariant(insertionRes.second);
        LOGV2_DEBUG(20598,
                    5,
                    "Subplanner: index number and entry",
                    "indexNumber"_attr = ie,
                    "indexEntry"_attr = i);
    }

    for (size_t i = 0; i < planningResult.orExpression->numChildren(); ++i) {
        // We need a place to shove the results from planning this branch.
        planningResult.branches.push_back(
            std::make_unique<SubqueriesPlanningResult::BranchPlanningResult>());
        auto branchResult = planningResult.branches.back().get();

        // Turn the i-th child into its own query.
        auto statusWithCQ = CanonicalQuery::makeForSubplanner(opCtx, query, i);
        if (!statusWithCQ.isOK()) {
            str::stream ss;
            ss << "Can't canonicalize subchild "
               << planningResult.orExpression->getChild(i)->debugString() << " "
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
            LOGV2_DEBUG(20599,
                        5,
                        "Subplanner: cached plan found",
                        "childIndex"_attr = i,
                        "numChildren"_attr = planningResult.orExpression->numChildren());
        } else {
            // No CachedSolution found. We'll have to plan from scratch.
            LOGV2_DEBUG(20600,
                        5,
                        "Subplanner: planning child",
                        "childIndex"_attr = i,
                        "numChildren"_attr = planningResult.orExpression->numChildren());

            // We don't set NO_TABLE_SCAN because peeking at the cache data will keep us from
            // considering any plan that's a collscan.
            invariant(branchResult->solutions.empty());

            auto statusWithMultiPlanSolns = samplingEstimator
                ? QueryPlanner::plan(
                      *branchResult->canonicalQuery, params, topLevelSampleFieldNames)
                : QueryPlanner::plan(*branchResult->canonicalQuery, params);

            if (!statusWithMultiPlanSolns.isOK()) {
                str::stream ss;
                ss << "Can't plan for subchild " << branchResult->canonicalQuery->toString() << " "
                   << statusWithMultiPlanSolns.getStatus().reason();
                return Status(ErrorCodes::BadValue, ss);
            }
            branchResult->solutions = std::move(statusWithMultiPlanSolns.getValue());

            LOGV2_DEBUG(20601,
                        5,
                        "Subplanner: number of solutions",
                        "numSolutions"_attr = branchResult->solutions.size());
        }
    }

    return std::move(planningResult);
}
}  // namespace mongo
