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

#include "mongo/s/shard_key_pattern_query_util.h"

#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/matcher/path_internal.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/update/path_support.h"
#include "mongo/logv2/log.h"
#include "mongo/util/transitional_tools_do_not_use/vector_spooling.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

using pathsupport::EqualityMatches;

// Maximum number of intervals produced by $in queries
constexpr size_t kMaxFlattenedInCombinations = 4000000;

IndexBounds collapseQuerySolution(const QuerySolutionNode* node) {
    if (node->children.empty()) {
        invariant(node->getType() == STAGE_IXSCAN);

        const IndexScanNode* ixNode = static_cast<const IndexScanNode*>(node);
        return ixNode->bounds;
    }

    if (node->children.size() == 1) {
        // e.g. FETCH -> IXSCAN
        return collapseQuerySolution(node->children.front().get());
    }

    // children.size() > 1, assert it's OR / SORT_MERGE.
    if (node->getType() != STAGE_OR && node->getType() != STAGE_SORT_MERGE) {
        // Unexpected node. We should never reach here.
        LOGV2_ERROR(23833,
                    "could not generate index bounds on query solution tree: {node}",
                    "node"_attr = redact(node->toString()));
        dassert(false);  // We'd like to know this error in testing.

        // Bail out with all shards in production, since this isn't a fatal error.
        return IndexBounds();
    }

    IndexBounds bounds;

    for (auto it = node->children.begin(); it != node->children.end(); it++) {
        // The first branch under OR
        if (it == node->children.begin()) {
            invariant(bounds.size() == 0);
            bounds = collapseQuerySolution(it->get());
            if (bounds.size() == 0) {  // Got unexpected node in query solution tree
                return IndexBounds();
            }
            continue;
        }

        IndexBounds childBounds = collapseQuerySolution(it->get());
        if (childBounds.size() == 0) {
            // Got unexpected node in query solution tree
            return IndexBounds();
        }

        invariant(childBounds.size() == bounds.size());

        for (size_t i = 0; i < bounds.size(); i++) {
            bounds.fields[i].intervals.insert(bounds.fields[i].intervals.end(),
                                              childBounds.fields[i].intervals.begin(),
                                              childBounds.fields[i].intervals.end());
        }
    }

    for (size_t i = 0; i < bounds.size(); i++) {
        IndexBoundsBuilder::unionize(&bounds.fields[i]);
    }

    return bounds;
}

BSONElement extractKeyElementFromDoc(const BSONObj& obj, StringData pathStr) {
    // Any arrays found get immediately returned. We are equipped up the call stack to specifically
    // deal with array values.
    size_t idxPath;
    return getFieldDottedOrArray(obj, FieldRef(pathStr), &idxPath);
}

BSONElement findEqualityElement(const EqualityMatches& equalities, const FieldRef& path) {
    int parentPathPart;
    const BSONElement parentEl =
        pathsupport::findParentEqualityElement(equalities, path, &parentPathPart);

    if (parentPathPart == static_cast<int>(path.numParts()))
        return parentEl;

    if (parentEl.type() != Object)
        return BSONElement();

    StringData suffixStr = path.dottedSubstring(parentPathPart, path.numParts());
    return extractKeyElementFromDoc(parentEl.Obj(), suffixStr);
}

}  // namespace

StatusWith<BSONObj> extractShardKeyFromBasicQuery(OperationContext* opCtx,
                                                  const NamespaceString& nss,
                                                  const ShardKeyPattern& shardKeyPattern,
                                                  const BSONObj& basicQuery) {
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(basicQuery.getOwned());

    const boost::intrusive_ptr<ExpressionContext> expCtx;
    auto statusWithCQ =
        CanonicalQuery::canonicalize(opCtx,
                                     std::move(findCommand),
                                     false, /* isExplain */
                                     expCtx,
                                     ExtensionsCallbackNoop(),
                                     MatchExpressionParser::kAllowAllSpecialFeatures);
    if (!statusWithCQ.isOK()) {
        return statusWithCQ.getStatus();
    }

    return extractShardKeyFromQuery(shardKeyPattern, *statusWithCQ.getValue());
}

