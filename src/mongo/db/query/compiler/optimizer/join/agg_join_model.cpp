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

#include "mongo/db/query/compiler/optimizer/join/agg_join_model.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/document_source_geo_near.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/query/compiler/optimizer/join/path_resolver.h"
#include "mongo/db/query/compiler/optimizer/join/predicate_extractor.h"
#include "mongo/db/query/util/disjoint_set.h"

#include <memory>
#include <utility>

namespace mongo::join_ordering {
namespace {
std::unique_ptr<Pipeline> createEmptyPipeline(
    const boost::intrusive_ptr<ExpressionContext>& sourceExpCtx) {
    auto expCtx = makeCopyFromExpressionContext(sourceExpCtx, sourceExpCtx->getNamespaceString());
    pipeline_factory::MakePipelineOptions opts;
    opts.attachCursorSource = false;
    std::vector<BSONObj> emptyPipeline;

    return pipeline_factory::makePipeline(emptyPipeline, expCtx, opts);
}

std::unique_ptr<CanonicalQuery> makeFullScanCQ(boost::intrusive_ptr<ExpressionContext> expCtx) {
    auto fcr = std::make_unique<FindCommandRequest>(expCtx->getNamespaceString());
    return std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = expCtx, .parsedFind = ParsedFindCommandParams{.findCommand = std::move(fcr)}});
}

StatusWith<std::unique_ptr<CanonicalQuery>> createCanonicalQuery(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    NamespaceString nss,
    std::unique_ptr<MatchExpression> expr) {
    auto pfc = ParsedFindCommand::withExistingFilter(expCtx,
                                                     nullptr,
                                                     std::move(expr),
                                                     std::make_unique<FindCommandRequest>(nss),
                                                     ProjectionPolicies::findProjectionPolicies());
    CanonicalQueryParams params{.expCtx = expCtx, .parsedFind = std::move(pfc.getValue())};
    return CanonicalQuery::make(std::move(params));
}

struct Predicates {
    std::unique_ptr<CanonicalQuery> canonicalQuery;
    std::vector<boost::intrusive_ptr<const Expression>> joinPredicates;
};

StatusWith<Predicates> extractPredicatesFromLookup(
    DocumentSourceLookUp* stage, boost::intrusive_ptr<ExpressionContext> pipelineExpCtx) {
    auto expCtx = stage->getSubpipelineExpCtx();
    if (stage->hasPipeline()) {
        stage = dynamic_cast<DocumentSourceLookUp*>(stage);
        auto ds = stage->getResolvedIntrospectionPipeline().peekFront();
        auto match = dynamic_cast<DocumentSourceMatch*>(ds);
        tassert(11317205, "expected $match stage as leading stage in subpipeline", match);
        // Attempt to split
        auto splitRes = splitJoinAndSingleCollectionPredicates(match->getMatchExpression(),
                                                               stage->getLetVariables());
        if (!splitRes.has_value()) {
            return Status(ErrorCodes::QueryFeatureNotAllowed,
                          "Encountered subpipeline with $match containing non-equijoin correlated "
                          "predicates");
        }

        std::unique_ptr<CanonicalQuery> cq;
        if (splitRes->singleTablePredicates) {
            auto swCq = createCanonicalQuery(
                pipelineExpCtx, stage->getFromNs(), std::move(splitRes->singleTablePredicates));
            if (!swCq.isOK()) {
                return swCq.getStatus();
            }
            cq = std::move(swCq.getValue());
        } else {
            cq = makeFullScanCQ(expCtx);
        }

        return {{
            .canonicalQuery = std::move(cq),
            .joinPredicates = std::move(splitRes->joinPredicates),
        }};
    }
    return {{.canonicalQuery = makeFullScanCQ(expCtx)}};
}

BSONObj resolvedPathToBSON(const ResolvedPath& rp) {
    return BSON("nodeId" << rp.nodeId << "fieldName" << rp.fieldName.fullPath());
}

std::vector<BSONObj> pipelineToBSON(const std::unique_ptr<Pipeline>& pipeline) {
    if (pipeline) {
        return pipeline->serializeToBson();
    } else {
        return {};
    }
}

bool isLookupEligible(const DocumentSourceLookUp& lookup) {
    if (!lookup.hasUnwindSrc()) {
        return false;
    }

    // TODO SERVER-116033: Support absorbed single-table additional filter predicates.
    if (lookup.hasAdditionalFilter()) {
        return false;
    }

    if (!lookup.hasPipeline()) {
        // A $lookup with no sub-pipeline is eligible.
        return true;
    }

    // If the $lookup has a sub-pipeline, then it may only contain a $match stage.
    return lookup.getResolvedIntrospectionPipeline().size() == 1 &&
        dynamic_cast<DocumentSourceMatch*>(lookup.getResolvedIntrospectionPipeline().peekFront());
}

/**
 * Find and add implicit (transitive) edges within the graph.
 * `maxNodes` is the maximum number of nodes allowed in a connected component to be used for
 * implicit edge finding.
 * Example: two edges A.a = B.b and B.b = C.c form an implicit edge A.a = C.c.
 */
