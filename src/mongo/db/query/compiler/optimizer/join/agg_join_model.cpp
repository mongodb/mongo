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
#include "mongo/db/exec/classic/subplanning_utils.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/document_source_geo_near.h"
#include "mongo/db/pipeline/document_source_internal_join_hint.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/query/compiler/dependency_analysis/pipeline_dependency_graph.h"
#include "mongo/db/query/compiler/optimizer/join/path_resolver.h"
#include "mongo/db/query/compiler/optimizer/join/predicate_extractor.h"
#include "mongo/db/query/compiler/optimizer/join/predicate_inferer.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/util/disjoint_set.h"
#include "mongo/util/assert_util.h"

#include <ios>
#include <memory>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

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

struct Predicates {
    std::unique_ptr<CanonicalQuery> canonicalQuery;
    std::vector<JoinPredicateExpr> joinPredicates;
};

StatusWith<Predicates> extractPredicatesFromLookup(DocumentSourceLookUp& stage) {
    auto expCtx = stage.getSubpipelineExpCtx();

    std::vector<JoinPredicateExpr> joinPredicates;
    std::unique_ptr<MatchExpression> singleTablePredicates;

    if (!stage.getResolvedIntrospectionPipeline().empty()) {
        auto ds = stage.getResolvedIntrospectionPipeline().peekFront();
        auto match = dynamic_cast<DocumentSourceMatch*>(ds);
        tassert(11317205, "expected $match stage as leading stage in subpipeline", match);
        // Attempt to split.
        auto splitRes = splitJoinAndSingleCollectionPredicates(match->getMatchExpression(),
                                                               stage.getLetVariables());
        if (!splitRes.has_value()) {
            return Status(ErrorCodes::QueryFeatureNotAllowed,
                          "Encountered subpipeline with $match containing non-equijoin correlated "
                          "predicates");
        }
        joinPredicates = std::move(splitRes->joinPredicates);
        singleTablePredicates = std::move(splitRes->singleTablePredicates);
    }

    // Absorbed filter, reachable via getAbsorbedFilter() for both pipeline-form and non-pipeline
    // $lookups. For pipeline-form $lookups the introspection pipeline above may have produced a
    // singleTablePredicates from the user's sub-pipeline $match; combine the two with $and.
    if (stage.hasAdditionalFilter()) {
        auto swExpr = MatchExpressionParser::parse(stage.getAbsorbedFilter(),
                                                   stage.getSubpipelineExpCtx(),
                                                   ExtensionsCallbackNoop(),
                                                   Pipeline::kAllowedMatcherFeatures);
        if (!swExpr.isOK()) {
            return swExpr.getStatus();
        }
        if (singleTablePredicates) {
            // We construct the AND by first adding the sub-pipeline $match (singleTablePredicates),
            // followed by the absorbed filter's $match. Note that downstream CanonicalQuery
            // construction stable-sorts the children of $and by MatchExpression type (with field
            // path as a secondary tiebreak within the same type), so the order here may not be
            // maintained.
            singleTablePredicates =
                makeAnd(std::move(singleTablePredicates), std::move(swExpr.getValue()));
        } else {
            singleTablePredicates = std::move(swExpr.getValue());
        }
    }
    // If neither branch is hit, then this a non-pipeline or empty subpipeline (pipeline: []) lookup
    // with no absorbed filters. In this case, 'joinPredicates' and 'singleTablePredicates' stay
    // empty and the foreign CQ is the full scan.

    std::unique_ptr<CanonicalQuery> cq;
    if (singleTablePredicates) {
        auto swCq = createCanonicalQueryFromSingleMatchExpression(
            expCtx, stage.getFromNs(), std::move(singleTablePredicates));
        if (!swCq.isOK()) {
            return swCq.getStatus();
        }
        cq = std::move(swCq.getValue());
    } else {
        cq = makeFullScanCQ(expCtx);
    }

    return {{
        .canonicalQuery = std::move(cq),
        .joinPredicates = std::move(joinPredicates),
    }};
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

bool isUnwindEligible(const DocumentSourceUnwind& unwind) {
    // If 'preserveNullAndEmptyArrays' is set to true, this is an outer join, which is currently
    // ineligible for join-opt. Similarly, we don't support $unwinds that set the array index.
    return !unwind.preserveNullAndEmptyArrays() && !unwind.indexPath();
}

bool isLookupEligible(const DocumentSourceLookUp& lookup) {
    if (lookup.getExpCtx()->getSubPipelineDepth() != 0) {
        // We've descended into a subpipelined, fallback.
        return false;
    }

    if (!lookup.hasUnwindSrc() || !isUnwindEligible(*lookup.getUnwindSource())) {
        return false;
    }

    // $lookup specified with localField/foreignField only (no pipeline spec). An absorbed filter,
    // if any, is reachable via getAbsorbedFilter() and handled in extractPredicatesFromLookup().
    if (!lookup.hasPipeline()) {
        return true;
    }

    // pipeline:[] passes this check — the absorbed filter, if any, is read via
    // getAbsorbedFilter() in extractPredicatesFromLookup(). Disconnected graphs (no join
    // predicate from any source) are rejected later by constructJoinModel.
    if (lookup.getResolvedIntrospectionPipeline().empty()) {
        return true;
    }

    // Otherwise the sub-pipeline must contain a single $match stage. The absorbed filter (if any)
    // is combined with that $match in extractPredicatesFromLookup().
    return lookup.getResolvedIntrospectionPipeline().size() == 1 &&
        dynamic_cast<DocumentSourceMatch*>(lookup.getResolvedIntrospectionPipeline().peekFront());
}

bool canJoinPredicateFieldIncludeArrays(
    const pipeline::dependency_graph::DependencyGraph& baseCollDeps,
    ExpressionContext* expCtx,
    const DocumentSource* ds,
    const NamespaceString& ns,
    const FieldPath& field) {
    if (ns == expCtx->getNamespaceString()) {
        return baseCollDeps.canPathBeArray(ds, field.fullPath());
    }
    // TODO SERVER-126992: Use a dependency graph instead of directly accessing foreign path
    // arrayness.
    return expCtx->canPathBeArrayForNss(field, ns);
}

/**
 * Validates that neither field in the join predicate can include arrays.
 */
bool canJoinPredicateIncludeArrays(const pipeline::dependency_graph::DependencyGraph& baseCollDeps,
                                   ExpressionContext* expCtx,
                                   const DocumentSource* ds,
                                   const NamespaceString& leftNs,
                                   const FieldPath& leftField,
                                   const NamespaceString& rightNs,
                                   const FieldPath& rightField) {
    return canJoinPredicateFieldIncludeArrays(baseCollDeps, expCtx, ds, leftNs, leftField) ||
        canJoinPredicateFieldIncludeArrays(baseCollDeps, expCtx, ds, rightNs, rightField);
}

bool hasNumericPathComponent(const FieldPath& fp) {
    for (size_t i = 0; i < fp.getPathLength(); ++i) {
        if (FieldRef::isNumericPathComponentLenient(fp.getFieldName(i))) {
            return true;
        }
    }
    return false;
}

}  // namespace

