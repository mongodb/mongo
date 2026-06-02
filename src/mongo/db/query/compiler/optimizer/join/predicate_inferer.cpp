/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/optimizer/join/predicate_inferer.h"

#include "mongo/db/exec/classic/subplanning_utils.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/compiler/optimizer/join/join_graph.h"
#include "mongo/db/query/compiler/optimizer/join/path_resolver.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/util/disjoint_set.h"

#include <boost/optional/optional.hpp>

namespace mongo::join_ordering {


/*
 * This helper modifies a single table predicate that is written in $expr syntax so that the paths
 * match the target node's field. eg consider the join A.a = C.c and the STP on node C, { $expr: {
 * $eq: [ "$c", { $const: 10 }. For node A the targetField will be "a" and as such, this helper will
 * return, { $expr: { $eq: [ "$a", { $const: 10 }.
 */
boost::intrusive_ptr<Expression> rewritePipelineExpressionPath(Expression* expr,
                                                               ExpressionContext* expCtx,
                                                               const std::string& targetField) {

    if (!expr) {
        return nullptr;
    }

    // The pipeline expression tree is immutable so to rewrite the field path, we create a new
    // ExpressionFieldPath with the path value for this equivalence class on the target node (eg
    // targetField). Borrowing the example above, this is where we effectively change "y" to "x"
    // so the STP (expr) can be applied to the target node A.
    if (dynamic_cast<ExpressionFieldPath*>(expr)) {
        return ExpressionFieldPath::createPathFromString(
            expCtx, targetField, expCtx->variablesParseState);
    }

    if (auto* cmp = dynamic_cast<ExpressionCompare*>(expr)) {
        std::vector<boost::intrusive_ptr<Expression>> newChildren;
        for (auto& child : cmp->getChildren()) {
            newChildren.push_back(rewritePipelineExpressionPath(child.get(), expCtx, targetField));
        }
        return boost::intrusive_ptr<Expression>(
            new ExpressionCompare(expCtx, cmp->getOp(), std::move(newChildren)));
    }

    if (auto* andExpr = dynamic_cast<ExpressionAnd*>(expr)) {
        std::vector<boost::intrusive_ptr<Expression>> newChildren;

        for (auto& child : andExpr->getChildren()) {
            newChildren.push_back(rewritePipelineExpressionPath(child.get(), expCtx, targetField));
        }

        return boost::intrusive_ptr<Expression>(new ExpressionAnd(expCtx, std::move(newChildren)));
    }

    if (auto* orExpr = dynamic_cast<ExpressionOr*>(expr)) {
        std::vector<boost::intrusive_ptr<Expression>> newChildren;

        for (auto& child : orExpr->getChildren()) {
            newChildren.push_back(rewritePipelineExpressionPath(child.get(), expCtx, targetField));
        }

        return boost::intrusive_ptr<Expression>(new ExpressionOr(expCtx, std::move(newChildren)));
    }

    return expr->clone(*expCtx);
}

/*
 * This helper rewrites the single table predicate's path to match the target node's field.
 * eg we have join A.x = B.y and STP B.y = 5. This helper ensures we propogate the access path
 * A.x = 5 and *not* A.y = 5.
 *
 * If the STP is written in $eq syntax, it will just update the PathMatchExpression leafs.
 * If the STP is written in $expr syntax, it calls a helper to walk the underlying pipeline
 * expression tree to rewrite the field paths to match the target node.
 */
std::unique_ptr<MatchExpression> rewriteMatchExpressionPath(std::unique_ptr<MatchExpression> expr,
                                                            const std::string& targetField,
                                                            ExpressionContext* expCtx) {

    if (!expr)
        return nullptr;

    if (auto* exprMatch = dynamic_cast<ExprMatchExpression*>(expr.get())) {
        // ExprMatchExpression is an ME wrapper around a pipeline expression. We need to access and
        // descend the pipeline expression tree to successfully rewrite the paths.
        auto rewritten =
            rewritePipelineExpressionPath(exprMatch->getExpression().get(), expCtx, targetField);
        return std::make_unique<ExprMatchExpression>(rewritten, expCtx);
    }

    if (auto* pathExpr = dynamic_cast<PathMatchExpression*>(expr.get())) {
        pathExpr->setPath(targetField);
    }

    for (size_t i = 0; i < expr->numChildren(); ++i) {
        // resetChild takes ownership; clone+rewrite the child and reset.
        auto childClone = std::unique_ptr<MatchExpression>(expr->getChild(i)->clone().release());
        auto rewrittenChild =
            rewriteMatchExpressionPath(std::move(childClone), targetField, expCtx);
        expr->resetChild(i, rewrittenChild.release());
    }

    return expr;
}


using SingleTablePredicate =
    std::variant<boost::intrusive_ptr<Expression>, std::unique_ptr<MatchExpression>>;
SingleTablePredicate pipelineExpression, matchExpression;

/*
 * Combines a vector of SingleTablePredicate children into a single predicate using
 * the appropriate AND/OR constructor depending on whether the children are all
 * Expressions or all MatchExpressions.
 */
template <typename ExprCombinator, typename MatchCombinator>
std::optional<SingleTablePredicate> combineChildren(
    std::vector<SingleTablePredicate> children,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {

    bool allExpr = std::all_of(children.begin(), children.end(), [](auto& c) {
        return std::holds_alternative<boost::intrusive_ptr<Expression>>(c);
    });
    if (allExpr) {
        std::vector<boost::intrusive_ptr<Expression>> exprChildren;
        exprChildren.reserve(children.size());
        for (auto& c : children) {
            exprChildren.push_back(std::move(std::get<boost::intrusive_ptr<Expression>>(c)));
        }
        auto combined = make_intrusive<ExprCombinator>(expCtx.get(), std::move(exprChildren));
        return SingleTablePredicate{combined->optimize()};
    }

    bool allMatch = std::all_of(children.begin(), children.end(), [](auto& c) {
        return std::holds_alternative<std::unique_ptr<MatchExpression>>(c);
    });
    if (allMatch) {
        auto combined = std::make_unique<MatchCombinator>();
        for (auto& c : children) {
            combined->add(std::move(std::get<std::unique_ptr<MatchExpression>>(c)));
        }
        return SingleTablePredicate{std::move(combined)};
    }

    return std::nullopt;
}
/*
 * This is a recursive function that descends a join node's access path (expr) to return any single
 * table predicates that reference the node's fieldPath that is associated with this given
 * equivalence class.
 *
 * For example consider the join A.a = B.b, A.c = B.c and the predicate A.a = 4
 * and A.c = 2. This will create two EC: {A.a, B.b} and {A.c, B.c} and a filter on A, {a: 4, c: 2}.
 * For the first EC, we want to only return {a: 4} and not include the filter on c.
 *
 * We have to be careful to preserve $eq vs $expr semantics. As such, this function returns either a
 * MatchExpression or PipelineExpression. Later on, modifyAndNormalizeSTP() will take care of
 * converting PipelineExpressions to MatchExpressions.
 */
std::optional<SingleTablePredicate> getSingleTablePredicateForField(
    const MatchExpression* expr,
    const FieldPath& fieldPath,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {

    if (!expr) {
        return std::nullopt;
    }

    if (ComparisonMatchExpressionBase::isEquality(expr->matchType())) {
        // Ensure that the field associated with this leaf path expression is the same as the field
        // passed to this function (eg the field associated with this node in this equivalence
        // class).
        const auto* cmp = dynamic_cast<const ComparisonMatchExpressionBase*>(expr);
        if (cmp->path() != fieldPath.fullPath()) {
            return std::nullopt;
        }

        // Preserve $eq vs $expr semantics.
        if (ComparisonMatchExpressionBase::isInternalExprComparison(expr->matchType())) {
            // This case represents the $expr case!
            // More specifically, we're dealing with $_internalExprEq, which is an internal operator
            // the optimizer adds to the we $expr expression for indexability. To preserve $expr
            // semantics and replace the incomplete/internal representation, we rewrite
            // $_internalExprEq MatchExpression as an $eq pipeline expression.
            const ComparisonMatchExpressionBase* cmp =
                dynamic_cast<const ComparisonMatchExpressionBase*>(expr);
            auto field = static_cast<std::string>(cmp->path());
            BSONElement value = cmp->getData();
            auto fieldExpr = ExpressionFieldPath::createPathFromString(
                expCtx.get(), field, expCtx->variablesParseState);
            auto constExpr = ExpressionConstant::create(expCtx.get(), Value(value));
            auto eqExpr = ExpressionCompare::create(
                expCtx.get(), ExpressionCompare::CmpOp::EQ, fieldExpr, constExpr);
            return eqExpr;
        }
        // This is an $eq MatchExpression. To preserve $eq semantics, we just return a copy
        // untouched.
        return expr->clone();
    }

    if (expr->matchType() == MatchExpression::AND) {
        std::vector<SingleTablePredicate> children;
        for (size_t i = 0; i < expr->numChildren(); ++i) {
            auto child = getSingleTablePredicateForField(expr->getChild(i), fieldPath, expCtx);
            if (child) {
                children.push_back(std::move(*child));
            }
        }

        if (children.empty()) {
            return std::nullopt;
        }

        if (children.size() == 1) {
            return std::move(children[0]);
        }

        if (auto combined =
                combineChildren<ExpressionAnd, AndMatchExpression>(std::move(children), expCtx)) {
            return combined;
        }
    }

    if (expr->matchType() == MatchExpression::OR) {
        std::vector<SingleTablePredicate> children;
        for (size_t i = 0; i < expr->numChildren(); ++i) {
            auto child = getSingleTablePredicateForField(expr->getChild(i), fieldPath, expCtx);
            if (child) {
                children.push_back(std::move(*child));
            }
        }
        if (children.empty()) {
            return std::nullopt;
        }

        if (children.size() == 1) {
            return std::move(children[0]);
        }

        if (auto combined =
                combineChildren<ExpressionOr, OrMatchExpression>(std::move(children), expCtx)) {
            return combined;
        }
    }
    return std::nullopt;
}

/*
 * This function finalizes the single table predicate so that it can be correctly propagated across
 * the equivalence class.
 * This includes three steps:
 * 1. Unify pred into match-expression form. A MatchExpression input is cloned as-is; a
 * PipelineExpression input is wrapped in $expr to produce an equivalent MatchExpression.
 * 2. Ensure the resulting MatchExpression's field paths match the target node's field paths and if
 * not, rewrite them to match.
 * 3. Finally, normalize this MatchExpression.
 */
StatusWith<std::unique_ptr<MatchExpression>> modifyAndNormalizeSTP(
    const SingleTablePredicate& pred,
    const std::vector<ResolvedPath>& resolvedPaths,
    MutableJoinGraph& graph,
    PathId pathId,
    const FieldPath& sourceFieldName) {

    auto& targetNode = graph.getNode(resolvedPaths[pathId].nodeId);
    auto targetNodeExpCtx = targetNode.accessPath->getExpCtx().get();
    const auto& targetFieldName = resolvedPaths[pathId].fieldName;
    // Step 1: Get a MatchExpression, regardless of which alternative pred holds.
    std::unique_ptr<MatchExpression> matchExpr = std::visit(
        [&](const auto& singletablepreciate) -> std::unique_ptr<MatchExpression> {
            using T = std::decay_t<decltype(singletablepreciate)>;
            if constexpr (std::is_same_v<T, boost::intrusive_ptr<Expression>>) {
                // Wrap a clone of the pipeline Expression in $expr.
                return std::make_unique<ExprMatchExpression>(
                    singletablepreciate->clone(*targetNodeExpCtx), targetNodeExpCtx);
            } else {
                // singletablepreciate is std::unique_ptr<MatchExpression>; clone it directly.
                return singletablepreciate->clone();
            }
        },
        pred);

    if (!matchExpr) {
        return Status(ErrorCodes::BadValue, "There is no predicate");
    }

    if (sourceFieldName != targetFieldName) {
        matchExpr = rewriteMatchExpressionPath(
            std::move(matchExpr), targetFieldName.fullPath(), targetNodeExpCtx);
    }

    matchExpr = normalizeMatchExpression(std::move(matchExpr));
    return matchExpr;
}

StatusWith<std::unique_ptr<CanonicalQuery>> createCanonicalQueryFromSingleMatchExpression(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    NamespaceString nss,
    std::unique_ptr<MatchExpression> expr) {
    ExpressionContext::PlanCacheOptions oldPlanCache = expCtx->getPlanCache();
    expCtx->setPlanCache(ExpressionContext::PlanCacheOptions::kDisablePlanCache);
    auto pfc = ParsedFindCommand::withExistingFilter(expCtx,
                                                     nullptr,
                                                     std::move(expr),
                                                     std::make_unique<FindCommandRequest>(nss),
                                                     ProjectionPolicies::findProjectionPolicies());
    CanonicalQueryParams params{.expCtx = expCtx, .parsedFind = std::move(pfc.getValue())};
    auto cq = CanonicalQuery::make(std::move(params));
    expCtx->setPlanCache(oldPlanCache);
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
                                     SingleTablePredicate singleTablePred,
                                     PathId sourcePathId,
                                     const std::vector<ResolvedPath>& resolvedPaths,
                                     MutableJoinGraph& graph,
                                     const FieldPath& sourceFieldName,
                                     std::vector<BSONObj>& accessPathsBackingBson) {
    for (PathId pathId : equivalenceClass) {
        if (pathId == sourcePathId) {
            // This node produced the STP we're propagating - no work to do, skip.
            continue;
        }
        auto& targetNode = graph.getNode(resolvedPaths[pathId].nodeId);
        auto swSTP =
            modifyAndNormalizeSTP(singleTablePred, resolvedPaths, graph, pathId, sourceFieldName);
        if (!swSTP.isOK()) {
            return swSTP.getStatus();
        }
        auto normalizedSTP = std::move(swSTP.getValue());
        auto* currentFilter = targetNode.accessPath->getPrimaryMatchExpression();
        std::unique_ptr<MatchExpression> newFilter;
        if (currentFilter->isTriviallyTrue()) {
            // This branch represents the case where the targetNode doesn't have a filter - so we
            // just replace it with the STP.
            newFilter = std::move(normalizedSTP);
        } else {
            // This branch represents the case where the targetNode already has a filter.
            // In this case, wrap the existing filter in an AND and add the STP we're currently
            // propagating.

            // Create the new AND wrapper.
            auto andExpr = std::make_unique<AndMatchExpression>();
            // Clone and add the existing filter.
            auto rootClone = currentFilter->clone();
            andExpr->add(std::move(rootClone));
            // Add the to-be-propagated STP
            andExpr->add(std::move(normalizedSTP));
            // Set newFilter to the AND wrapper.
            newFilter = std::unique_ptr<MatchExpression>(std::move(andExpr));
        }
        // After construction, CanonicalQuery's MatchExpression is read-only. Since the propogation
        // logic above requires updating the root of the MatchExpression, updating the join node's
        // access path will require constructing an entirely new CQ with the newFilter.
        auto nss = targetNode.collectionName;
        auto expCtx = ExpressionContextBuilder{}
                          .opCtx(targetNode.accessPath->getExpCtx()->getOperationContext())
                          .ns(nss)
                          .build();
        auto swCq =
            createCanonicalQueryFromSingleMatchExpression(expCtx, nss, std::move(newFilter));
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
    }
    return Status::OK();
}

struct SingleTablePredResult {
    SingleTablePredicate pred;
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
    const std::vector<ResolvedPath>& resolvedPaths,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {

    for (PathId pathId : equivalenceClass) {
        auto& cq = graph.getNode(resolvedPaths[pathId].nodeId).accessPath;

        if (!cq->getPrimaryMatchExpression()->isTriviallyTrue()) {
            // There is a single table predicate (STP) on this node.
            auto& fieldName = resolvedPaths[pathId].fieldName;
            // Confirm that the STP is indeed on the field that is associated with this
            // equivalence class.
            if (auto pred = getSingleTablePredicateForField(
                    cq->getPrimaryMatchExpression(), fieldName, expCtx)) {

                return SingleTablePredResult{std::move(*pred), pathId, fieldName};
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
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    std::vector<BSONObj> accessPathsBackingBson;

    for (const auto& [setId, equivalenceClass] : pathSets) {
        if (auto res = extractSingleTablePredicateFromEquivclass(
                equivalenceClass, graph, resolvedPaths, expCtx)) {
            auto& [singleTablePredicate, pathId, fieldName] = *res;
            // Add the STP to all the other nodes in this equivalence class.
            auto sw = propagateSingleTablePredicate(equivalenceClass,
                                                    std::move(singleTablePredicate),
                                                    pathId,
                                                    resolvedPaths,
                                                    graph,
                                                    fieldName,
                                                    accessPathsBackingBson);
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
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
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
            }
            pathSet.push_back(currentPathId);
        }
    }
    // TODO SERVER-117385 remove this flag and this check once we can handle inferred predicates in
    // our CE.
    if (!expCtx->getQueryKnobConfiguration().getInferSingleTablePredicates()) {
        return std::vector<BSONObj>{};
    }
    return inferSingleTablePredicate(graph, resolvedPaths, pathSets, expCtx);
}

}  // namespace mongo::join_ordering