StatusWith<BSONObj> extractShardKeyFromBasicQueryWithContext(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const ShardKeyPattern& shardKeyPattern,
    const BSONObj& basicQuery) {
    auto findCommand = std::make_unique<FindCommandRequest>(expCtx->ns);
    findCommand->setFilter(basicQuery.getOwned());
    if (!expCtx->getCollatorBSON().isEmpty()) {
        findCommand->setCollation(expCtx->getCollatorBSON().getOwned());
    }

    auto statusWithCQ =
        CanonicalQuery::canonicalize(expCtx->opCtx,
                                     std::move(findCommand),
                                     false, /* isExplain */
                                     expCtx,
                                     ExtensionsCallbackNoop(),
                                     MatchExpressionParser::kAllowAllSpecialFeatures);
    if (!statusWithCQ.isOK()) {
        return statusWithCQ.getStatus();
    }

    return extractShardKeyFromQuery(shardKeyPattern, *statusWithCQ.getValue());
}

BSONObj extractShardKeyFromQuery(const ShardKeyPattern& shardKeyPattern,
                                 const CanonicalQuery& query) {
    // Extract equalities from query.
    EqualityMatches equalities;
    // TODO: Build the path set initially?
    FieldRefSet keyPatternPathSet(
        transitional_tools_do_not_use::unspool_vector(shardKeyPattern.getKeyPatternFields()));
    // We only care about extracting the full key pattern paths - if they don't exist (or are
    // conflicting), we don't contain the shard key.
    Status eqStatus =
        pathsupport::extractFullEqualityMatches(*query.root(), keyPatternPathSet, &equalities);
    // NOTE: Failure to extract equality matches just means we return no shard key - it's not
    // an error we propagate
    if (!eqStatus.isOK())
        return BSONObj();

    // Extract key from equalities
    // NOTE: The method below is equivalent to constructing a BSONObj and running
    // extractShardKeyFromDoc, but doesn't require creating the doc.

    BSONObjBuilder keyBuilder;
    // Iterate the parsed paths to avoid re-parsing
    for (auto it = shardKeyPattern.getKeyPatternFields().begin();
         it != shardKeyPattern.getKeyPatternFields().end();
         ++it) {
        const FieldRef& patternPath = **it;
        BSONElement equalEl = findEqualityElement(equalities, patternPath);

        if (!ShardKeyPattern::isValidShardKeyElementForStorage(equalEl))
            return BSONObj();

        if (shardKeyPattern.getHashedField() &&
            shardKeyPattern.getHashedField().fieldNameStringData() == patternPath.dottedField()) {
            keyBuilder.append(
                patternPath.dottedField(),
                BSONElementHasher::hash64(equalEl, BSONElementHasher::DEFAULT_HASH_SEED));
        } else {
            // NOTE: The equal element may *not* have the same field name as the path - nested $and,
            // $eq, for example
            keyBuilder.appendAs(equalEl, patternPath.dottedField());
        }
    }

    dassert(shardKeyPattern.isShardKey(keyBuilder.asTempObj()));
    return keyBuilder.obj();
}

BoundList flattenBounds(const ShardKeyPattern& shardKeyPattern, const IndexBounds& indexBounds) {
    invariant(indexBounds.fields.size() == (size_t)shardKeyPattern.toBSON().nFields());

    // If any field is unsatisfied, return empty bound list.
    for (const auto& field : indexBounds.fields) {
        if (field.intervals.empty()) {
            return BoundList();
        }
    }

    // To construct our bounds we will generate intervals based on bounds for the first field, then
    // compound intervals based on constraints for the first 2 fields, then compound intervals for
    // the first 3 fields, etc.
    //
    // As we loop through the fields, we start generating new intervals that will later get extended
    // in another iteration of the loop. We define these partially constructed intervals using pairs
    // of BSONObjBuilders (shared_ptrs, since after one iteration of the loop they still must exist
    // outside their scope).
    using BoundBuilders = std::vector<std::pair<BSONObjBuilder, BSONObjBuilder>>;

    BoundBuilders builders;
    builders.emplace_back();

    BSONObjIterator keyIter(shardKeyPattern.toBSON());
    // Until equalityOnly is false, we are just dealing with equality (no range or $in queries).
    bool equalityOnly = true;

    for (size_t i = 0; i < indexBounds.fields.size(); ++i) {
        BSONElement e = keyIter.next();

        StringData fieldName = e.fieldNameStringData();

        // Get the relevant intervals for this field, but we may have to transform the list of
        // what's relevant according to the expression for this field
        const OrderedIntervalList& oil = indexBounds.fields[i];
        const auto& intervals = oil.intervals;

        if (equalityOnly) {
            if (intervals.size() == 1 && intervals.front().isPoint()) {
                // This field is only a single point-interval
                for (auto& builder : builders) {
                    builder.first.appendAs(intervals.front().start, fieldName);
                    builder.second.appendAs(intervals.front().end, fieldName);
                }
            } else {
                // This clause is the first to generate more than a single point. We only execute
                // this clause once. After that, we simplify the bound extensions to prevent
                // combinatorial explosion.
                equalityOnly = false;

                BoundBuilders newBuilders;

                for (auto& builder : builders) {
                    BSONObj first = builder.first.obj();
                    BSONObj second = builder.second.obj();

                    for (const auto& interval : intervals) {
                        uassert(17439,
                                "combinatorial limit of $in partitioning of results exceeded",
                                newBuilders.size() < kMaxFlattenedInCombinations);

                        newBuilders.emplace_back();

                        newBuilders.back().first.appendElements(first);
                        newBuilders.back().first.appendAs(interval.start, fieldName);

                        newBuilders.back().second.appendElements(second);
                        newBuilders.back().second.appendAs(interval.end, fieldName);
                    }
                }

                builders = std::move(newBuilders);
            }
        } else {
            // If we've already generated a range or multiple point-intervals just extend what we've
            // generated with min/max bounds for this field
            for (auto& builder : builders) {
                builder.first.appendAs(intervals.front().start, fieldName);
                builder.second.appendAs(intervals.back().end, fieldName);
            }
        }
    }

    BoundList ret;
    for (auto& builder : builders) {
        ret.emplace_back(builder.first.obj(), builder.second.obj());
    }

    return ret;
}

