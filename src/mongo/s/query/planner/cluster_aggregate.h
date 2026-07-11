// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/ifr_flag_retry_info.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/resolved_namespace.h"
#include "mongo/db/router_role/router_role.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/s/query/exec/cluster_client_cursor_params.h"
#include "mongo/s/query/exec/document_source_merge_cursors.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

class OperationContext;
class ShardId;

/**
 * Methods for running aggregation across a sharded cluster.
 */
class [[MONGO_MOD_PUBLIC]] ClusterAggregate {
public:
    /**
     * Max number of retries to resolve the underlying namespace of a view.
     */
    static constexpr unsigned kMaxViewRetries = 10;

    /**
     * 'requestedNss' is the namespace aggregation will register cursors under. This is the
     * namespace which we will return in responses to aggregate / getMore commands, and it is the
     * namespace we expect users to hand us inside any subsequent getMores. 'executionNss' is the
     * namespace we will run the mongod aggregate and subsequent getMore's against.
     */
    struct Namespaces {
        NamespaceString requestedNss;
        NamespaceString executionNss;
    };

    /**
     * Executes the aggregation 'request' using context 'opCtx'.
     *
     * The 'namespaces' struct should contain both the user-requested namespace and the namespace
     * over which the aggregation will actually execute. Typically these two namespaces are the
     * same, but they may differ in the case of a query on a view.
     *
     * 'privileges' contains the privileges that were required to run this aggregation, to be used
     * later for re-checking privileges for GetMore commands.
     *
     * 'liteParsedPipeline' is the lite-parsed form of the 'request'. When 'alreadyDesugared' is
     * true, the pipeline has already been desugared (e.g. during view resolution) and will not be
     * re-validated under the external client.
     *
     * On success, fills out 'result' with the command response.
     *
     * It manages the collection routing, meaning that the aggregation may be implicitly retried by
     * `runAggregate` if the placement of the collection has changed.
     */
    static Status runAggregate(
        OperationContext* opCtx,
        const Namespaces& namespaces,
        AggregateCommandRequest& request,
        const LiteParsedPipeline& liteParsedPipeline,
        const PrivilegeVector& privileges,
        boost::optional<ExplainOptions::Verbosity> verbosity,
        BSONObjBuilder* result,
        std::string_view comment = "ClusterAggregate::runAggregate"sv,
        std::shared_ptr<IncrementalFeatureRolloutContext> ifrContext = nullptr,
        bool alreadyDesugared = false);

    /**
     * Convenience version that internally constructs the LiteParsedPipeline.
     */
    static Status runAggregate(
        OperationContext* opCtx,
        const Namespaces& namespaces,
        AggregateCommandRequest& request,
        const PrivilegeVector& privileges,
        boost::optional<ExplainOptions::Verbosity> verbosity,
        BSONObjBuilder* result,
        std::string_view comment = "ClusterAggregate::runAggregate"sv,
        std::shared_ptr<IncrementalFeatureRolloutContext> ifrContext = nullptr);

    /**
     * Convenience version to inject the routingCtx by the caller. This function skips the
     * collection routing management, therefore it has to be managed by the caller. If the view is
     * resolved, 'request' will refer to the resolved request and 'originalRequest' will refer to
     * the unresolved request'. Avoid calling this function unless it's strictly necessary.
     */
    static Status runAggregateWithRoutingCtx(
        OperationContext* opCtx,
        RoutingContext& routingCtx,
        const Namespaces& namespaces,
        AggregateCommandRequest& request,
        const LiteParsedPipeline& liteParsedPipeline,
        const PrivilegeVector& privileges,
        boost::optional<ResolvedNamespace> resolvedView,
        boost::optional<AggregateCommandRequest> originalRequest,
        boost::optional<ExplainOptions::Verbosity> verbosity,
        BSONObjBuilder* result,
        std::shared_ptr<IncrementalFeatureRolloutContext> ifrContext = nullptr,
        bool alreadyDesugared = false);

    /**
     * Retries a command that either
     * 1) was previously run on a view, by resolving the view as an aggregation
     * against the underlying collection,
     * or 2) encountered an IFR retry kickback, by disabling the given IFR flag.
     *
     * 'privileges' contains the privileges that were required to run this aggregation, to be used
     * later for re-checking privileges for GetMore commands.
     *
     * On success, populates 'result' with the command response.
     *
     * This function doesn't throw, it return a Status object instead.
     *
     * TODO SERVER-118953 Remove this function when all callsites can use the generic path.
     */
    [[deprecated]] static Status retryOnViewOrIFRKickbackError(
        OperationContext* opCtx,
        const AggregateCommandRequest& request,
        const std::variant<ResolvedNamespace, IFRFlagRetryInfo>& errInfo,
        const NamespaceString& requestedNss,
        const PrivilegeVector& privileges,
        boost::optional<ExplainOptions::Verbosity> verbosity,
        BSONObjBuilder* result,
        std::shared_ptr<IncrementalFeatureRolloutContext> ifrContext = nullptr);
};

}  // namespace mongo
