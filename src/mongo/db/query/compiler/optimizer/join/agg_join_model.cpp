// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/optimizer/join/agg_join_model.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/classic/subplanning_utils.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_geo_near.h"
#include "mongo/db/pipeline/document_source_internal_join_hint.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/query/compiler/dependency_analysis/pipeline_dependency_graph.h"
#include "mongo/db/query/compiler/optimizer/join/path_resolver.h"
#include "mongo/db/query/compiler/optimizer/join/predicate_extractor.h"
#include "mongo/db/query/compiler/optimizer/join/predicate_inferer.h"
#include "mongo/db/query/compiler/optimizer/join/server_status_metrics.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

#include <memory>
#include <string_view>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::join_ordering {

// Sleeps for {ms: <millis>} while building the join model, so that tests can verify that
// 'joinModelingTimeMicros' measures this phase.
MONGO_FAIL_POINT_DEFINE(sleepWhileBuildingJoinModel);

namespace {
std::unique_ptr<Pipeline> createEmptyPipeline(
    const boost::intrusive_ptr<ExpressionContext>& sourceExpCtx) {
    auto expCtx = makeCopyFromExpressionContext(sourceExpCtx, sourceExpCtx->getNamespaceString());
    pipeline_factory::MakePipelineOptions opts;
    opts.attachCursorSource = false;
    std::vector<BSONObj> emptyPipeline;

    return pipeline_factory::makePipeline(emptyPipeline, expCtx, opts);
}

DocumentSource* getFirstSubpipelineStage(const DocumentSourceLookUp& lookup) {
    if (!lookup.getResolvedIntrospectionPipeline().empty()) {
        return lookup.getResolvedIntrospectionPipeline().peekFront();
    }
    return nullptr;
}

StatusWith<std::unique_ptr<CanonicalQuery>> createCQForJoinPipeline(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    Pipeline& pipeline,
    boost::optional<JoinFallbackReason>& reason) {
    // Ensure that we disable classic plan cache use for CQ generation, but reset this if it turns
    // out we need to bail.
    ExpressionContext::PlanCacheOptions oldPlanCache = expCtx->getPlanCache();
    expCtx->setPlanCache(ExpressionContext::PlanCacheOptions::kDisablePlanCache);
    ON_BLOCK_EXIT([&]() { expCtx->setPlanCache(oldPlanCache); });

    auto swCQ = createCanonicalQuery(expCtx, expCtx->getNamespaceString(), pipeline);
    if (!swCQ.isOK()) {
        reason = JoinFallbackReason::kFailedToCreateCQ;
        return swCQ;
    }

    // Mark the CQ as SBE-compatible to work around the check in 'shouldCacheQuery()' that prevents
    // caching of non-sbe collscan plans.
    swCQ.getValue()->setSbeCompatible(true);

    if (SubPlanningUtils::canUseSubplanning(*swCQ.getValue())) {
        reason = JoinFallbackReason::kRootedOrSubplanning;
        return Status(ErrorCodes::QueryFeatureNotAllowed,
                      "Encountered rooted $or, can't use subplanning together with join opt");
    }

    return swCQ;
}

struct Predicates {
    std::unique_ptr<CanonicalQuery> canonicalQuery;
    std::vector<JoinPredicate> joinPredicates;
};

/**
 * Given a predicate an dits source $lookup (may be null if trailing $match) resolve the join
 * predicate fields into a JoinPredicate that can be added to an edge in the JoinGraph. Returns
 * boost::none if we fail to resolve either path.
 */
boost::optional<JoinPredicate> resolve(const ExtractedJoinPredicate& predicate,
                                       const DocumentSourceLookUp* lookup,
                                       PathResolver& pathResolver,
                                       NodeId foreignNodeId,
                                       boost::optional<JoinFallbackReason>& fallbackReason) {
    // We're not sure which collection this field came from- don't specify a node scope.
    // "local" is a misnomer- it really means "as seen in top-level pipeline".
    auto localPathId =
        pathResolver.resolve(predicate.localField(), lookup, boost::none, fallbackReason);
    if (!localPathId) {
        return boost::none;
    }

    // Ensure that for local/foreign field join, we specify that this is at the start of the foreign
    // collection pipeline- otherwise we just evaluate dependencies from where the $expr predicate
    // was found.
    auto foreignPathId = pathResolver.resolve(
        predicate.foreignField(),
        predicate.isExpr() ? predicate.source() : getFirstSubpipelineStage(*lookup),
        foreignNodeId,
        fallbackReason);
    if (!foreignPathId) {
        return boost ::none;
    }
    return JoinPredicate{.op = predicate.isExpr() ? JoinPredicate::Operator::ExprEq
                                                  : JoinPredicate::Operator::Eq,
                         .left = *localPathId,
                         .right = *foreignPathId};
}

/**
 * $lookups can encode predicates in one of two ways: local/foreign field joins, and/or $expr/$eq
 * expressions within its subpipeline. This function finds and resolves all such predicates. If for
 * some reason this $lookup includes ineligible nested sub-pipeline stages, or if the predicate
 * fields are invalid, this function returns a bad status to signal that we can't push this $lookup
 * into the agg join model prefix, and sets 'reason' for fallback metrics tracking.
 */
StatusWith<Predicates> extractPredicatesFromLookup(const DocumentSourceLookUp& lookup,
                                                   PathResolver& pathResolver,
                                                   NodeId foreignNodeId,
                                                   boost::optional<JoinFallbackReason>& reason) {
    std::vector<JoinPredicate> joinPredicates;
    std::unique_ptr<MatchExpression> singleTablePredicates;

    if (lookup.hasLocalFieldForeignFieldJoin()) {
        auto resolved = resolve(ExtractedJoinPredicate::make(
                                    *lookup.getLocalField(), *lookup.getForeignField(), &lookup),
                                &lookup,
                                pathResolver,
                                foreignNodeId,
                                reason);
        if (!resolved) {
            return Status(ErrorCodes::BadValue, "Could not resolve local or foreign $lookup paths");
        }
        joinPredicates.push_back(*resolved);
    }

    // Note: we may have other stages here. If we do, that just means we can't extract join
    // predicates from the subpipeline (this is fine).
    auto start = lookup.getSubPipeline()->begin();
    if (auto match = dynamic_cast<DocumentSourceMatch*>(getFirstSubpipelineStage(lookup)); match) {
        // Attempt to split.
        auto splitRes = splitJoinAndSingleCollectionPredicates(match, lookup.getLetVariables());
        if (!splitRes.has_value()) {
            reason = JoinFallbackReason::kNonEquijoinCorrelatedPredicate;
            return Status(ErrorCodes::QueryFeatureNotAllowed,
                          "Encountered subpipeline with $match containing non-equijoin correlated "
                          "predicates");
        }

        joinPredicates.reserve(joinPredicates.size() + splitRes->joinPredicates.size());
        for (const auto& predicate : splitRes->joinPredicates) {
            auto resolved = resolve(predicate, &lookup, pathResolver, foreignNodeId, reason);
            if (!resolved) {
                return Status(ErrorCodes::BadValue,
                              "Could not resolve local or foreign $lookup paths");
            }
            joinPredicates.push_back(std::move(resolved.value()));
        }

        singleTablePredicates = std::move(splitRes->singleTablePredicates);
        start++;  // Ignore starting $match now that we've extracted it.
    }

    // Absorbed filter, reachable via getAbsorbedFilter() for both pipeline-form and non-pipeline
    // $lookups. For pipeline-form $lookups the introspection pipeline above may have produced a
    // singleTablePredicates from the user's sub-pipeline $match; combine the two with $and.
    if (lookup.hasAdditionalFilter()) {
        auto swExpr = MatchExpressionParser::parse(lookup.getAbsorbedFilter(),
                                                   lookup.getSubpipelineExpCtx(),
                                                   ExtensionsCallbackNoop(),
                                                   Pipeline::kAllowedMatcherFeatures);
        tassert(13199201,
                str::stream() << "Failed to parse absorbed filter: " << swExpr.getStatus(),
                swExpr.isOK());
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

    // Recreate the pipeline with just our STPs & any CBR + SBE eligible subpipeline.
    auto cqExpCtx = lookup.getSubpipelineExpCtx();
    auto pipelineForCQ =
        Pipeline::create(DocumentSourceContainer(start, lookup.getSubPipeline()->end()), cqExpCtx);
    if (singleTablePredicates) {
        pipelineForCQ->addInitialSource(
            make_intrusive<DocumentSourceMatch>(std::move(singleTablePredicates), cqExpCtx));
    }

    auto swCq = createCQForJoinPipeline(cqExpCtx, *pipelineForCQ, reason);
    if (!swCq.isOK()) {
        return swCq.getStatus();
    }

    if (!pipelineForCQ->empty()) {
        // We bail out if the entire sub-pipeline can't be pushed into a CQ.
        reason = JoinFallbackReason::kUnsupportedStage;
        return Status(ErrorCodes::QueryFeatureNotAllowed, "Encountered complex sub-pipeline");
    }

    return {{
        .canonicalQuery = std::move(swCq.getValue()),
        .joinPredicates = std::move(joinPredicates),
    }};
}

BSONObj resolvedPathToBSON(const ResolvedPath& rp) {
    BSONObjBuilder bob;
    bob << "nodeId" << rp.nodeId << "underlyingFieldPath" << rp.underlyingFieldPath.fullPath();
    if (rp.fieldPathAfterRenames) {
        bob << "fieldPathAfterRenames" << rp.fieldPathAfterRenames->fullPath();
    }
    return bob.obj();
}

std::vector<BSONObj> pipelineToBSON(const std::unique_ptr<Pipeline>& pipeline) {
    if (pipeline) {
        return pipeline->serializeToBson();
    } else {
        return {};
    }
}

/**
 * The helpers below return the reason the pipeline element is ineligible for join optimization, or
 * boost::none if it is eligible.
 */
boost::optional<JoinFallbackReason> isUnwindEligible(const DocumentSourceUnwind& unwind) {
    // If 'preserveNullAndEmptyArrays' is set to true, this is an outer join, which is currently
    // ineligible for join-opt. Similarly, we don't support $unwinds that set the array index.
    if (unwind.preserveNullAndEmptyArrays()) {
        return JoinFallbackReason::kOuterJoinUnwind;
    }
    if (unwind.indexPath()) {
        return JoinFallbackReason::kUnwindIncludeArrayIndex;
    }
    return boost::none;
}

bool isSubPipelineOrPrefixEligible(auto start, auto end) {
    return std::all_of(start, end, [](const auto& docSrc) {
        return dynamic_cast<DocumentSourceMatch*>(docSrc.get()) ||
            dynamic_cast<DocumentSourceProject*>(docSrc.get()) ||
            dynamic_cast<DocumentSourceSingleDocumentTransformation*>(docSrc.get()) ||
            dynamic_cast<DocumentSourceAddFields*>(docSrc.get());
    });
}

boost::optional<JoinFallbackReason> isLookupEligible(const DocumentSourceLookUp& lookup) {
    if (lookup.getExpCtx()->getSubPipelineDepth() != 0) {
        // We've descended into a subpipeline, fallback.
        return JoinFallbackReason::kInsideSubPipeline;
    }

    if (!lookup.hasUnwindSrc()) {
        return JoinFallbackReason::kLookupNotUnwound;
    }
    if (auto reason = isUnwindEligible(*lookup.getUnwindSource())) {
        return reason;
    }

    // $lookup specified with localField/foreignField only (no pipeline spec). An absorbed filter,
    // if any, is reachable via getAbsorbedFilter() and handled in extractPredicatesFromLookup().
    if (!lookup.hasPipeline()) {
        return boost::none;
    }

    // pipeline:[] passes this check — the absorbed filter, if any, is read via
    // getAbsorbedFilter() in extractPredicatesFromLookup(). Disconnected graphs (no join
    // predicate from any source) are rejected later by constructJoinModel.
    if (lookup.getResolvedIntrospectionPipeline().empty()) {
        return boost::none;
    }

    // Otherwise the sub-pipeline must contain a single $match stage. The absorbed filter (if any)
    // is combined with that $match in extractPredicatesFromLookup().
    if (!isSubPipelineOrPrefixEligible(
            lookup.getResolvedIntrospectionPipeline().getSources().begin(),
            lookup.getResolvedIntrospectionPipeline().getSources().end())) {
        return JoinFallbackReason::kIneligibleSubPipelineStage;
    }
    return boost::none;
}

bool addJoinPredicates(const std::vector<JoinPredicate>& joinPreds,
                       const std::vector<ResolvedPath>& resolved,
                       MutableJoinGraph& graph,
                       OpDebug::JoinOptimizationMetrics& metrics) {
    for (const auto& predicate : joinPreds) {
        auto leftNodeId = resolved[predicate.left].nodeId;
        auto rightNodeId = resolved[predicate.right].nodeId;
        tassert(11116401,
                "Join predicate fields must be from different nodes",
                leftNodeId != rightNodeId);
        if (!graph.addEdge(leftNodeId, rightNodeId, {predicate})) {
            metrics.fallbackReason = JoinFallbackReason::kTooManyEdgesOrPredicates;
            return false;
        }

        if (predicate.op == JoinPredicate::Eq) {
            metrics.numSyntacticEqJoinPredicates++;
        } else {
            tassert(13208600, "Expected an $expr edge", predicate.op == JoinPredicate::ExprEq);
            metrics.numSyntacticExprJoinPredicates++;
        }
    }
    return true;
}

}  // namespace

bool AggJoinModel::pipelineEligibleForJoinReordering(const Pipeline& pipeline) {
    auto startIt = pipeline.getSources().begin();

    // Permit a leading join hint- we'll check it later.
    if (dynamic_cast<DocumentSourceInternalJoinHint*>(startIt->get())) {
        startIt++;
    }

    bool foundLookup = false;
    auto it = startIt;
    while (it != pipeline.getSources().end()) {
        if (auto* lookup = dynamic_cast<DocumentSourceLookUp*>(it->get()); lookup) {
            // Found first $lookup- if prefix not valid, or if $lookup itself is not eligible,
            // bail!
            if (auto reason = isLookupEligible(*lookup)) {
                joinOptMetrics.fallbackReasons.increment(*reason);
                return false;
            }
            if (!isSubPipelineOrPrefixEligible(startIt, it)) {
                joinOptMetrics.fallbackReasons.increment(
                    JoinFallbackReason::kIneligiblePrefixStage);
                return false;
            }
            foundLookup = true;
            // One eligible $lookup is enough to proceed.
            break;
        }
        it++;
    }

    if (!foundLookup) {
        joinOptMetrics.fallbackReasons.increment(JoinFallbackReason::kNoLookup);
        return false;
    }

    // Note: we do additional checks after the query shape check, since that's the most important
    // reason we might bail.

    // We don't support non-simple collations.
    if (!CollatorInterface::isSimpleCollator(pipeline.getContext()->getCollator())) {
        joinOptMetrics.fallbackReasons.increment(JoinFallbackReason::kPipelineCollation);
        return false;
    }

    return true;
}

StatusWith<AggJoinModel> AggJoinModel::constructJoinModel(
    const Pipeline& pipeline,
    AggModelBuildParams buildParams,
    OpDebug::JoinOptimizationMetrics& metrics) {
    // Try to create a CanonicalQuery. We begin by cloning the pipeline (this includes
    // sub-pipelines!) to ensure that if we bail out, this stays idempotent.
    // TODO SERVER-111383: We should see if we can make createCanonicalQuery() idempotent instead.
    // 'expCtx' is the original pipeline's context; do not use it below except for the temporary
    // plan cache adjustment around createCanonicalQuery. All join optimization work uses
    // 'clonedExpCtx' so that bail-outs leave 'expCtx' unchanged.
    auto expCtx = pipeline.getContext();

    // Count number of unique namespaces involved in join graph prefix for metrics collection
    // purposes. Ensure that we update this metric for any exit path, be it a fallback or a
    // successful AggJoinModel construction. The same goes for the modeling time: we record it even
    // if model construction fails, so that the cost of an unsuccessful join-optimization attempt is
    // visible.
    absl::flat_hash_set<NamespaceString> uniqueNamespaces;
    Timer joinModelingTimer;
    ON_BLOCK_EXIT([&]() {
        metrics.numNamespaces = uniqueNamespaces.size();
        metrics.joinModelingTimeMicros = joinModelingTimer.micros();
    });
    sleepWhileBuildingJoinModel.execute(
        [](const BSONObj& data) { sleepmillis(data["ms"].numberInt()); });

    const auto& nss = expCtx->getNamespaceString();
    uniqueNamespaces.insert(nss);
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
    auto canMainCollPathBeArray = [clonedExpCtx, &nss](std::string_view path) {
        return clonedExpCtx->canPathBeArrayForNss(FieldRef(path), nss);
    };
    pipeline::dependency_graph::DependencyGraph mainCollDeps(suffix->getSources(),
                                                             canMainCollPathBeArray);

    auto swCQ = createCQForJoinPipeline(expCtx, *suffix, metrics.fallbackReason);
    if (!swCQ.isOK()) {
        // Bail out & return the failure status- we failed to generate a CanonicalQuery from a
        // pipeline prefix.
        return swCQ.getStatus();
    }

    // Initialize the JoinGraph & base NodeId.
    MutableJoinGraph graph{buildParams.joinGraphBuildParams};
    auto baseNodeId = graph.addNode(nss, std::move(swCQ.getValue()), boost::none);
    tassert(13199200, "Unable to create base node", baseNodeId);

    auto prefix = createEmptyPipeline(suffix->getContext());
    if (hint) {
        // Keep hint on the pipeline prefix.
        prefix->pushBack(std::move(hint));
    }

    PathResolver pathResolver{*baseNodeId, mainCollDeps};

    // Go through the pipeline trying to find the maximal chain of join optimization eligible
    // $lookup+$unwinds pairs and turning them into CanonicalQueries. At the end only ineligible for
    // join optimization stages are left in the suffix.
    while (!suffix->getSources().empty()) {
        // If we already reach the maximum number of nodes and/or edges we bail out from building
        // the graph and put the remaining stages into the suffix.
        if (graph.numNodes() >= buildParams.joinGraphBuildParams.maxNodesInJoin) {
            // Note that we add one node per loop.
            metrics.fallbackReason = JoinFallbackReason::kTooManyNodes;
            break;
        }
        if (graph.numEdges() >= buildParams.joinGraphBuildParams.maxEdgesInJoin) {
            metrics.fallbackReason = JoinFallbackReason::kTooManyEdgesOrPredicates;
            break;
        }

        auto* stage = suffix->getSources().front().get();
        if (auto* lookup = dynamic_cast<DocumentSourceLookUp*>(stage); lookup) {
            if (auto reason = isLookupEligible(*lookup)) {
                metrics.fallbackReason = *reason;
                break;
            }

            // "Reserve" a node id we can tentatively resolve paths against until we're actually
            // ready to modify the join graph.
            NodeId foreignNodeIdReserved = graph.numNodes();
            if (!pathResolver.trackEmbedPath(*lookup, foreignNodeIdReserved)) {
                metrics.fallbackReason = JoinFallbackReason::kInvalidEmbedPath;
                break;
            }

            // Attempt to extract join predicates and single table predicates from the $lookup
            // expressed as $expr in $match stage or as a local field/ foreign field join. If there
            // is no subpipeline, this returns no join predicates and a CanonicalQuery with empty
            // predicate. If this returns a bad status, then this extraction failed due to an
            // ineligible stage/expression.
            auto swPreds = extractPredicatesFromLookup(
                *lookup, pathResolver, foreignNodeIdReserved, metrics.fallbackReason);
            if (!swPreds.isOK()) {
                break;
            }
            auto preds = std::move(swPreds.getValue());

            // If we get here, it means we're ready to modify the join graph to include this
            // $lookup. Once the join graph has been modified, any failure case should cause us to
            // bail out of join optimization completely, rather than just ending the prefix here
            // (since we've already partially incorporated the current join).
            auto foreignNodeId = graph.addNode(
                lookup->getFromNs(), std::move(preds.canonicalQuery), lookup->getAsField());
            tassert(12835900,
                    "Expected reserved node id to match eventual id",
                    foreignNodeId && foreignNodeIdReserved == *foreignNodeId);

            // Add join predicates to join graph. Bail if we fail to add any of them.
            if (!addJoinPredicates(
                    preds.joinPredicates, pathResolver.resolvedPaths(), graph, metrics)) {
                // 'addEdge' applies both the edge and predicate limits, and reports a single
                // failure for either.
                return Status(ErrorCodes::BadValue,
                              "Graph is too big: too many edges or predicates");
            }

            auto next = suffix->popFront();
            if (prefix->getSources().empty()) {
                prefix->addInitialSource(std::move(next));
            } else {
                prefix->pushBack(std::move(next));
            }

            uniqueNamespaces.insert(lookup->getFromNs());

        } else if (auto* match = dynamic_cast<DocumentSourceMatch*>(stage); match) {
            if (graph.numNodes() < 2) {
                // Bail out if the leading $match can't be absorbed.
                break;
            }

            auto result = extractExprPredicates(pathResolver, match);
            bool canMatchBeEliminated = result.expressionIsFullyAbsorbed;
            if (!addJoinPredicates(
                    result.predicates, pathResolver.resolvedPaths(), graph, metrics)) {
                // End prefix here- we weren't able to add all edges, but that's ok.
                break;
            }

            if (!canMatchBeEliminated) {
                // End prefix here- this $match includes something we can't push into the join
                // model. 'extractExprPredicates()' reports which part.
                metrics.fallbackReason = result.reason;
                break;
            }

            // This $match encodes a valid join-predicate & can be fully absorbed into our
            // join-graph.
            prefix->pushBack(suffix->popFront());

        } else {
            // Unrecognized stage, give up on building a prefix.
            metrics.fallbackReason = JoinFallbackReason::kUnsupportedStage;
            break;
        }
    }

    // Find out how many $lookups remain in the suffix.
    for (const auto& stage : suffix->getSources()) {
        if (dynamic_cast<const DocumentSourceLookUp*>(stage.get())) {
            metrics.numLookupsInSuffix++;
        }
    }

    // Update remaining syntactic statistics.
    metrics.numJoinGraphNodes = graph.numNodes();
    metrics.numSyntacticEdges = graph.numEdges();

    auto resolvedPaths = pathResolver.releaseResolvedPaths(graph.numNodes());

    if (graph.numNodes() < 2) {
        // We need at least 1 eligible $lookup and a fully SBE-pushed-down prefix.
        if (!metrics.fallbackReason) {
            metrics.fallbackReason = JoinFallbackReason::kTooFewNodes;
        }
        return Status(ErrorCodes::QueryFeatureNotAllowed, "Join reordering not allowed");
    }

    auto swVec =
        addImplicitEdgesAndInferPredicates(graph,
                                           resolvedPaths,
                                           buildParams.maxNumberNodesConsideredForImplicitEdges,
                                           clonedExpCtx,
                                           metrics);
    tassert(13199202, "Failed to add implicit edges and infer predicates", swVec.isOK());

    // Update remaining inferred statistics.
    metrics.numInferredEdges = graph.numEdges() - metrics.numSyntacticEdges;

    JoinGraph result = JoinGraph(std::move(graph));
    const auto shape = result.getShape();
    if (!(shape & JoinGraph::GraphShapeFlags::Connected)) {
        metrics.fallbackReason = JoinFallbackReason::kGraphDisconnected;
        return Status(ErrorCodes::InternalErrorNotSupported,
                      "Join graph must be connected as cross-products are not yet supported");
    }

    metrics.isClique = shape & JoinGraph::GraphShapeFlags::Clique;
    metrics.isCycle = shape & JoinGraph::GraphShapeFlags::Cycle;
    metrics.isChain = shape & JoinGraph::GraphShapeFlags::Chain;
    metrics.isStar = shape & JoinGraph::GraphShapeFlags::Star;

    // Note: we may still bail out after the join model is constructed, but not for reasons related
    // to the query shape.
    metrics.joinOptimizable = true;
    return AggJoinModel(std::move(result),
                        std::move(resolvedPaths),
                        std::move(prefix),
                        std::move(suffix),
                        std::move(swVec.getValue()),
                        std::move(clonedExpCtx));
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