IndexBounds getIndexBoundsForQuery(const BSONObj& key, const CanonicalQuery& canonicalQuery) {
    // $text is not allowed in planning since we don't have text index on mongos.
    // TODO: Treat $text query as a no-op in planning on mongos. So with shard key {a: 1},
    //       the query { a: 2, $text: { ... } } will only target to {a: 2}.
    if (QueryPlannerCommon::hasNode(canonicalQuery.root(), MatchExpression::TEXT)) {
        IndexBounds bounds;
        IndexBoundsBuilder::allValuesBounds(key, &bounds, false);  // [minKey, maxKey]
        return bounds;
    }

    // Similarly, ignore GEO_NEAR queries in planning, since we do not have geo indexes on mongos.
    if (QueryPlannerCommon::hasNode(canonicalQuery.root(), MatchExpression::GEO_NEAR)) {
        // If the GEO_NEAR predicate is a child of AND, remove the GEO_NEAR and continue building
        // bounds. Currently a CanonicalQuery can have at most one GEO_NEAR expression, and only at
        // the top-level, so this check is sufficient.
        auto geoIdx = [](auto root) -> boost::optional<size_t> {
            if (root->matchType() == MatchExpression::AND) {
                for (size_t i = 0; i < root->numChildren(); ++i) {
                    if (MatchExpression::GEO_NEAR == root->getChild(i)->matchType()) {
                        return boost::make_optional(i);
                    }
                }
            }
            return boost::none;
        }(canonicalQuery.root());

        if (!geoIdx) {
            IndexBounds bounds;
            IndexBoundsBuilder::allValuesBounds(key, &bounds, false);
            return bounds;
        }

        canonicalQuery.root()->getChildVector()->erase(
            canonicalQuery.root()->getChildVector()->begin() + geoIdx.value());
    }

    // Consider shard key as an index
    std::string accessMethod = IndexNames::findPluginName(key);
    dassert(accessMethod == IndexNames::BTREE || accessMethod == IndexNames::HASHED);
    const auto indexType = IndexNames::nameToType(accessMethod);

    // Use query framework to generate index bounds
    QueryPlannerParams plannerParams;
    // Must use "shard key" index
    plannerParams.options = QueryPlannerParams::NO_TABLE_SCAN;
    IndexEntry indexEntry(key,
                          indexType,
                          IndexDescriptor::kLatestIndexVersion,
                          // The shard key index cannot be multikey.
                          false,
                          // Empty multikey paths, since the shard key index cannot be multikey.
                          MultikeyPaths{},
                          // Empty multikey path set, since the shard key index cannot be multikey.
                          {},
                          false /* sparse */,
                          false /* unique */,
                          IndexEntry::Identifier{"shardkey"},
                          nullptr /* filterExpr */,
                          BSONObj(),
                          nullptr, /* collator */
                          nullptr /* projExec */);
    plannerParams.indices.push_back(std::move(indexEntry));

    auto statusWithMultiPlanSolns = QueryPlanner::plan(canonicalQuery, plannerParams);
    if (statusWithMultiPlanSolns.getStatus().code() != ErrorCodes::NoQueryExecutionPlans) {
        auto solutions = uassertStatusOK(std::move(statusWithMultiPlanSolns));

        // Pick any solution that has non-trivial IndexBounds. bounds.size() == 0 represents a
        // trivial IndexBounds where none of the fields' values are bounded.
        for (auto&& soln : solutions) {
            IndexBounds bounds = collapseQuerySolution(soln->root());
            if (bounds.size() > 0) {
                return bounds;
            }
        }
    }

    // We cannot plan the query without collection scan, so target to all shards.
    IndexBounds bounds;
    IndexBoundsBuilder::allValuesBounds(key, &bounds, false);  // [minKey, maxKey]
    return bounds;
}

