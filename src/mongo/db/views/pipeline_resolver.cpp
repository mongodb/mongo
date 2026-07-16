// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/views/pipeline_resolver.h"

#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/lite_parsed_desugarer.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/resolved_namespace.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/pipeline/search/search_helper_bson_obj.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/stdx/unordered_set.h"

namespace mongo {
namespace {
/**
 * Builds the base resolved request by copying the original request, setting the namespace to the
 * resolved view's namespace (if any), and handling explain/cursor precedence. When 'resolvedView'
 * is boost::none the namespace is left unchanged (top-level is a base collection).
 */
AggregateCommandRequest buildBaseResolvedRequest(
    const boost::optional<ResolvedNamespace>& resolvedView,
    const AggregateCommandRequest& originalRequest) {
    // Start with a copy of the original request and modify fields as needed. We assume that most
    // fields should be unchanged from the original request; any fields that need to be changed will
    // be modified below.
    // TODO SERVER-110454: Avoid copying the original pipeline when possible.
    AggregateCommandRequest resolvedRequest = originalRequest;
    if (resolvedView) {
        resolvedRequest.setNamespace(resolvedView->getResolvedNamespace());
    }

    // If both 'explain' and 'cursor' are set, we give precedence to 'explain' and drop 'cursor'.
    if (originalRequest.getExplain()) {
        resolvedRequest.setCursor(SimpleCursorOptions());
    }

    return resolvedRequest;
}

/**
 * Builds a resolved pipeline by prepending the view pipeline to the
 * user pipeline. Handles special cases for mongot pipelines and timeseries views.
 * For mongot pipelines, returns just the user pipeline (view pipeline is applied by
 * $_internalSearchIdLookup). For timeseries views, prepends the view pipeline.
 */
std::vector<BSONObj> buildResolvedPipelineForSimpleCase(
    const std::shared_ptr<IncrementalFeatureRolloutContext>& ifrContext,
    const ResolvedNamespace& resolvedView,
    const std::vector<BSONObj>& userPipeline) {
    // Mongot user pipelines are a unique case: $_internalSearchIdLookup applies the view pipeline.
    // For this reason, we do not expand the aggregation request to include the view pipeline.
    // Caller is expected to use LiteParsedPipeline::handleView() for such cases.
    if (search_helper_bson_obj::isMongotPipeline(ifrContext, userPipeline) ||
        search_helper_bson_obj::isExtensionMongotPipeline(ifrContext, userPipeline)) {
        return userPipeline;
    }

    // The new pipeline consists of two parts: first, 'pipeline' in this ResolvedView; then, the
    // pipeline in 'request'.
    auto& viewPipeline = resolvedView.getBsonPipeline();
    std::vector<BSONObj> resolvedPipeline;
    resolvedPipeline.reserve(viewPipeline.size() + userPipeline.size());
    resolvedPipeline.insert(resolvedPipeline.end(), viewPipeline.begin(), viewPipeline.end());
    resolvedPipeline.insert(resolvedPipeline.end(), userPipeline.begin(), userPipeline.end());
    return resolvedPipeline;
}

/**
 * Builds the resolved pipeline for the regular (non-mongot, non-timeseries) path. Handles both:
 *   - 'resolvedView' set: the top-level namespace is a view; insert a top-level view entry
 *     so its pipeline is prepended.
 *   - 'resolvedView' boost::none: the top-level namespace is a base collection; the closure in
 *     'preResolvedNamespaces' is still threaded into the LiteParsedPipeline so subpipeline
 *     views (e.g. inside $unionWith / $lookup) get their stages stitched.
 *
 * In both cases this requires mongos-specific desugaring and serialization to ensure view info
 * is properly bound and included in the BSON sent from mongos to the shards.
 */
std::pair<std::vector<BSONObj>, boost::optional<LiteParsedPipeline>>
buildResolvedPipelineForRegularView(OperationContext* opCtx,
                                    const AggregateCommandRequest& request,
                                    const boost::optional<ResolvedNamespace>& resolvedView,
                                    const NamespaceString& requestedNss,
                                    boost::optional<ExplainOptions::Verbosity> verbosity,
                                    std::shared_ptr<IncrementalFeatureRolloutContext> ifrContext,
                                    const PipelineResolver::MongosPipelineHelpers& helpers,
                                    const ResolvedNamespaceMap& preResolvedNamespaces) {
    // For regular views, we need mongos-specific desugaring and serialization to ensure
    // view info is properly bound and included in the BSON sent from mongos to the shards.
    auto lpp = LiteParsedPipeline(request, true, LiteParserOptions{.ifrContext = ifrContext});
    lpp.makeOwned();

    // Desugar and call handleView() to invoke bindResolvedNamespace() for extension stages.
    LiteParsedDesugarer::desugar(&lpp, ifrContext);

    auto mergeResolved = [&](ResolvedNamespaceMap m) {
        for (const auto& [nss, rn] : preResolvedNamespaces) {
            m.insert_or_assign(nss, rn);
        }
        return m;
    };

    auto resolvedNamespaces =
        mergeResolved(helpers.resolveInvolvedNamespaces(lpp.getInvolvedNamespaces()));

    if (resolvedView) {
        PipelineResolver::insertTopLevelViewEntry(
            resolvedNamespaces, requestedNss, *resolvedView, ifrContext);
    }

    const bool userHadSearchStage = lpp.hasSearchStage();

    PipelineResolver::resolveInvolvedNamespacesOnLiteParsedPipeline(
        &lpp, requestedNss, resolvedNamespaces);
    // Involved namespaces may have been added to lpp, so get the most up to date set.
    auto updatedResolvedNamespaces =
        mergeResolved(helpers.resolveInvolvedNamespaces(lpp.getInvolvedNamespaces()));

    // Parse the modified LiteParsedPipeline to a full Pipeline and serialize the Pipeline back
    // to BSON. This ensures that any view info bound to LiteParsedDocumentSources is included
    // in the BSON sent from mongos to the shards. When the top-level is a base collection, fall
    // back to the request's own namespace and collation.
    const auto& executionNss =
        resolvedView ? resolvedView->getResolvedNamespace() : request.getNamespace();
    BSONObj collationObj = resolvedView ? resolvedView->getDefaultCollation()
                                        : request.getCollation().value_or(BSONObj{});
    auto expCtx = helpers.makeExpressionContext(opCtx,
                                                request,
                                                boost::none,  // cri
                                                executionNss,
                                                requestedNss,
                                                collationObj,
                                                boost::none,  // uuid
                                                updatedResolvedNamespaces,
                                                false,  // hasChangeStream
                                                verbosity,
                                                ExpressionContextCollationMatchesDefault::kYes,
                                                ifrContext);
    // For $rankFusion/$scoreFusion on views, set view on expCtx so $search/$vectorSearch inside
    // them embed the view pipeline during desugaring (their LiteParsed stage returns false from
    // shouldResolveSubpipelineViews, so expCtx->getView() is the fallback).
    // TODO SERVER-121094 Stopgap until the legacy $search path is removed.
    if (resolvedView && userHadSearchStage) {
        search_helpers::checkAndSetViewOnExpCtx(expCtx, lpp, *resolvedView, requestedNss);
    }
    auto pipeline =
        Pipeline::parseFromLiteParsed(lpp, expCtx, nullptr, false, true /* useStubInterface */);

    query_shape::SerializationOptions wireOpts{.isSerializingForRemoteDispatch = true};
    return {pipeline->serializeToBson(wireOpts), std::move(lpp)};
}

/**
 * Applies final transformations to a resolved request:
 * - Applies timeseries rewrites if needed (only when 'resolvedView' is set)
 * - Rewrites index hints for timeseries views (only when 'resolvedView' is set)
 * - Sets the view's default collation (only when 'resolvedView' is set)
 *
 * When 'resolvedView' is boost::none the top-level namespace is a base collection, so the
 * request's namespace, hint, and collation are left untouched.
 */
void applyFinalTransformations(AggregateCommandRequest& resolvedRequest,
                               std::vector<BSONObj>&& resolvedPipeline,
                               const boost::optional<ResolvedNamespace>& resolvedView,
                               const AggregateCommandRequest& originalRequest) {
    // Apply timeseries rewrites if the first stage is $_internalUnpackBucket.
    if (resolvedView && !resolvedPipeline.empty() &&
        resolvedPipeline[0][DocumentSourceInternalUnpackBucket::kStageNameInternal]) {
        resolvedView->applyTimeseriesRewrites(&resolvedPipeline);
    }

    // Set the pipeline once after all transformations are complete.
    resolvedRequest.setPipeline(std::move(resolvedPipeline));

    if (!resolvedView) {
        return;
    }

    // If we have an index hint on a time-series view, we may need to rewrite the index spec to
    // match the index on the underlying buckets collection.
    if (originalRequest.getHint() && resolvedView->isTimeseries()) {
        auto newHint = resolvedView->rewriteIndexHintForTimeseries(*originalRequest.getHint());
        if (newHint.has_value()) {
            resolvedRequest.setHint(*newHint);
        }
    }

    // Operations on a view must always use the default collation of the view. We must have already
    // checked that if the user's request specifies a collation, it matches the collation of the
    // view.
    resolvedRequest.setCollation(resolvedView->getDefaultCollation());
}
}  // namespace

AggregateCommandRequest PipelineResolver::buildRequestWithResolvedPipeline(
    const std::shared_ptr<IncrementalFeatureRolloutContext>& ifrContext,
    const ResolvedNamespace& resolvedView,
    const AggregateCommandRequest& originalRequest) {
    AggregateCommandRequest expandedRequest =
        buildBaseResolvedRequest(resolvedView, originalRequest);

    std::vector<BSONObj> resolvedPipeline =
        buildResolvedPipelineForSimpleCase(ifrContext, resolvedView, originalRequest.getPipeline());

    applyFinalTransformations(
        expandedRequest, std::move(resolvedPipeline), resolvedView, originalRequest);

    return expandedRequest;
}

void PipelineResolver::applyViewToLiteParsed(LiteParsedPipeline* userLPP,
                                             const ResolvedNamespace& resolvedView,
                                             const NamespaceString& viewNss,
                                             const ResolvedNamespaceMap& resolvedNamespaces,
                                             const LiteParserOptions& options) {
    // Desugar extension stages in the view pipeline before stitching. This uses a callback
    // registered by lite_parsed_desugarer.cpp to avoid a circular build dependency between
    // ResolvedNamespace and LiteParsedDesugarer.
    auto view = ResolvedNamespace::makeForView(
        viewNss, resolvedView.getResolvedNamespace(), resolvedView.getBsonPipeline(), options);
    view.desugarViewPipeline();
    userLPP->handleView(view, resolvedNamespaces);
}

void PipelineResolver::validateStagesOnView(LiteParsedPipeline* userLPP,
                                            const ResolvedNamespace& resolvedView,
                                            const NamespaceString& viewNss,
                                            const ResolvedNamespaceMap& resolvedNamespaces,
                                            const LiteParserOptions& options) {
    auto view = ResolvedNamespace::makeForView(
        viewNss, resolvedView.getResolvedNamespace(), resolvedView.getBsonPipeline(), options);
    view.desugarViewPipeline();
    userLPP->bindResolvedNamespaceToStages(view, resolvedNamespaces);
}

PipelineResolver::MongosViewRequestResult PipelineResolver::buildResolvedMongosViewRequest(
    OperationContext* opCtx,
    const AggregateCommandRequest& request,
    const boost::optional<ResolvedNamespace>& resolvedView,
    const NamespaceString& requestedNss,
    boost::optional<ExplainOptions::Verbosity> verbosity,
    std::shared_ptr<IncrementalFeatureRolloutContext> ifrContext,
    const MongosPipelineHelpers& helpers,
    const ResolvedNamespaceMap& preResolvedNamespaces) {
    // Build the base resolved request (copy, set namespace if there's a top-level view, handle
    // explain/cursor).
    AggregateCommandRequest resolvedAggRequest = buildBaseResolvedRequest(resolvedView, request);

    // Build the resolved pipeline. Mongot and timeseries views can use the simple pipeline
    // building logic. Regular views and the no-top-level-view case (where the closure in
    // 'preResolvedNamespaces' must still be threaded through subpipeline lite-parsing) go through
    // the regular path.
    std::vector<BSONObj> resolvedPipeline;
    boost::optional<LiteParsedPipeline> userLPP;
    if (resolvedView &&
        !search_helper_bson_obj::isHybridSearchBsonPipeline(request.getPipeline()) &&
        (search_helper_bson_obj::isMongotPipeline(ifrContext, request.getPipeline()) ||
         resolvedView->isTimeseries())) {
        resolvedPipeline =
            buildResolvedPipelineForSimpleCase(ifrContext, *resolvedView, request.getPipeline());

        // For mongot pipelines on views, validate that extension stages are allowed. The legacy
        // first stage handles view resolution itself, but subsequent extension stages still need
        // view validation. Timeseries views are skipped; their pipeline is fully resolved above via
        // buildResolvedPipelineForSimpleCase.
        // TODO SERVER-115069: this binds the view to top-level stages only. Not user-visible
        // today — mongot sub-pipelines on views resolve shard-side via the sharded view kickback,
        // where the recursive mongod path runs. Fold into the recursive resolver with the
        // bindResolvedNamespace() migration.
        if (!resolvedView->isTimeseries()) {
            LiteParserOptions options{.ifrContext = ifrContext};
            auto lpp = LiteParsedPipeline(request, true, options);
            lpp.makeOwned();
            LiteParsedDesugarer::desugar(&lpp, ifrContext);
            auto resolvedNamespaces =
                helpers.resolveInvolvedNamespaces(lpp.getInvolvedNamespaces());
            validateStagesOnView(&lpp, *resolvedView, requestedNss, resolvedNamespaces, options);
        }

        userLPP = boost::none;
    } else {
        std::tie(resolvedPipeline, userLPP) =
            buildResolvedPipelineForRegularView(opCtx,
                                                request,
                                                resolvedView,
                                                requestedNss,
                                                verbosity,
                                                ifrContext,
                                                helpers,
                                                preResolvedNamespaces);
    }

    // Apply final transformations (timeseries rewrites, index hint rewriting, collation setting).
    // No-ops when 'resolvedView' is null.
    applyFinalTransformations(
        resolvedAggRequest, std::move(resolvedPipeline), resolvedView, request);

    return MongosViewRequestResult{std::move(resolvedAggRequest), std::move(userLPP)};
}

namespace {
bool resolveInvolvedNamespacesImpl(LiteParsedPipeline* lpp,
                                   const NamespaceString& mainNss,
                                   const ResolvedNamespaceMap& resolvedNamespaces,
                                   stdx::unordered_set<NamespaceString>& inProgress) {
    auto isView = [&](const NamespaceString& nss) {
        auto it = resolvedNamespaces.find(nss);
        return it != resolvedNamespaces.end() && it->second.isInvolvedNamespaceAView();
    };
    // Build the view ResolvedNamespace to hand to handleView. If 'mainNss' is a view in the
    // map, copy it and desugar its pipeline, else pass an empty sentinel.
    bool anyViewBound = false;
    ResolvedNamespace view;
    if (auto it = resolvedNamespaces.find(mainNss);
        it != resolvedNamespaces.end() && it->second.isInvolvedNamespaceAView()) {
        view = it->second;
        view.liteParseViewPipeline();
        view.desugarViewPipeline();
        anyViewBound = true;
    }

    // Capture the original stage count BEFORE handleView/_stitchFront prepends the view's stages.
    // The delta (post - pre) is the number of newly prepended stages iterated in Pass A below.
    const size_t originalSize = lpp->getStages().size();

    if (search_helpers::isMongotLiteParsedPipeline(*lpp)) {
        lpp->bindResolvedNamespaceToStages(view, resolvedNamespaces);
    } else {
        lpp->handleView(view, resolvedNamespaces);
    }

    // NOTE: lpp->getStages() must be obtained AFTER handleView/_stitchFront, which replaces
    // the internal vector, invalidating any reference taken before the call.
    const auto& stages = lpp->getStages();
    const size_t viewStageCount = stages.size() - originalSize;

    // Recurse into the sub-pipeline views of every stage in the index range [begin, end).
    // Returns true if any nested view was bound.
    auto resolveSubpipelinesInRange = [&](size_t begin, size_t end) {
        bool bound = false;
        for (size_t i = begin; i < end; ++i) {
            if (!stages[i]->shouldResolveSubpipelineViews()) {
                continue;
            }
            auto* subs = stages[i]->getMutableSubPipelines();
            if (!subs) {
                continue;
            }
            for (auto& sub : *subs) {
                auto subNss = sub->getOriginalParseNss();
                // For materialized view sub-pipelines (e.g. those created by
                // materializeViewSubpipeline for $graphLookup {from: someView}), the
                // _originalParseNss is the backing collection, not the view — and isView() on the
                // backing collection returns false, bypassing cycle detection.  Use the tagged view
                // NSS instead so mutual-view cycles (viewA.$graphLookup{from:viewB},
                // viewB.$graphLookup{from:viewA}) are correctly detected.
                const auto& taggedViewNss = sub.getViewNss();
                const auto& cycleNss = taggedViewNss ? *taggedViewNss : subNss;
                const bool isRunningAgainstAView = isView(cycleNss);
                if (isRunningAgainstAView && !inProgress.insert(cycleNss).second) {
                    continue;
                }
                bound |= resolveInvolvedNamespacesImpl(
                    sub.operator->(), subNss, resolvedNamespaces, inProgress);
                if (isRunningAgainstAView) {
                    inProgress.erase(cycleNss);
                }
            }
        }
        return bound;
    };

    // Pass A: walk stages handleView/_stitchFront just prepended (indices [0, viewStageCount)).
    // These came from the view's own definition, so a subpipeline back to mainNss is a true
    // cycle.
    anyViewBound |= resolveSubpipelinesInRange(0, viewStageCount);

    // Pass B: walk stages that were already in the LPP before handleView prepended the view
    // definition (indices [viewStageCount, N)). Whether a subpipeline reference back to mainNss
    // is a legitimate user query or an infinite loop depends on whether mainNss is a view.
    //
    // Case A — mainNss is a view (anyViewBound=true): the stages here are user-written. A user
    // query may legitimately reference the same view again (e.g. nested $lookup depth checks).
    // See PipelineResolverTest::AllowsRepeatedViewReferenceInUserStages for a unit test.
    //
    // Case B — mainNss is a base collection (anyViewBound=false): the stages here were planted
    // by view resolution. Any back-reference indicates a cycle.
    // See jstests/concurrency/fsm_workloads/view_catalog/view_catalog_cycle_lookup.js for an
    // end-to-end test of the $graphLookup mutual-view-cycle this guards against.
    const bool wasInProgress = anyViewBound && (inProgress.erase(mainNss) > 0);
    anyViewBound |= resolveSubpipelinesInRange(viewStageCount, stages.size());
    if (wasInProgress) {
        inProgress.insert(mainNss);
    }

    return anyViewBound;
}
}  // namespace

bool PipelineResolver::resolveInvolvedNamespacesOnLiteParsedPipeline(
    LiteParsedPipeline* lpp,
    const NamespaceString& mainNss,
    const ResolvedNamespaceMap& resolvedNamespaces) {
    stdx::unordered_set<NamespaceString> inProgress;
    return resolveInvolvedNamespacesImpl(lpp, mainNss, resolvedNamespaces, inProgress);
}

void PipelineResolver::insertTopLevelViewEntry(
    ResolvedNamespaceMap& resolvedNamespaces,
    const NamespaceString& requestedNss,
    ResolvedNamespace resolvedView,
    std::shared_ptr<IncrementalFeatureRolloutContext> ifrContext) {
    ResolvedNamespaceViewOptions viewOptions;
    viewOptions.involvedNamespaceIsAView = true;
    viewOptions.shouldParseLpp = true;
    // Thread the IFR context through so that extension stages within the view pipeline have the
    // same feature-flag view as the top-level request. Without this, extension stages in view
    // definitions lite-parse with a null _ifrContext and LiteParsedExpanded::bindResolvedNamespace
    // trips its hybridSearchFlagEnabled check at view-binding time.
    viewOptions.options =
        std::make_shared<LiteParserOptions>(LiteParserOptions{.ifrContext = std::move(ifrContext)});

    // Preserve the UUID of the underlying collection if the namespace was already in the map
    // from a prior catalog lookup. This could occur when the requested Nss is both the top-level
    // view and targeted by a subpipeline stage.
    if (auto it = resolvedNamespaces.find(requestedNss); it != resolvedNamespaces.end()) {
        viewOptions.collUUID = it->second.getCollUUID();
    }
    // The resolution loop does not add the top-level view's namespace, so fall back to the
    // caller-supplied backing-collection UUID; without it, $search inside the desugared $unionWith
    // on this view fails with "a uuid is required for a search query".
    if (!viewOptions.collUUID) {
        viewOptions.collUUID = resolvedView.getCollUUID();
    } else if (resolvedView.getCollUUID()) {
        tassert(12828500,
                "Conflicting backing-collection UUIDs for the top-level view entry",
                *viewOptions.collUUID == *resolvedView.getCollUUID());
    }
    resolvedNamespaces.insert_or_assign(requestedNss,
                                        ResolvedNamespace(requestedNss,
                                                          resolvedView.getResolvedNamespace(),
                                                          resolvedView.getBsonPipeline(),
                                                          resolvedView.getDefaultCollation(),
                                                          viewOptions));
}
}  // namespace mongo