void addImplicitEdges(MutableJoinGraph& graph,
                      const std::vector<ResolvedPath>& resolvedPaths,
                      size_t maxNodes) {
    DisjointSet ds{resolvedPaths.size()};
    for (const auto& edge : graph.edges()) {
        for (const auto& pred : edge.predicates) {
            if (pred.op == JoinPredicate::Eq) {
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
                graph.addSimpleEqualityEdge(nodeId, currentNodeId, pathId, currentPathId);
            }
            pathSet.push_back(currentPathId);
        }
    }
}

// Checked cast which performs a tassert if it fails.
template <typename U, typename V>
U tassert_cast(V* v) {
    auto ret = dynamic_cast<U>(v);
    if (!ret) {
        tasserted(11317202, "cast failed");
    }
    return ret;
}

// Compute the field path which the given variable ID refers to in the local collection of a
// $lookup. We assume that the given a set of let variables from $lookup all are defined to be
// simple FieldPaths (i.e.are of the form {foo: '$foo'}). This allows us resolve a variable to
// underlying FieldPath.
FieldPath localCollectionFieldPath(const std::vector<LetVariable>& letVars, Variables::Id id) {
    auto varIt =
        std::find_if(letVars.cbegin(), letVars.cend(), [&id](auto&& var) { return var.id == id; });
    tassert(
        11317201, "variable ID not found in given set of let variables", varIt != letVars.cend());
    auto& var = *varIt;
    auto localFieldPath = tassert_cast<const ExpressionFieldPath*>(var.expression.get());
    return localFieldPath->getFieldPathWithoutCurrentPrefix();
}

// Insert the given join predicates into the given join graph. Assumes that the join predicates are
// agg expressions of the form {$eq: ['$foreignCollFieldPath', '$$localCollVar']}.
void addExprJoinPredicates(MutableJoinGraph& graph,
                           const std::vector<boost::intrusive_ptr<const Expression>>& joinPreds,
                           PathResolver& pathResolver,
                           const std::vector<LetVariable>& letVars,
                           NodeId localColl,
                           NodeId foreignColl) {
    for (auto&& joinPred : joinPreds) {
        auto eqNode = tassert_cast<const ExpressionCompare*>(joinPred.get());
        auto left = tassert_cast<const ExpressionFieldPath*>(eqNode->getChildren()[0].get());
        auto right = tassert_cast<const ExpressionFieldPath*>(eqNode->getChildren()[1].get());

        boost::optional<PathId> localPath;
        boost::optional<PathId> foreignPath;

        if (left->isVariableReference()) {
            // LHS is referencing a field from local collection
            // RHS is referencing a field from the foreign collection
            localPath = pathResolver.addPath(
                localColl, localCollectionFieldPath(letVars, left->getVariableId()));
            foreignPath =
                pathResolver.addPath(foreignColl, right->getFieldPathWithoutCurrentPrefix());
        } else if (right->isVariableReference()) {
            // LHS is referencing a field from the foreign collection
            // RHS is referencing a field from local collection
            localPath = pathResolver.addPath(
                localColl, localCollectionFieldPath(letVars, right->getVariableId()));
            foreignPath =
                pathResolver.addPath(foreignColl, left->getFieldPathWithoutCurrentPrefix());
        } else {
            // We expect one of the children of the ExpressionCompare to be a variable and the other
            // to be a field path.
            MONGO_UNREACHABLE_TASSERT(11317203);
        }
        tassert(11317204,
                "expected to resolve both local and foreign paths",
                localPath.has_value() && foreignPath.has_value());
        // TODO SERVER-112608: Account for different semantics of $expr equality.
        graph.addSimpleEqualityEdge(localColl, foreignColl, *localPath, *foreignPath);
    }
}

}  // namespace

bool AggJoinModel::pipelineEligibleForJoinReordering(const Pipeline& pipeline) {
    // Pipelines starting with $geoNear are not eligible.
    if (!pipeline.getSources().empty() &&
        dynamic_cast<DocumentSourceGeoNear*>(pipeline.peekFront())) {
        return false;
    }

    // Since we can reorder base collections, any pipeline with even just one eligible $lookup +
    // $unwind pair could be eligible.
    return std::any_of(pipeline.getSources().begin(), pipeline.getSources().end(), [](auto ds) {
        if (auto* lookup = dynamic_cast<DocumentSourceLookUp*>(ds.get()); lookup) {
            return isLookupEligible(*lookup);
        }
        return false;
    });
}

