// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/optimizer/join/predicate_inferer.h"

#include "mongo/db/exec/classic/subplanning_utils.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/compiler/optimizer/join/join_graph.h"
#include "mongo/db/query/compiler/optimizer/join/path_resolver.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/util/disjoint_set.h"

#include <boost/optional/optional.hpp>

namespace mongo::join_ordering {


/*
 * This helper rewrites the single table predicate's path to match the target node's field.
 * eg we have join A.x = B.y and STP B.y = 5. This helper ensures we propogate the access path
 * A.x = 5 and *not* A.y = 5.
 */
std::unique_ptr<MatchExpression> rewriteMatchExpressionPath(std::unique_ptr<MatchExpression> expr,
                                                            const std::string& targetField) {

    if (!expr)
        return nullptr;

    if (auto* pathExpr = dynamic_cast<PathMatchExpression*>(expr.get())) {
        pathExpr->setPath(targetField);
    }

    for (size_t i = 0; i < expr->numChildren(); ++i) {
        // resetChild takes ownership; clone+rewrite the child and reset.
        auto childClone = std::unique_ptr<MatchExpression>(expr->getChild(i)->clone().release());
        auto rewrittenChild = rewriteMatchExpressionPath(std::move(childClone), targetField);
        expr->resetChild(i, rewrittenChild.release());
    }

    return expr;
}

/*
 * This is a recursive function that descends a join node's access path (expr) to return any single
 * table predicates that reference the node's fieldPath that is associated with this given
 * equivalence class. Returns nullptr if there are none.
 *
 * For example consider the join A.a = B.b, A.c = B.c and the predicate A.a = 4
 * and A.c = 2. This will create two EC: {A.a, B.b} and {A.c, B.c} and a filter on A, {a: 4, c: 2}.
 * For the first EC, we want to only return {a: 4} and not include the filter on c.
 *
 * $expr equalities are returned as regular $eq, and the $expr predicate itself stays on its own
 * node. A regular $eq is implied on the other nodes across both $eq and $expr join edges. The $expr
 * equality is not: {$expr: {$eq: ["$a", null]}} does not match a missing 'a', but a
 * localField/foreignField join matches missing to null.
 *
 * This is only correct because join fields are never arrays, which join optimization enforces.
 * With arrays, neither form implies the other: {"a.b": {$eq: [3]}} matches {a: [{b: [3]}]} but not
 * {a: [{b: 3}]}  and {$expr: {$eq: ["$a.b", [3]]}} matches the opposite.
 */
std::unique_ptr<MatchExpression> getSingleTablePredicateForField(const MatchExpression* expr,
                                                                 const FieldPath& fieldPath) {

    if (!expr) {
        return nullptr;
    }

    if (ComparisonMatchExpressionBase::isEquality(expr->matchType())) {
        // Ensure that the field associated with this leaf path expression is the same as the field
        // passed to this function (eg the field associated with this node in this equivalence
        // class).
        const auto* cmp = checked_cast<const ComparisonMatchExpressionBase*>(expr);
        if (cmp->path() != fieldPath.fullPath()) {
            return nullptr;
        }

        if (ComparisonMatchExpressionBase::isInternalExprComparison(expr->matchType())) {
            // $_internalExprEq comes from an $expr equality, which we infer as a regular $eq.
            return std::make_unique<EqualityMatchExpression>(cmp->path(), Value(cmp->getData()));
        }
        // This is an $eq MatchExpression. To preserve $eq semantics, we just return a copy
        // untouched.
        return expr->clone();
    }

    if (expr->matchType() == MatchExpression::MATCH_IN) {
        const auto* inExpr = checked_cast<const InMatchExpression*>(expr);
        if (inExpr->path() != fieldPath.fullPath()) {
            return nullptr;
        }

        return inExpr->clone();
    }

    if (expr->matchType() == MatchExpression::AND) {
        std::vector<std::unique_ptr<MatchExpression>> children;
        for (size_t i = 0; i < expr->numChildren(); ++i) {
            if (auto child = getSingleTablePredicateForField(expr->getChild(i), fieldPath)) {
                children.push_back(std::move(child));
            }
        }

        if (children.empty()) {
            return nullptr;
        }

        if (children.size() == 1) {
            return std::move(children[0]);
        }

        auto andExpr = std::make_unique<AndMatchExpression>();
        for (auto& child : children) {
            andExpr->add(std::move(child));
        }
        return andExpr;
    }

    // Do not support $or, $nor, $not
    return nullptr;
}

/*
 * This function finalizes the single table predicate so that it can be correctly propagated across
 * the equivalence class: it clones the predicate and, if the target node's field differs from the
 * source node's, rewrites the field paths to match.
 */
std::unique_ptr<MatchExpression> finalizeSTPForTargetNode(
    const MatchExpression& pred,
    const std::vector<ResolvedPath>& resolvedPaths,
    PathId pathId,
    const FieldPath& sourceFieldName) {

    const auto& targetFieldName = resolvedPaths[pathId].underlyingFieldPath;
    auto matchExpr = pred.clone();
    if (sourceFieldName != targetFieldName) {
        matchExpr = rewriteMatchExpressionPath(std::move(matchExpr), targetFieldName.fullPath());
    }
    return matchExpr;
}

StatusWith<std::unique_ptr<CanonicalQuery>> cloneCQWithUpdatedFilter(
    const CanonicalQuery& cqOld, std::unique_ptr<MatchExpression> expr, bool enableSimplification) {
    const auto& oldExpCtx = cqOld.getExpCtx();
    auto expCtx = makeCopyFromExpressionContext(oldExpCtx, cqOld.nss(), oldExpCtx->getUUID());
    // Share the collator instance so it is the same object as on the source ExpressionContext,
    // which the $expr nodes in 'expr' still point at.
    if (!expCtx->getIgnoreCollator()) {
        expCtx->setCollator(oldExpCtx->getCollatorShared());
    }
    if (enableSimplification) {
        expCtx->setInLookup(false);
    }
    expCtx->setPlanCache(ExpressionContext::PlanCacheOptions::kDisablePlanCache);

    expr->setCollator(expCtx->getCollator());

    auto fcr = query_request_helper::makeFromFindCommand(cqOld.getFindCommandRequest());
    // Replace the old filter with the new one.
    fcr->setFilter(expr->serialize());
    auto pfc = ParsedFindCommand::withExistingFilter(expCtx,
                                                     nullptr,
                                                     std::move(expr),
                                                     std::move(fcr),
                                                     ProjectionPolicies::findProjectionPolicies());
    if (!pfc.isOK()) {
        return pfc.getStatus();
    }
    CanonicalQueryParams params{.expCtx = expCtx, .parsedFind = std::move(pfc.getValue())};
    auto cq = CanonicalQuery::make(std::move(params));
    if (cq.isOK() && SubPlanningUtils::canUseSubplanning(*cq.getValue())) {
        return Status(ErrorCodes::QueryFeatureNotAllowed,
                      "Encountered rooted $or, can't use subplanning together with join opt");
    }
    return cq;
}
/*
 * This function takes a single table predicate and the equivalence class associated with it and
 * propogates it to all other nodes in said equivalence class.
 */
Status propagateSingleTablePredicate(absl::InlinedVector<PathId, 8> equivalenceClass,
                                     const MatchExpression& singleTablePred,
                                     PathId sourcePathId,
                                     const std::vector<ResolvedPath>& resolvedPaths,
                                     MutableJoinGraph& graph,
                                     const FieldPath& sourceFieldName,
                                     std::vector<BSONObj>& accessPathsBackingBson,
                                     OpDebug::JoinOptimizationMetrics& metrics) {
    for (PathId pathId : equivalenceClass) {
        if (pathId == sourcePathId) {
            // This node produced the STP we're propagating - no work to do, skip.
            continue;
        }
        auto& targetNode = graph.getNode(resolvedPaths[pathId].nodeId);
        auto normalizedSTP =
            finalizeSTPForTargetNode(singleTablePred, resolvedPaths, pathId, sourceFieldName);
        auto* currentFilter = targetNode.accessPath->getPrimaryMatchExpression();

        // We add STPs to an increasingly large AND, which gets flattened during normalization in
        // normalizeAndOptimizeSingleTablePredicates() step.
        auto andExpr = std::make_unique<AndMatchExpression>();
        // Clone and add the existing filter.
        auto rootClone = currentFilter->clone();
        andExpr->add(std::move(rootClone));
        // Add the to-be-propagated STP
        andExpr->add(std::move(normalizedSTP));
        // Set newFilter to the AND wrapper.
        auto newFilter = std::unique_ptr<MatchExpression>(std::move(andExpr));

        // After construction, CanonicalQuery's MatchExpression is read-only. Since the propogation
        // logic above requires updating the root of the MatchExpression, updating the join node's
        // access path will require constructing an entirely new CQ with the newFilter. This will
        // normalize and optimize the MatchExpression (eg flattening an unnecessary AND wrapper)
        // before creating the new CQ.
        auto swCq = cloneCQWithUpdatedFilter(
            *targetNode.accessPath, std::move(newFilter), /*enableSimplification=*/true);
        if (!swCq.isOK()) {
            uassertStatusOK(swCq.getStatus());
        }

        // A MatchExpression parsed from the user request doesn't own its underlying bson - the
        // FindCommandRequest associated with the same CQ, does. Since the STPs are extracted from
        // CQ's MatchExpression, if we don't save the overall CQ before the std::move, the
        // underlying bson will get destroyed. This means that any join node's access path that
        // pointed to that MatchExpression will now be pointing to corrupted data. To avoid this, we
        // hold onto the original BSONObj.
        accessPathsBackingBson.emplace_back(targetNode.accessPath->getQueryObj());
        targetNode.accessPath = std::move(swCq.getValue());

        metrics.numInferredSingleTablePredicates++;
    }
    return Status::OK();
}

struct SingleTablePredResult {
    std::unique_ptr<MatchExpression> pred;
    PathId pathId;
    FieldPath fieldName;
};

/*
 * This function takes as input a single equivalence class and inspects each node in the EC for a
 * single table predicate that applies to that EC, which it will then return.
 *
 * For example, consider the EC {M.m, N.n, O.o} and the predicate on M {a : 400, m : 10}. Since
 * M.a is not a member of that EC, we will only return {m : 10}.
 */
boost::optional<SingleTablePredResult> extractSingleTablePredicateFromEquivclass(
    const absl::InlinedVector<PathId, 8>& equivalenceClass,
    MutableJoinGraph& graph,
    const std::vector<ResolvedPath>& resolvedPaths) {

    for (PathId pathId : equivalenceClass) {
        auto& cq = graph.getNode(resolvedPaths[pathId].nodeId).accessPath;

        if (!cq->getPrimaryMatchExpression()->isTriviallyTrue()) {
            // There is a single table predicate (STP) on this node.
            auto& fieldName = resolvedPaths[pathId].underlyingFieldPath;
            // Confirm that the STP is indeed on the field that is associated with this
            // equivalence class.
            if (auto pred =
                    getSingleTablePredicateForField(cq->getPrimaryMatchExpression(), fieldName)) {

                return SingleTablePredResult{std::move(pred), pathId, fieldName};
            }
        }
    }
    return boost::none;
}

/*
 * This function takes as input pathSets which is the join's set of equivalence classes.
 * For each equivalence class in the join graph, this function inspects each node for a single table
 * predicate that applies to the given EC. If an STP is identified, it is propagated to all the
 * other nodes in the same EC via a filter on that node/CanonicalQuery.
 *
 * For example, consider the join A.a = B.b and C.c = A.a where A.a = 100. From this, we can infer
 * B.b = 100 and C.c = 100 thus we add the filter c = 100 to node C and b = 100 to node B.
 */
StatusWith<std::vector<BSONObj>> inferSingleTablePredicate(
    MutableJoinGraph& graph,
    const std::vector<ResolvedPath>& resolvedPaths,
    stdx::unordered_map<size_t, absl::InlinedVector<PathId, 8>> pathSets,
    OpDebug::JoinOptimizationMetrics& metrics) {
    std::vector<BSONObj> accessPathsBackingBson;

    for (const auto& [setId, equivalenceClass] : pathSets) {
        if (auto res =
                extractSingleTablePredicateFromEquivclass(equivalenceClass, graph, resolvedPaths)) {
            auto& [singleTablePredicate, pathId, fieldName] = *res;
            // Add the STP to all the other nodes in this equivalence class.
            auto sw = propagateSingleTablePredicate(equivalenceClass,
                                                    *singleTablePredicate,
                                                    pathId,
                                                    resolvedPaths,
                                                    graph,
                                                    fieldName,
                                                    accessPathsBackingBson,
                                                    metrics);
            if (!sw.isOK()) {
                return sw;
            }
        }
    }
    return accessPathsBackingBson;
}

/**
 * This function has two purposes:
 * 1. To find and add implicit (transitive) edges within the graph.
 * `maxNodes` is the maximum number of nodes allowed in a connected component to be used for
 * implicit edge finding.
 * Example: two edges A.a = B.b and B.b = C.c form an implicit edge A.a = C.c.
 *
 * 2. To propagate single table predicates where available across all the equivalence classes
 * created by the disjoint set in the previous step.
 * Example: Let's take the previous join graph and specify the predicate B.b = 5. This second step
 * will then add the filter c = 5 on node C and a = 5 on node A.
 */
StatusWith<std::vector<BSONObj>> addImplicitEdgesAndInferPredicates(
    MutableJoinGraph& graph,
    const std::vector<ResolvedPath>& resolvedPaths,
    size_t maxNodes,
    OpDebug::JoinOptimizationMetrics& metrics) {
    DisjointSet ds{resolvedPaths.size()};
    for (const auto& edge : graph.edges()) {
        for (const auto& pred : edge.predicates) {
            if (pred.isEquality()) {
                ds.unite(pred.left, pred.right);
            }
        }
    }

    stdx::unordered_map<size_t, absl::InlinedVector<PathId, 8>> pathSets{};
    for (size_t i = 0; i < ds.size(); ++i) {
        auto setId = ds.find(i);
        tassert(11116502, "Unknown pathId", setId.has_value());
        auto& pathSet = pathSets[setId.value()];
        if (pathSet.size() < maxNodes) {
            const PathId currentPathId = static_cast<PathId>(i);
            const NodeId currentNodeId = resolvedPaths[currentPathId].nodeId;
            for (PathId pathId : pathSet) {
                const NodeId nodeId = resolvedPaths[pathId].nodeId;
                if (nodeId == currentNodeId) {
                    // Both paths belong to the same node (e.g., results.key and
                    // results.cor.key.foo). This would be a self-edge, which is not a join edge.
                    // Skip it; the inferred single-table equality is handled by
                    // inferSingleTablePredicate below.
                    continue;
                }
                // The join graph limits 'maxEdgesInJoin' or 'maxPredicatesInEdge' can be hit here
                // and the predicate wouldn't be added. This is fine because it doesn't affect the
                // correctness of the query, only the size of the graph and the number of possible
                // join plans.
                // Note: We always add implicit edges as equality edges, then enforce stricter $expr
                // equality semantics during physical plan generation.
                auto edgeId =
                    graph.addSimpleEqualityEdge(nodeId, currentNodeId, pathId, currentPathId);
                if (!edgeId) {
                    // This graph is plenty big, we'll leave the rest of the joins in the suffix
                    // unoptimized.
                    break;
                }
                metrics.numInferredEqJoinPredicates++;
            }
            pathSet.push_back(currentPathId);
        }
    }

    return inferSingleTablePredicate(graph, resolvedPaths, pathSets, metrics);
}

}  // namespace mongo::join_ordering