bool AggJoinModel::pipelineEligibleForJoinReordering(const Pipeline& pipeline) {
    // We don't support non-simple collations.
    if (!CollatorInterface::isSimpleCollator(pipeline.getContext()->getCollator())) {
        return false;
    }

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

    boost::intrusive_ptr<DocumentSource> hint;
    if (dynamic_cast<DocumentSourceInternalJoinHint*>(suffix->peekFront())) {
        // Remove hint stage from pipeline if present.
        hint = suffix->popFront();
    }

    // Initialize deps after popping the $hint stage, but BEFORE we try to push a pipeline prefix
    // into our base collection CQ. This is important so we don't miss (for instance) $projects at
    // the start of the pipeline that might rename fields.
    auto canMainCollPathBeArray = [clonedExpCtx, &nss](StringData path) {
        return clonedExpCtx->canPathBeArrayForNss(FieldRef(path), nss);
    };
    pipeline::dependency_graph::DependencyGraph mainCollDeps(suffix->getSources(),
                                                             canMainCollPathBeArray);

    ExpressionContext::PlanCacheOptions oldPlanCache = expCtx->getPlanCache();
    expCtx->setPlanCache(ExpressionContext::PlanCacheOptions::kDisablePlanCache);
    auto swCQ = createCanonicalQuery(expCtx, nss, *suffix);

    expCtx->setPlanCache(oldPlanCache);

    if (!swCQ.isOK()) {
        // Bail out & return the failure status- we failed to generate a CanonicalQuery from a
        // pipeline prefix.
        return swCQ.getStatus();
    }

    if (SubPlanningUtils::canUseSubplanning(*swCQ.getValue())) {
        return Status(ErrorCodes::QueryFeatureNotAllowed,
                      "Encountered rooted $or, can't use subplanning together with join opt");
    }

    if (swCQ.getValue()->getSortPattern()) {
        return Status(ErrorCodes::BadValue, "Sort stage found in pipeline");
    }
    // Initialize the JoinGraph & base NodeId.
    MutableJoinGraph graph{buildParams.joinGraphBuildParams};
    auto baseNodeId =
        graph.addNode(expCtx->getNamespaceString(), std::move(swCQ.getValue()), boost::none);
    if (!baseNodeId) {
        return Status(ErrorCodes::BadValue, "Failed to create a node for base collection");
    }

    auto prefix = createEmptyPipeline(suffix->getContext());
    if (hint) {
        // Keep hint on the pipeline prefix.
        prefix->pushBack(std::move(hint));
    }

    std::vector<ResolvedPath> resolvedPaths;
    PathResolver pathResolver{*baseNodeId, resolvedPaths};

    const auto isJoinPredicateIneligible = [&](const NamespaceString& leftNs,
                                               const FieldPath& leftField,
                                               const NamespaceString& rightNs,
                                               const FieldPath& rightField,
                                               const DocumentSource* src) {
        // Ensures join predicate fields don't include numeric path components and can't include
        // arrays.
        return hasNumericPathComponent(leftField) || hasNumericPathComponent(rightField) ||
            canJoinPredicateIncludeArrays(
                   mainCollDeps, clonedExpCtx.get(), src, leftNs, leftField, rightNs, rightField);
    };

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

            if (pathResolver.pathResolvesToJoinNode(lookup->getAsField(), *baseNodeId)) {
                break;
            }

            if (lookup->hasLocalFieldForeignFieldJoin() &&
                isJoinPredicateIneligible(expCtx->getNamespaceString(),
                                          *lookup->getLocalField(),
                                          lookup->getFromNs(),
                                          *lookup->getForeignField(),
                                          lookup)) {
                // End prefix here, this join predicate is invalid.
                break;
            }

            // Attempt to extract join predicates and single table predicates from the $lookup
            // expressed as $expr in $match stage. If there is no subpipeline, this returns no join
            // predicates and a CanonicalQuery with empty predicate. If this returns a bad status,
            // then this extraction failed due to an ineligible stage/expression.
            auto swPreds = extractPredicatesFromLookup(*lookup);
            if (!swPreds.isOK()) {
                break;
            }
            auto preds = std::move(swPreds.getValue());

            // Similar check as above, but now for predicates extracted from the sub-pipeline.
            if (std::any_of(
                    preds.joinPredicates.begin(), preds.joinPredicates.end(), [&](auto&& jp) {
                        return isJoinPredicateIneligible(expCtx->getNamespaceString(),
                                                         jp.localField(),
                                                         lookup->getFromNs(),
                                                         jp.foreignField(),
                                                         lookup);
                    })) {
                // Some field in a join predicate introduced by a $expr $match in a sub-pipeline
                // might be ineligible. End prefix here.
                break;
            }

            // If we get here, it means we're ready to modify the join graph to include this
            // $lookup. Once the join graph has been modified, any failure case should cause us to
            // bail out of join optimization completely, rather than just ending the prefix here
            // (since we've already partially incorporated the current join).

            auto foreignNodeId = graph.addNode(
                lookup->getFromNs(), std::move(preds.canonicalQuery), lookup->getAsField());

            if (!foreignNodeId) {
                return Status(ErrorCodes::BadValue, "Graph is too big: too many nodes");
            }

            // Resolve all local-side join fields BEFORE adding the foreign node to the path
            // resolver. The order is important: a local reference is evaluated against the input
            // document before the foreign results are embedded at the "as" field, so it must
            // resolve to the base/earlier node that owns it -- even when it is prefixed by the
            // foreign collection's embedPath (e.g. 'let: {l: "$X.y"}' combined with 'as: "X"').
            // Resolving it after the foreign node has been added would misattribute the local
            // field to the foreign node and produce an illegal self-edge.
            boost::optional<PathId> localFieldPathId;
            if (lookup->hasLocalFieldForeignFieldJoin()) {
                localFieldPathId = pathResolver.resolve(*lookup->getLocalField());
                if (!localFieldPathId) {
                    return Status(ErrorCodes::BadValue, "Local path could not be resolved");
                }
            }

            std::vector<PathId> exprLocalPathIds;
            exprLocalPathIds.reserve(preds.joinPredicates.size());
            for (const auto& joinPred : preds.joinPredicates) {
                auto localPathId = pathResolver.resolve(joinPred.localField());
                if (!localPathId) {
                    return Status(ErrorCodes::BadValue, "Local path could not be resolved");
                }
                exprLocalPathIds.push_back(*localPathId);
            }

            // Now register the foreign node's scope. Foreign-side fields are attributed to it
            // explicitly via addPath(), so the foreign node only needs to be in the resolver from
            // this point on.
            pathResolver.addNode(*foreignNodeId, lookup->getAsField());

            // Add join predicate expressed as local/foreign field syntax to join graph.
            if (lookup->hasLocalFieldForeignFieldJoin()) {
                auto foreignPathId =
                    pathResolver.addPath(*foreignNodeId, *lookup->getForeignField());

                auto edgeId = graph.addSimpleEqualityEdge(pathResolver[*localFieldPathId].nodeId,
                                                          *foreignNodeId,
                                                          *localFieldPathId,
                                                          foreignPathId);
                if (!edgeId) {
                    // Cannot add an edge for existing nodes.
                    return Status(ErrorCodes::BadValue, "Graph is too big: too many edges");
                }
            }

            // Add join predicates expressed as $expr in subpipelines to join graph.
            for (size_t i = 0; i < preds.joinPredicates.size(); ++i) {
                PathId foreignPathId =
                    pathResolver.addPath(*foreignNodeId, preds.joinPredicates[i].foreignField());
                auto localNodeId = pathResolver[exprLocalPathIds[i]].nodeId;
                graph.addExprEqualityEdge(
                    localNodeId, *foreignNodeId, exprLocalPathIds[i], foreignPathId);
            }

            auto next = suffix->popFront();
            if (prefix->getSources().empty()) {
                prefix->addInitialSource(std::move(next));
            } else {
                prefix->pushBack(std::move(next));
            }

        } else if (auto* match = dynamic_cast<DocumentSourceMatch*>(stage); match) {
            tassert(11116400, "unexpected $match", !prefix->getSources().empty());

            auto result = extractExprPredicates(pathResolver, match->getMatchExpression());
            bool canMatchBeEliminated = result.expressionIsFullyAbsorbed;
            for (const auto& predicate : result.predicates) {
                auto leftNodeId = pathResolver[predicate.left].nodeId;
                auto rightNodeId = pathResolver[predicate.right].nodeId;
                tassert(11116401,
                        "Join predicate fields must be from different nodes",
                        leftNodeId != rightNodeId);

                if (isJoinPredicateIneligible(graph.getNode(leftNodeId).collectionName,
                                              pathResolver[predicate.left].fieldName,
                                              graph.getNode(rightNodeId).collectionName,
                                              pathResolver[predicate.right].fieldName,
                                              match)) {
                    // Some field in a join predicate introduced by this trailing $match is
                    // ineligible. Don't absorb it.
                    canMatchBeEliminated = false;
                    continue;
                }

                graph.addEdge(leftNodeId, rightNodeId, {predicate});
            }

            if (!canMatchBeEliminated) {
                // End prefix here- this $match includes something we can't push into the join
                // model.
                break;
            }

            // This $match encodes a valid join-predicate & can be fully absorbed into our
            // join-graph.
            prefix->pushBack(suffix->popFront());

        } else {
            // Unrecognized stage, give up on building a prefix.
            break;
        }
    }

    if (graph.numNodes() < 2) {
        // We need at least 1 eligible $lookup and a fully SBE-pushed-down prefix.
        return Status(ErrorCodes::QueryFeatureNotAllowed, "Join reordering not allowed");
    }

    auto swVec = addImplicitEdgesAndInferPredicates(
        graph, resolvedPaths, buildParams.maxNumberNodesConsideredForImplicitEdges, expCtx);
    if (!swVec.isOK()) {
        return swVec.getStatus();
    }

    JoinGraph result = JoinGraph(std::move(graph));
    if (!result.isConnected()) {
        return Status(ErrorCodes::InternalErrorNotSupported,
                      "Join graph must be connected as cross-products are not yet supported");
    }
    return AggJoinModel(std::move(result),
                        std::move(resolvedPaths),
                        std::move(prefix),
                        std::move(suffix),
                        std::move(swVec.getValue()));
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
