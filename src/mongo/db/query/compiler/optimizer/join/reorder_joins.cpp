/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/optimizer/join/reorder_joins.h"

#include "mongo/db/query/compiler/optimizer/join/join_graph.h"
#include "mongo/db/query/compiler/optimizer/join/join_plan.h"
#include "mongo/db/query/compiler/optimizer/join/join_reordering_context.h"
#include "mongo/db/query/compiler/optimizer/join/plan_enumerator.h"
#include "mongo/db/query/compiler/optimizer/join/plan_enumerator_helpers.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/db/query/random_utils.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::join_ordering {

namespace {
/**
 * Helper function to simplify creation of BinaryJoinEmbedding QSNs.
 */
std::unique_ptr<QuerySolutionNode> makeBinaryJoinEmbeddingQSN(
    JoinMethod method,
    std::vector<QSNJoinPredicate>&& joinPreds,
    std::unique_ptr<QuerySolutionNode> leftChild,
    boost::optional<FieldPath> leftEmbedPath,
    std::unique_ptr<QuerySolutionNode> rightChild,
    boost::optional<FieldPath> rightEmbedPath) {
    switch (method) {
        case JoinMethod::HJ:
            return std::make_unique<HashJoinEmbeddingNode>(std::move(leftChild),
                                                           std::move(rightChild),
                                                           std::move(joinPreds),
                                                           leftEmbedPath,
                                                           rightEmbedPath);
        case JoinMethod::NLJ:
            return std::make_unique<NestedLoopJoinEmbeddingNode>(std::move(leftChild),
                                                                 std::move(rightChild),
                                                                 std::move(joinPreds),
                                                                 leftEmbedPath,
                                                                 rightEmbedPath);
        case JoinMethod::INLJ:
            return std::make_unique<IndexedNestedLoopJoinEmbeddingNode>(std::move(leftChild),
                                                                        std::move(rightChild),
                                                                        std::move(joinPreds),
                                                                        leftEmbedPath,
                                                                        rightEmbedPath);
    }
    MONGO_UNREACHABLE_TASSERT(11336909);
}

/**
 * Construct an appropriate IndexProbe for the given 'edge' & 'node', if possible.
 */
std::unique_ptr<QuerySolutionNode> createIndexProbeQSN(
    const JoinNode& node, std::shared_ptr<const IndexCatalogEntry> ice) {
    const auto& desc = ice->descriptor();
    std::unique_ptr<QuerySolutionNode> qsn = std::make_unique<FetchNode>(
        std::make_unique<IndexProbeNode>(node.collectionName,
                                         IndexEntry{desc->keyPattern(),
                                                    desc->getIndexType(),
                                                    desc->version(),
                                                    false /*isMultikey*/,
                                                    {} /*multikeyPaths*/,
                                                    {} /*multikeySet*/,
                                                    desc->isSetSparseByUser(),
                                                    desc->unique(),
                                                    IndexEntry::Identifier{desc->indexName()},
                                                    desc->infoObj(),
                                                    nullptr /*wildcardProjection*/,
                                                    std::move(ice)}),
        node.collectionName);
    if (auto matchExpr = node.accessPath->getPrimaryMatchExpression();
        matchExpr != nullptr && !matchExpr->isTriviallyTrue()) {
        qsn->filter = matchExpr->clone();
    }
    return qsn;
}

/**
 * Given a PathId we need to add to a join predicate, we need to fetch its corresponding name
 * and (potentially) expand it to include the full path including where it is embedded.
 *
 * For example, consider the pipeline:
 *   [{$lookup: {from: "b", localField: "foo", foreignField: "bar", as: "b"}}, {$unwind: "$b"}].
 *
 * The field "bar" resolves to "bar" when not expanded and to "b.bar" when expanded.
 */
FieldPath expandEmbeddedPath(const JoinReorderingContext& ctx, PathId pathId, bool expand) {
    const auto& resolvedPath = ctx.resolvedPaths[pathId];
    if (!expand) {
        return resolvedPath.fieldName;
    }

    const auto& node = ctx.joinGraph.getNode(resolvedPath.nodeId);
    if (node.embedPath.has_value()) {
        return node.embedPath->concat(resolvedPath.fieldName);
    }
    return resolvedPath.fieldName;
}

QSNJoinPredicate makePhysicalPredicate(const JoinReorderingContext& ctx,
                                       JoinPredicate pred,
                                       bool expandLeftPath,
                                       bool expandRightPath) {
    // Left field is a local field and potentially could come from already joined foreign
    // collection, so its embedPath is important to handle here. Right field is a foreign field
    // which comes from the current foreign collection, SBE does not expect it to be prefixed
    // with the foreign collection's as field.
    return {.op = convertToPhysicalOperator(pred.op),
            .leftField = expandEmbeddedPath(ctx, pred.left, expandLeftPath),
            .rightField = expandEmbeddedPath(ctx, pred.right, expandRightPath)};
}

std::vector<QSNJoinPredicate> makeJoinPreds(const JoinReorderingContext& ctx,
                                            const std::vector<JoinEdge>& edges,
                                            bool expandLeftPath,
                                            bool expandRightPath) {
    std::vector<QSNJoinPredicate> preds;
    preds.reserve(edges.size());
    for (const auto& edge : edges) {
        for (const auto& pred : edge.predicates) {
            preds.push_back(makePhysicalPredicate(ctx, pred, expandLeftPath, expandRightPath));
        }
    }
    return preds;
}

void addEstimatesIfExplain(const JoinReorderingContext& ctx,
                           const PlanEnumeratorContext& peCtx,
                           QuerySolutionNode* node,
                           NodeSet set,
                           const JoinCostEstimate& cost,
                           cost_based_ranker::EstimateMap& estimates) {
    if (!ctx.explain) {
        return;
    }

    auto ce = peCtx.getJoinCardinalityEstimator()->getOrEstimateSubsetCardinality(set);
    auto est = std::make_unique<cost_based_ranker::QSNEstimate>(ce, cost.getTotalCost());
    if (internalQueryExplainJoinCostComponents.load()) {
        auto joinEst = std::make_unique<JoinExtraEstimateInfo>(ce, cost.getTotalCost());
        joinEst->docsProcessed = cost.getNumDocsProcessed().toDouble();
        joinEst->docsOutput = cost.getNumDocsOutput().toDouble();
        joinEst->sequentialIOPages = cost.getIoSeqPages().toDouble();
        joinEst->randomIOPages = cost.getIoRandPages().toDouble();
        joinEst->localOpCost = cost.getLocalOpCost().toDouble();
        joinEst->mackertLohmanCase = cost.getMackertLohmanCase();
        est = std::move(joinEst);
    }
    estimates.insert_or_assign(node, std::move(est));
}

// Forward-declare because of mutual recursion.
std::unique_ptr<QuerySolutionNode> buildQSNFromJoiningNode(
    const JoinReorderingContext& ctx,
    const PlanEnumeratorContext& peCtx,
    const JoiningNode& join,
    cost_based_ranker::EstimateMap& estimates);

std::unique_ptr<QuerySolutionNode> buildQSNFromJoinPlan(const JoinReorderingContext& ctx,
                                                        const PlanEnumeratorContext& peCtx,
                                                        JoinPlanNodeId nodeId,
                                                        cost_based_ranker::EstimateMap& estimates) {
    std::unique_ptr<QuerySolutionNode> qsn;
    std::visit(OverloadedVisitor{
                   [&](const JoiningNode& join) {
                       qsn = buildQSNFromJoiningNode(ctx, peCtx, join, estimates);
                       addEstimatesIfExplain(
                           ctx, peCtx, qsn.get(), join.bitset, join.cost, estimates);
                   },
                   [&](const BaseNode& base) {
                       // TODO SERVER-111913: Avoid this clone
                       qsn = base.soln->root()->clone();
                       addEstimatesIfExplain(
                           ctx, peCtx, qsn.get(), NodeSet().set(base.node), base.cost, estimates);
                   },
                   [&](const INLJRHSNode& ip) {
                       qsn = createIndexProbeQSN(ctx.joinGraph.getNode(ip.node), ip.entry);
                   }},
               peCtx.registry().get(nodeId));
    return qsn;
}

NodeId getLeftmostNodeIdOfJoinPlan(const JoinReorderingContext& ctx,
                                   JoinPlanNodeId nodeId,
                                   const JoinPlanNodeRegistry& registry) {
    // Traverse binary tree to get left-most node, so we can use it as the base collection for
    // the aggregation itself- this is the first collection we join with.
    return std::visit(OverloadedVisitor{[&ctx, &registry](const JoiningNode& join) {
                                            return getLeftmostNodeIdOfJoinPlan(
                                                ctx, join.left, registry);
                                        },
                                        [](const BaseNode& base) { return base.node; },
                                        [](const INLJRHSNode&) {
                                            // By definition, this should never happen.
                                            MONGO_UNREACHABLE_TASSERT(11371704);
                                            return (NodeId)0;
                                        }},
                      registry.get(nodeId));
}

/**
 * Retrieve all edges between the left and right node sets, oriented so that the right side of the
 * edge corresponds to the right side of the join.
 *
 * This order is important for generating the 'QSNJoinPredicate' which is order sensitive. Note this
 * is "cheating" a little bit because 'JoinEdge' is logically an undirected edge in the graph but
 * implemented with left/right ordered members. We are exploiting this implementation detail to
 * avoid doing duplicate work of determining the orientation in making the 'IndexedJoinPredicate'
 * and 'QSNJoinPredicate' below.
 *
 * For example, given nodes A, B, C, D with edges A-C and C-D, calling getEdges(joinGraph, {A, B,
 * D}, {C}) will return edges A-C and C-D (note the orientation).
 */
std::vector<JoinEdge> getEdges(const JoinGraph& joinGraph, NodeSet left, NodeSet right) {
    auto edges = joinGraph.getJoinEdges(left, right);
    tassert(11179800,
            "Must have at least one join edge as cross products are not currently supported",
            !edges.empty());

    std::vector<JoinEdge> res;
    res.reserve(edges.size());
    for (auto edgeId : edges) {
        const JoinEdge& edge = joinGraph.getEdge(edgeId);
        // Ensure that edge is oriented so that 'right' side corresponds to right side of join.
        if (right.test(edge.left)) {
            res.push_back(edge.reverseEdge());
        } else {
            res.push_back(edge);
        }
    }
    return res;
}

const JoinNode& findFirstNode(const JoinGraph& joinGraph, NodeSet set) {
    for (size_t i = 0; i < kHardMaxNodesInJoin; i++) {
        if (set.test(i)) {
            return joinGraph.getNode((NodeId)i);
        }
    }
    MONGO_UNREACHABLE_TASSERT(11336910);
}

std::unique_ptr<QuerySolutionNode> buildQSNFromJoiningNode(
    const JoinReorderingContext& ctx,
    const PlanEnumeratorContext& peCtx,
    const JoiningNode& join,
    cost_based_ranker::EstimateMap& estimates) {
    auto leftChild = buildQSNFromJoinPlan(ctx, peCtx, join.left, estimates);
    auto rightChild = buildQSNFromJoinPlan(ctx, peCtx, join.right, estimates);

    const auto& leftSubset = peCtx.registry().getBitset(join.left);
    const auto& rightSubset = peCtx.registry().getBitset(join.right);

    const bool isLeftBaseNode = leftSubset.count() == 1;
    const bool isRightBaseNode = rightSubset.count() == 1;

    boost::optional<FieldPath> leftEmbedding, rightEmbedding;
    if (isLeftBaseNode) {
        // Node on the left may be embedded- we need to retrieve its embedding.
        const auto& leftNode = findFirstNode(ctx.joinGraph, leftSubset);
        leftEmbedding = leftNode.embedPath;
    }
    if (isRightBaseNode) {
        // Node on the right may be embedded- we need to retrieve its embedding.
        const auto& rightNode = findFirstNode(ctx.joinGraph, rightSubset);
        rightEmbedding = rightNode.embedPath;
    }

    auto edges = getEdges(ctx.joinGraph, leftSubset, rightSubset);

    // Only expand predicates for non-base nodes.
    bool expandLeftPath = !isLeftBaseNode;
    bool expandRightPath = !isRightBaseNode;
    auto joinPreds = makeJoinPreds(ctx, edges, expandLeftPath, expandRightPath);

    return makeBinaryJoinEmbeddingQSN(join.method,
                                      std::move(joinPreds),
                                      std::move(leftChild),
                                      leftEmbedding,
                                      std::move(rightChild),
                                      rightEmbedding);
}

ReorderedJoinSolution makeReorderedJoinSoln(const JoinReorderingContext& ctx,
                                            const PlanEnumeratorContext& peCtx) {
    auto bestPlanNodeId = peCtx.getBestFinalPlan();

    const auto& registry = peCtx.registry();
    LOGV2_DEBUG(11179802,
                5,
                "Winning join plan",
                "plan"_attr =
                    registry.joinPlanNodeToBSON(bestPlanNodeId, ctx.joinGraph.numNodes()));

    // Build QSN based on best plan.
    auto ret = std::make_unique<QuerySolution>();
    auto baseNodeId = getLeftmostNodeIdOfJoinPlan(ctx, bestPlanNodeId, registry);

    cost_based_ranker::EstimateMap estimates;
    ret->setRoot(buildQSNFromJoinPlan(ctx, peCtx, bestPlanNodeId, estimates));
    LOGV2_DEBUG(11179803, 5, "QSN for winning plan", "qsn"_attr = ret->toString());

    ReorderedJoinSolution out{
        .soln = std::move(ret), .baseNode = baseNodeId, .estimates = std::move(estimates)};

    // Also generate rejected plans if we're in explain mode.
    if (ctx.explain) {
        auto rejectedPlans = peCtx.getRejectedFinalPlans();
        out.rejectedSolns.reserve(rejectedPlans.size());

        for (auto&& planNodeId : rejectedPlans) {
            auto solution = std::make_unique<QuerySolution>();
            solution->setRoot(buildQSNFromJoinPlan(ctx, peCtx, planNodeId, out.estimates));
            out.rejectedSolns.push_back(
                {std::move(solution), getLeftmostNodeIdOfJoinPlan(ctx, planNodeId, registry)});
        }
    }

    return out;
}

/**
 * Traverses the join graph randomly to generate a join order (expressed as a vector of NodeIds).
 */
std::vector<NodeId> pickRandomJoinOrder(const JoinReorderingContext& ctx,
                                        random_utils::PseudoRandomGenerator& rand) {
    // Set of nodes we have already visited
    NodeSet visited;
    // Final join order, and ordered queue of nodes we still need to visit.
    std::vector<NodeId> order, frontier;
    order.reserve(ctx.joinGraph.numNodes());

    // Randomly select a base collection.
    NodeId baseId = rand.generateUniformInt(0, (int)(ctx.joinGraph.numNodes() - 1));
    frontier.push_back(baseId);

    boost::optional<FieldPath> leftMostFieldPath;

    while (!frontier.empty()) {
        auto current = frontier.back();

        // In the case of a cycle, we may have already seen this node. Skip it.
        if (visited.test(current)) {
            frontier.pop_back();
            continue;
        }

        frontier.pop_back();
        order.push_back(current);
        visited.set(current);

        // Get unvisited neighbors
        auto neighbors = ctx.joinGraph.getNeighbors(current);
        std::vector<uint32_t> unvisited;
        for (size_t i = 0; i < neighbors.size(); ++i) {
            if (!neighbors.test(i)) {
                continue;
            }
            if (!visited.test(i)) {
                unvisited.push_back(i);
            }
        }
        // Randomize the order of the neighbors and add them to the queue
        rand.shuffleVector(unvisited);
        for (auto n : unvisited) {
            frontier.push_back(n);
        }
    }

    return order;
}

PerSubsetLevelEnumerationMode makeRandomizedEnumerationHint(
    const JoinReorderingContext& ctx,
    random_utils::PseudoRandomGenerator& rand,
    PlanTreeShape planShape,
    boost::optional<JoinMethod> overrideMethod) {
    auto nodes = pickRandomJoinOrder(ctx, rand);

    // If we are given a specific shape of plan, make sure we only produce a hint that would be
    // valid for that shape, otherwise randomize the shape by randomly picking which side to add the
    // next base node to.
    auto pickChildSide = [planShape, &rand, overrideMethod]() {
        switch (planShape) {
            case PlanTreeShape::LEFT_DEEP:
                return false;
            case PlanTreeShape::RIGHT_DEEP:
                return true;
            case PlanTreeShape::ZIG_ZAG: {
                if (overrideMethod &&
                    (*overrideMethod == JoinMethod::INLJ || *overrideMethod == JoinMethod::NLJ)) {
                    // We only allow NLJ/INLJ to take a base node on the RHS.
                    return false;
                }
                return rand.generateRandomBool();
            }
        };
        MONGO_UNREACHABLE_TASSERT(11458201);
    };

    // If we're given a specific method, use that for every join- otherwise, randomly pick between
    // available methods.
    auto pickMethod = [&rand,
                       &ctx](NodeId node, NodeSet prevNodeSet, bool isLeftChild, size_t level) {
        if (isLeftChild && level > 1) {
            // Only hash joins can have a base node on the left, unless we're at level 0/1.
            return JoinMethod::HJ;
        }

        // Only bother hinting INLJ if we could actually use INLJ (otherwise we will have many
        // retries).
        auto edges = ctx.joinGraph.getJoinEdges(NodeSet{}.set(node), prevNodeSet);
        bool canUseINLJ = std::any_of(edges.begin(), edges.end(), [&ctx, node](const auto& e) {
            const auto& edge = ctx.joinGraph.getEdge(e);
            return bestIndexSatisfyingJoinPredicates(ctx, node, edge) != nullptr;
        });

        const auto v = rand.generateUniformInt(0, canUseINLJ ? 2 : 1);
        switch (v) {
            case 0:
                return JoinMethod::NLJ;
            case 1:
                return JoinMethod::HJ;
            case 2:
                return JoinMethod::INLJ;
            default:
                break;
        }
        MONGO_UNREACHABLE_TASSERT(11458202);
    };

    // Pick the shape of the tree & the join methods used.
    std::vector<SubsetLevelMode> modes;
    modes.reserve(nodes.size());
    size_t level = 0;
    NodeSet prevSubset;
    for (auto&& node : nodes) {
        // NOTE: both of these are ignored for the first hint/ the base node.
        bool isLeftChild = pickChildSide();
        auto method =
            overrideMethod ? *overrideMethod : pickMethod(node, prevSubset, isLeftChild, level);
        modes.push_back({level,
                         // When a plan is fully hinted, we only have one option at each subset
                         // anyway- pick the cheapest one.
                         PlanEnumerationMode::CHEAPEST,
                         JoinHint{.node = node, .method = method, .isLeftChild = isLeftChild}});
        level++;
        prevSubset.set(node);
    }
    return modes;
}

}  // namespace

