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

#pragma once

#include "mongo/db/feature_flag.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/util/modules.h"

#include <memory>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

class MONGO_MOD_PUBLIC PipelineResolver {
public:
    /**
     * Constructs a new aggregation request which targets the base collection of 'resolvedView'
     * and applies the view pipeline to the original aggregation request pipeline.
     */
    static AggregateCommandRequest buildRequestWithResolvedPipeline(
        const ResolvedView& resolvedView, const AggregateCommandRequest& request);

    /**
     * Applies the view to a LiteParsedPipeline by constructing a ViewInfo from the resolved view,
     * desugaring the view pipeline, and calling handleView(). This is the common logic used by both
     * mongod and mongos view handling. Modifies 'userLPP' in place.
     */
    static void applyViewToLiteParsed(LiteParsedPipeline* userLPP,
                                      const ResolvedView& resolvedView,
                                      const NamespaceString& viewNss,
                                      const LiteParserOptions& options = LiteParserOptions{});

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
     * Builds a resolved aggregation request from a view for mongos, handling special cases for
     * mongot pipelines, timeseries views, and invoking ViewPolicy callbacks for extension stages.
     *
     * This is the single entrypoint for mongos view resolution. It returns a fully resolved
     * request with the pipeline set, and optionally a LiteParsedPipeline that was created during
     * resolution (for regular views) to avoid recreating it later.
     */
    static MongosViewRequestResult buildResolvedMongosViewRequest(
        OperationContext* opCtx,
        const AggregateCommandRequest& request,
        const ResolvedView& resolvedView,
        const NamespaceString& requestedNss,
        boost::optional<ExplainOptions::Verbosity> verbosity,
        std::shared_ptr<IncrementalFeatureRolloutContext> ifrContext,
        const MongosPipelineHelpers& helpers);
};

}  // namespace mongo
