// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/optimizer/join/index_fingerprint.h"

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/compiler/dependency_analysis/expression_dependencies.h"
#include "mongo/db/query/compiler/logical_model/projection/projection.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <functional>
#include <string>

#include <boost/container_hash/hash.hpp>

namespace mongo::join_ordering {
namespace {

size_t hashBSONObj(const BSONObj& obj) {
    return SimpleBSONObjComparator::kInstance.hash(obj);
}

/**
 * Collects every field path referenced anywhere in 'expr' into 'out'.
 *
 * Deliberately not 'QueryPlannerIXSelect::getFields': that models precisely which fields the plan
 * enumerator generates bounds on today, so it prunes subtrees we must not prune -- most notably it
 * stops at a $nor. Here we want a superset, and one that stays a superset as the optimizer grows
 * new ways to use an index. Over-collecting costs an unnecessary replan; under-collecting lets a
 * cached plan referencing a dropped index survive a DDL that should have invalidated it.
 */
void addFilterFields(const MatchExpression* expr, const std::string& prefix, StringSet& out) {
    // '$expr' holds an aggregation expression rather than MatchExpression children.
    if (const auto* exprMatch = dynamic_cast<const ExprMatchExpression*>(expr)) {
        const auto deps = expression::getDependencies(exprMatch->getExpression().get());
        for (const auto& field : deps.fields) {
            out.insert(FieldPath::getFullyQualifiedPath(prefix, field));
        }
    }

    std::string childPrefix = prefix;
    if (const auto path = expr->path(); !path.empty()) {
        auto qualifiedPath = FieldPath::getFullyQualifiedPath(prefix, path);
        // A $elemMatch object predicate is really a predicate on 'path.child', which is the form an
        // index over it would be built on, so descend with the path pushed onto the prefix.
        if (expr->matchType() == MatchExpression::ELEM_MATCH_OBJECT) {
            childPrefix = qualifiedPath;
        }
        out.insert(std::move(qualifiedPath));
    }
    for (size_t i = 0; i < expr->numChildren(); ++i) {
        addFilterFields(expr->getChild(i), childPrefix, out);
    }
}

/**
 * Hash of everything about an index that could change which plan the optimizer picks.
 *
 * Note that we deliberately do not reuse 'IndexEntry::operator==', which compares names only: a
 * same-named index recreated with a different definition must produce a different hash.
 */
size_t hashIndex(const IndexDescriptor& desc) {
    size_t hash = 0;
    // The name matters because cached INLJ nodes resolve their probe index by name (see
    // 'CachedInljNode::inljForeignIndexName').
    boost::hash_combine(hash, std::hash<std::string>{}(desc.indexName()));
    boost::hash_combine(hash, hashBSONObj(desc.keyPattern()));
    boost::hash_combine(hash, hashBSONObj(desc.partialFilterExpression()));
    boost::hash_combine(hash, hashBSONObj(desc.collation()));
    // 'infoObj' carries the remaining index options, e.g. geo and wildcard parameters.
    boost::hash_combine(hash, hashBSONObj(desc.infoObj()));
    boost::hash_combine(hash, static_cast<int>(desc.getIndexType()));
    boost::hash_combine(hash, static_cast<int>(desc.version()));
    boost::hash_combine(hash, desc.unique());
    boost::hash_combine(hash, desc.behavesAsSparse());
    boost::hash_combine(hash, desc.hidden());
    boost::hash_combine(hash, desc.isPartial());
    // TODO SERVER-130790: incorporate multikey state ('ice->isMultikey()' and, when multikey,
    // 'ice->getMultikeyPaths()') into the fingerprint so that a field becoming multikey
    // invalidates plans built against the non-multikey state.
    return hash;
}

/**
 * Adds every join predicate field to the set of the node that owns it. An INLJ probe index is built
 * on one of these, so they matter even for a node whose own query has no predicates at all.
 *
 * Note that the edge's endpoints are never read. 'ResolvedPath::nodeId' already names the node a
 * path belongs to, so each path is written straight to that node's set, which is why this is one
 * pass over the graph rather than one pass per node.
 */
void addJoinPredicateFields(const JoinGraph& graph,
                            const std::vector<ResolvedPath>& resolvedPaths,
                            std::vector<StringSet>& out) {
    for (EdgeId edgeId = 0; edgeId < graph.numEdges(); ++edgeId) {
        for (const auto& pred : graph.getEdge(edgeId).predicates) {
            for (PathId pathId : {pred.left, pred.right}) {
                tassert(13259300, "path id out of range", pathId < resolvedPaths.size());
                const auto& path = resolvedPaths[pathId];
                tassert(13259301, "node id out of range", path.nodeId < out.size());
                // Index key patterns are declared in base collection terms, so use the underlying
                // path rather than any post-rename name.
                out[path.nodeId].insert(path.underlyingFieldPath.fullPath());
            }
        }
    }
}

/**
 * Whether an index on 'keyPattern' could matter to a query over 'relevantFields', i.e. whether any
 * of its key fields overlaps one of them.
 *
 * Given the relevant field 'a.b', this returns for key pattern:
 *  - {'a.b': 1}   -> true, an exact match.
 *  - {'a': 1}       -> true, a prefix.
 *  - {'a.b.c': 1} -> true, an extension.
 *  - {'ab': 1}    -> false.
 */
bool indexTouchesAnyField(const BSONObj& keyPattern, const StringSet& relevantFields) {
    for (const auto& keyElem : keyPattern) {
        const auto keyField = keyElem.fieldNameStringData();
        // Exact equality is the common case, and needs only a hash lookup.
        if (relevantFields.contains(keyField)) {
            return true;
        }
        const FieldRef keyFieldRef(keyField);
        for (const auto& relevantField : relevantFields) {
            if (keyFieldRef.fullyOverlapsWith(FieldRef(relevantField))) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

StringSet relevantFieldsForQuery(const CanonicalQuery& cq) {
    StringSet fields;

    addFilterFields(cq.getPrimaryMatchExpression(), "" /* prefix */, fields);

    if (const auto* proj = cq.getProj()) {
        // For an exclusion projection these are the paths the projection excludes rather than the
        // ones it requires. We add them either way: this set only needs to be a superset.
        for (const auto& path : proj->getRequiredFields()) {
            fields.insert(path);
        }
    }

    if (const auto& sort = cq.getSortPattern()) {
        for (const auto& part : *sort) {
            // '$meta' sort parts carry no field path.
            if (part.fieldPath) {
                fields.insert(part.fieldPath->fullPath());
            }
        }
    }

    return fields;
}

std::vector<StringSet> relevantFieldsPerNode(const JoinGraph& graph,
                                             const std::vector<ResolvedPath>& resolvedPaths) {
    std::vector<StringSet> fields(graph.numNodes());

    // Fields each node's own query references, in any of its filter, projection or sort.
    for (size_t i = 0; i < graph.numNodes(); ++i) {
        if (const auto* accessPath = graph.accessPathAt(static_cast<NodeId>(i)); accessPath) {
            fields[i] = relevantFieldsForQuery(*accessPath);
        }
    }

    addJoinPredicateFields(graph, resolvedPaths, fields);

    return fields;
}

std::vector<IndexFingerprint> computeRelevantIndexHashes(
    const std::vector<std::shared_ptr<const IndexCatalogEntry>>& inljEligibleIndexes,
    const StringSet& relevantFields) {
    std::vector<IndexFingerprint> indexHashes;
    for (const auto& ice : inljEligibleIndexes) {
        const auto* desc = ice->descriptor();
        if (!indexTouchesAnyField(desc->keyPattern(), relevantFields)) {
            continue;
        }
        indexHashes.push_back(hashIndex(*desc));
    }
    std::sort(indexHashes.begin(), indexHashes.end());
    return indexHashes;
}

std::vector<NodeFingerprint> makeNodeFingerprints(const JoinGraph& graph,
                                                  const std::vector<ResolvedPath>& resolvedPaths,
                                                  const AvailableIndexes& perCollIdxs) {
    const auto perNodeFields = relevantFieldsPerNode(graph, resolvedPaths);

    std::vector<NodeFingerprint> fingerprints;
    fingerprints.reserve(graph.numNodes());
    for (size_t i = 0; i < graph.numNodes(); ++i) {
        const auto& nss = graph.getNode(static_cast<NodeId>(i)).collectionName;
        auto it = perCollIdxs.find(nss);
        tassert(13259302,
                str::stream() << "no INLJ-eligible index list for collection "
                              << nss.toStringForErrorMsg(),
                it != perCollIdxs.end());
        fingerprints.push_back(NodeFingerprint{
            .relevantIndexHashes = computeRelevantIndexHashes(it->second, perNodeFields[i])});
    }
    return fingerprints;
}

}  // namespace mongo::join_ordering