StatusWith<ReorderedJoinSolution> constructSolutionWithRandomOrder(
    const JoinReorderingContext& ctx,
    JoinCardinalityEstimator* estimator,
    JoinCostEstimator* coster,
    int seed,
    PlanTreeShape planShape,
    boost::optional<JoinMethod> method,
    bool enableHJOrderPruning,
    size_t maxRandomHintRetries) {
    random_utils::PseudoRandomGenerator rand(seed);

    // We always run once, then rety up to 'maxRandomHintTries' times.
    for (size_t tries = 0; tries <= maxRandomHintRetries; tries++) {
        if (tries > 0) {
            LOGV2_DEBUG(11458205, 5, "Hint failed, trying again", "try"_attr = tries);
        }
        auto mode = makeRandomizedEnumerationHint(ctx, rand, planShape, method);
        LOGV2_DEBUG(11458203,
                    5,
                    "Generated random order with hint",
                    "seed"_attr = seed,
                    "planShape"_attr = planShape,
                    "mode"_attr = mode,
                    "enableHJOrderPruning"_attr = enableHJOrderPruning);

        PlanEnumeratorContext peCtx(
            ctx,
            estimator,
            coster,
            EnumerationStrategy{.planShape = planShape,
                                .mode = std::move(mode),
                                .enableHJOrderPruning = enableHJOrderPruning});
        peCtx.enumerateJoinSubsets();

        if (peCtx.enumerationSuccessful()) {
            return makeReorderedJoinSoln(ctx, peCtx);
        }
    }

    return Status(ErrorCodes::QueryRejectedBySettings,
                  "The randomized enumerator was unable to generate a valid hint for plan "
                  "enumeration with the current settings");
}

StatusWith<ReorderedJoinSolution> constructSolutionBottomUp(const JoinReorderingContext& ctx,
                                                            JoinCardinalityEstimator& estimator,
                                                            JoinCostEstimator& coster,
                                                            EnumerationStrategy strategy) {

    PlanEnumeratorContext peCtx(ctx, &estimator, &coster, std::move(strategy));
    peCtx.enumerateJoinSubsets();
    if (!peCtx.enumerationSuccessful()) {
        return Status(
            ErrorCodes::QueryRejectedBySettings,
            "Expected the bottom-up enumerator to generate a solution, but found none with the "
            "provided enumeration settings");
    }

    return makeReorderedJoinSoln(ctx, peCtx);
}
}  // namespace mongo::join_ordering
