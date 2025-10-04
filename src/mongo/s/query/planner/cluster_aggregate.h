/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/router_role_api/router_role.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/s/query/exec/cluster_client_cursor_params.h"
#include "mongo/s/query/exec/document_source_merge_cursors.h"

#include <boost/optional/optional.hpp>

namespace mongo {

class OperationContext;
class ShardId;

/**
 * Methods for running aggregation across a sharded cluster.
 */
class ClusterAggregate {
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
     * 'cri' is the routing table used by the higher level code that the aggregation
     * should use during its execution. If it's empty, the routing table will be requested
     * internally.
     *
     * On success, fills out 'result' with the command response.
     *
     * It manages the collection routing, meaning that the aggregation may be implicitly retried by
     * `runAggregate` if the placement of the collection has changed.
     */
    static Status runAggregate(OperationContext* opCtx,
                               const Namespaces& namespaces,
                               AggregateCommandRequest& request,
                               const LiteParsedPipeline& liteParsedPipeline,
                               const PrivilegeVector& privileges,
                               boost::optional<ExplainOptions::Verbosity> verbosity,
                               BSONObjBuilder* result,
                               StringData comment = "ClusterAggregate::runAggregate"_sd);


    /**
     * Convenience version that internally constructs the LiteParsedPipeline.
     */
    static Status runAggregate(OperationContext* opCtx,
                               const Namespaces& namespaces,
                               AggregateCommandRequest& request,
                               const PrivilegeVector& privileges,
                               boost::optional<ExplainOptions::Verbosity> verbosity,
                               BSONObjBuilder* result,
                               StringData comment = "ClusterAggregate::runAggregate"_sd);

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
        boost::optional<ResolvedView> resolvedView,
        boost::optional<AggregateCommandRequest> originalRequest,
        boost::optional<ExplainOptions::Verbosity> verbosity,
        BSONObjBuilder* result);

    /**
     * Retries a command that was previously run on a view by resolving the view as an aggregation
     * against the underlying collection.
     *
     * 'privileges' contains the privileges that were required to run this aggregation, to be used
     * later for re-checking privileges for GetMore commands.
     *
     * On success, populates 'result' with the command response.
     *
     * This function doesn't throw, it return a Status object instead.
     */
    static Status retryOnViewError(OperationContext* opCtx,
                                   const AggregateCommandRequest& request,
                                   const ResolvedView& resolvedView,
                                   const NamespaceString& requestedNss,
                                   const PrivilegeVector& privileges,
                                   boost::optional<ExplainOptions::Verbosity> verbosity,
                                   BSONObjBuilder* result,
                                   unsigned numberRetries = 0);
};

}  // namespace mongo