void getShardIdsForQuery(boost::intrusive_ptr<ExpressionContext> expCtx,
                         const BSONObj& query,
                         const BSONObj& collation,
                         const ChunkManager& cm,
                         std::set<ShardId>* shardIds,
                         std::set<ChunkRange>* chunkRanges,
                         bool* targetMinKeyToMaxKey) {
    if (chunkRanges) {
        invariant(chunkRanges->empty());
    }

    auto findCommand = std::make_unique<FindCommandRequest>(cm.getNss());
    findCommand->setFilter(query.getOwned());

    expCtx->uuid = cm.getUUID();

    if (!collation.isEmpty()) {
        findCommand->setCollation(collation.getOwned());
    } else if (cm.getDefaultCollator()) {
        auto defaultCollator = cm.getDefaultCollator();
        findCommand->setCollation(defaultCollator->getSpec().toBSON());
        expCtx->setCollator(defaultCollator->clone());
    }

    auto cq = uassertStatusOK(
        CanonicalQuery::canonicalize(expCtx->opCtx,
                                     std::move(findCommand),
                                     false, /* isExplain */
                                     expCtx,
                                     ExtensionsCallbackNoop(),
                                     MatchExpressionParser::kAllowAllSpecialFeatures));

    // Fast path for targeting equalities on the shard key.
    auto shardKeyToFind = extractShardKeyFromQuery(cm.getShardKeyPattern(), *cq);
    if (!shardKeyToFind.isEmpty()) {
        try {
            auto chunk = cm.findIntersectingChunk(shardKeyToFind, collation);
            shardIds->insert(chunk.getShardId());
            if (chunkRanges) {
                chunkRanges->insert(chunk.getRange());
            }
            if (targetMinKeyToMaxKey) {
                *targetMinKeyToMaxKey = false;
            }
            return;
        } catch (const DBException&) {
            // The query uses multiple shards
        }
    }

    // Transforms query into bounds for each field in the shard key
    // for example :
    //   Key { a: 1, b: 1 },
    //   Query { a : { $gte : 1, $lt : 2 },
    //            b : { $gte : 3, $lt : 4 } }
    //   => Bounds { a : [1, 2), b : [3, 4) }
    IndexBounds bounds = getIndexBoundsForQuery(cm.getShardKeyPattern().toBSON(), *cq);

    // Transforms bounds for each shard key field into full shard key ranges
    // for example :
    //   Key { a : 1, b : 1 }
    //   Bounds { a : [1, 2), b : [3, 4) }
    //   => Ranges { a : 1, b : 3 } => { a : 2, b : 4 }
    BoundList ranges = flattenBounds(cm.getShardKeyPattern(), bounds);

    for (BoundList::const_iterator it = ranges.begin(); it != ranges.end(); ++it) {
        const auto& min = it->first;
        const auto& max = it->second;

        cm.getShardIdsForRange(min, max, shardIds, chunkRanges);

        if (targetMinKeyToMaxKey && ChunkMap::allElementsAreOfType(MinKey, min) &&
            ChunkMap::allElementsAreOfType(MaxKey, max)) {
            *targetMinKeyToMaxKey = true;
        }

        // Once we know we need to visit all shards no need to keep looping.
        // However, this optimization does not apply when we are reading from a snapshot
        // because _shardVersions contains shards with chunks and is built based on the last
        // refresh. Therefore, it is possible for _shardVersions to have fewer entries if a shard
        // no longer owns chunks when it used to at _clusterTime.
        if (!cm.isAtPointInTime() && shardIds->size() == cm.getNShardsOwningChunks()) {
            break;
        }
    }

    // SERVER-4914 Some clients of getShardIdsForQuery() assume at least one shard will be returned.
    // For now, we satisfy that assumption by adding a shard with no matches rather than returning
    // an empty set of shards.
    if (shardIds->empty()) {
        cm.forEachChunk([&](const Chunk& chunk) {
            shardIds->insert(chunk.getShardId());
            if (chunkRanges) {
                chunkRanges->insert(chunk.getRange());
            }
            if (targetMinKeyToMaxKey) {
                *targetMinKeyToMaxKey = false;
            }
            return false;
        });
    }
}

}  // namespace mongo