StatusWith<AggJoinModel> AggJoinModel::constructJoinModel(const Pipeline& pipeline,
                                                          AggModelBuildParams buildParams) {
    // Try to create a CanonicalQuery. We begin by cloning the pipeline (this includes
    // sub-pipelines!) to ensure that if we bail out, this stays idempotent.
    // TODO SERVER-111383: We should see if we can make createCanonicalQuery() idempotent instead.
    auto expCtx = pipeline.getContext();
    const auto& nss = expCtx->getNamespaceString();
    auto clonedExpCtx = makeCopyFromExpressionContext(expCtx, nss);
    auto suffix = pipeline.clone(clonedExpCtx);

    auto swCQ = createCanonicalQuery(expCtx, nss, *suffix);
    if (!swCQ.isOK()) {
        // Bail out & return the failure status- we failed to generate a CanonicalQuery from a
        // pipeline prefix.
        return swCQ.getStatus();
    }

    // Initialize the JoinGraph & base NodeId.
    MutableJoinGraph graph{buildParams.joinGraphBuildParams};
    auto baseNodeId =
        graph.addNode(expCtx->getNamespaceString(), std::move(swCQ.getValue()), boost::none);
    if (!baseNodeId) {
        return Status(ErrorCodes::BadValue, "Failed to create a node for base collection");
    }

    auto prefix = createEmptyPipeline(suffix->getContext());
    std::vector<ResolvedPath> resolvedPaths;
    PathResolver pathResolver{*baseNodeId, resolvedPaths};

    // Go through the pipeline trying to find the maximal chain of join optimization eligible
    // $lookup+$unwinds pairs and turning them into CanonicalQueries. At the end only ineligible for
    // join optimization stages are left in the suffix.
    // If we already reach the maximum number of nodes and/or edges we bail out from building the
    // graph and put the remaining stages into the suffix.
    while (!suffix->getSources().empty() &&
           graph.numNodes() < buildParams.joinGraphBuildParams.maxNodesInJoin &&
           graph.numEdges() < buildParams.joinGraphBuildParams.maxEdgesInJoin) {
        auto* stage = suffix->getSources().front().get();
        if (auto* lookup = dynamic_cast<DocumentSourceLookUp*>(stage); lookup) {
            if (!isLookupEligible(*lookup)) {
                break;
            }

            // Attempt to extract join predicates and single table predicates from the $lookup
            // expressed as $expr in $match stage. If there is no subpipeline, this returns no join
            // predicates and a CanonicalQuery with empty predicate. If this returns a bad status,
            // then this extraction failed due to an inelgible stage/expression.
            auto swPreds = extractPredicatesFromLookup(lookup, lookup->getSubpipelineExpCtx());
            if (!swPreds.isOK()) {
                break;
            }

            auto foreignNodeId = graph.addNode(lookup->getFromNs(),
                                               std::move(swPreds.getValue().canonicalQuery),
                                               lookup->getAsField());

            if (!foreignNodeId) {
                return Status(ErrorCodes::BadValue, "Graph is too big: too many nodes");
            }

            // Add join predicate expressed as local/foreign field syntax to join graph.
            if (lookup->hasLocalFieldForeignFieldJoin()) {
                // The order of resolving the paths are important here: localPathId shouln't be
                // resolved to the foreign collection even if it is prefixed by the foreign
                // collection's embedPath.
                auto localPathId = pathResolver.resolve(*lookup->getLocalField());

                pathResolver.addNode(*foreignNodeId, lookup->getAsField());
                auto foreignPathId =
                    pathResolver.addPath(*foreignNodeId, *lookup->getForeignField());

                auto edgeId = graph.addSimpleEqualityEdge(
                    pathResolver[localPathId].nodeId, *foreignNodeId, localPathId, foreignPathId);
                if (!edgeId) {
                    // Cannot add an edge for existing nodes.
                    return Status(ErrorCodes::BadValue, "Graph is too big: too many edges");
                }
            } else {
                pathResolver.addNode(*foreignNodeId, lookup->getAsField());
            }

            // Add join predicates expressed as $expr in subpipelines to join graph.
            addExprJoinPredicates(graph,
                                  swPreds.getValue().joinPredicates,
                                  pathResolver,
                                  lookup->getLetVariables(),
                                  *baseNodeId,
                                  *foreignNodeId);

            auto next = suffix->popFront();
            if (prefix->getSources().empty()) {
                prefix->addInitialSource(std::move(next));
            } else {
                prefix->pushBack(std::move(next));
            }
        } else {
            // Unrecognized stage, give up on building a prefix.
            break;
        }
    }

    if (graph.numNodes() < 2) {
        // We need at least 1 eligible $lookup and a fully SBE-pushed-down prefix.
        return Status(ErrorCodes::QueryFeatureNotAllowed, "Join reordering not allowed");
    }

    addImplicitEdges(graph, resolvedPaths, buildParams.maxNumberNodesConsideredForImplicitEdges);

    return AggJoinModel(JoinGraph(std::move(graph)),
                        std::move(resolvedPaths),
                        std::move(prefix),
                        std::move(suffix));
}

BSONObj AggJoinModel::toBSON() const {
    BSONObjBuilder result{};
    result.append("graph", graph.toBSON());
    {
        BSONArrayBuilder ab{};
        for (const auto& rp : resolvedPaths) {
            ab.append(resolvedPathToBSON(rp));
        }
        result.append("resolvedPaths", ab.arr());
    }
    result.append("prefix", pipelineToBSON(prefix));
    result.append("suffix", pipelineToBSON(suffix));
    return result.obj();
}
}  // namespace mongo::join_ordering
