// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/feature_flag.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/resolved_namespace.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/util/modules.h"

#include <memory>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

class [[MONGO_MOD_PUBLIC]] PipelineResolver {
public:
    /**
     * Constructs a new aggregation request which targets the base collection of 'resolvedView'
     * and applies the view pipeline to the original aggregation request pipeline.
     */
    static AggregateCommandRequest buildRequestWithResolvedPipeline(
        const std::shared_ptr<IncrementalFeatureRolloutContext>& ifrContext,
        const ResolvedNamespace& resolvedView,
        const AggregateCommandRequest& request);

    /**
     * Applies the view to a LiteParsedPipeline by constructing a view ResolvedNamespace from the
     * resolved view, desugaring the view pipeline, and calling handleView(). This is the legacy
     * view-application helper used by FF-off paths. Modifies 'userLPP' in place.
     * TODO SERVER-121094 Remove together with the FF-off branches in $unionWith / $lookup once
     * featureFlagExtensionsInsideHybridSearch is fully rolled out.
     */
    static void applyViewToLiteParsed(LiteParsedPipeline* userLPP,
                                      const ResolvedNamespace& resolvedView,
                                      const NamespaceString& viewNss,
                                      const ResolvedNamespaceMap& resolvedNamespaces,
                                      const LiteParserOptions& options = LiteParserOptions{});

    /**
     * Calls bindResolvedNamespace() on each stage in 'userLPP' without prepending the view
     * pipeline. Used for mongot pipelines on views where the legacy first stage handles view
     * resolution itself, but subsequent extension stages still need view validation.
     */
    static void validateStagesOnView(LiteParsedPipeline* userLPP,
                                     const ResolvedNamespace& resolvedView,
                                     const NamespaceString& viewNss,
                                     const ResolvedNamespaceMap& resolvedNamespaces,
                                     const LiteParserOptions& options = LiteParserOptions{});

    /**
     * Walks 'lpp' and binds any view found at 'mainNss' in 'resolvedNamespaces', then recurses
     * into each stage's subpipelines using the subpipeline's original parse NSS as the recursive
     * 'mainNss'. Returns whether any view was bound.
     */
    static bool resolveInvolvedNamespacesOnLiteParsedPipeline(
        LiteParsedPipeline* lpp,
        const NamespaceString& mainNss,
        const ResolvedNamespaceMap& resolvedNamespaces);

    /**
     * Inserts a ResolvedView into a ResolvedNamespaceMap.
     * Creates a ResolvedNamespace from the view with view-specific options set.
     *
     * Threads the caller-supplied 'ifrContext' into the resulting ResolvedNamespace's
     * LiteParserOptions so that extension stages inside the view pipeline lite-parse with the
     * same feature-flag view as the top-level request. Pass nullptr if there is no relevant IFR
     * context (e.g. internal test fixtures).
     *
     * 'underlyingCollUUID' is the UUID of the view's underlying collection (i.e. the collection the
     * aggregation actually executes against). It is recorded as the entry's collUUID when the map
     * does not already carry one. This matters when the top-level view is also targeted by a
     * sub-pipeline stage (e.g. a $rankFusion/$scoreFusion input desugared into a $unionWith on the
     * same view): the sub-pipeline's $search/$vectorSearch reads pExpCtx->getUUID(), so without the
     * UUID those stages would error ("a uuid is required for a search query") or return EOF.
     */
    static void insertTopLevelViewEntry(
        ResolvedNamespaceMap& resolvedNamespaces,
        const NamespaceString& requestedNss,
        ResolvedNamespace resolvedView,
        std::shared_ptr<IncrementalFeatureRolloutContext> ifrContext = nullptr);

    using MakeExpressionContextFn = std::function<boost::intrusive_ptr<ExpressionContext>(
        OperationContext*,
        const AggregateCommandRequest&,
        const boost::optional<CollectionRoutingInfo>&,
        const NamespaceString&,
        const NamespaceString&,
        BSONObj,
        boost::optional<UUID>,
        ResolvedNamespaceMap,
        bool,
        boost::optional<ExplainOptions::Verbosity>,
        ExpressionContextCollationMatchesDefault,
        std::shared_ptr<IncrementalFeatureRolloutContext>)>;
    using ResolveNamespacesFn =
        std::function<ResolvedNamespaceMap(const stdx::unordered_set<NamespaceString>&)>;

    /**
     * Helper struct for mongos-specific functions needed to parse/serialize pipelines.
     */
    struct MongosPipelineHelpers {
        MakeExpressionContextFn makeExpressionContext;
        ResolveNamespacesFn resolveInvolvedNamespaces;
    };

    /**
     * Result of building a resolved view request for mongos. Contains the resolved request and
     * optionally a LiteParsedPipeline that was created during resolution (to avoid recreating it).
     */
    struct MongosViewRequestResult {
        AggregateCommandRequest resolvedRequest;
        boost::optional<LiteParsedPipeline> liteParsedPipeline;
    };

    /**
     * Builds a resolved aggregation request for mongos, handling special cases for mongot
     * pipelines, timeseries views, and invoking bindResolvedNamespace() for extension stages.
     *
     * If 'resolvedView' is set, the top-level namespace is a view and its pipeline is
     * prepended to the user pipeline (modulo mongot/timeseries handling). If 'resolvedView' is
     * boost::none, the top-level namespace is a base collection: the request namespace and
     * pipeline shape are preserved, but 'preResolvedNamespaces' (a transitive closure of
     * sub-pipeline view resolutions shipped back from a sentinel-primary kickback) is still
     * threaded through the LiteParsedPipeline so $unionWith / $lookup subpipelines see the
     * resolved views.
     *
     * Returns a fully resolved request with the pipeline set, and optionally a LiteParsedPipeline
     * that was created during resolution (for regular views) to avoid recreating it later.
     */
    static MongosViewRequestResult buildResolvedMongosViewRequest(
        OperationContext* opCtx,
        const AggregateCommandRequest& request,
        const boost::optional<ResolvedNamespace>& resolvedView,
        const NamespaceString& requestedNss,
        boost::optional<ExplainOptions::Verbosity> verbosity,
        std::shared_ptr<IncrementalFeatureRolloutContext> ifrContext,
        const MongosPipelineHelpers& helpers,
        const ResolvedNamespaceMap& preResolvedNamespaces);
};

}  // namespace mongo
