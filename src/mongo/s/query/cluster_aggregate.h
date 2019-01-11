
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

#include <memory>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregation_request.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/commands/strategy.h"
#include "mongo/s/query/cluster_client_cursor_params.h"
#include "mongo/s/query/document_source_merge_cursors.h"

namespace mongo {

class LiteParsedPipeline;
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
     * On success, fills out 'result' with the command response.
     */
    static Status runAggregate(OperationContext* opCtx,
                               const Namespaces& namespaces,
                               const AggregationRequest& request,
                               const PrivilegeVector& privileges,
                               BSONObjBuilder* result);

    /**
     * Retries a command that was previously run on a view by resolving the view as an aggregation
     * against the underlying collection.
     *
     * 'privileges' contains the privileges that were required to run this aggregation, to be used
     * later for re-checking privileges for GetMore commands.
     *
     * On success, populates 'result' with the command response.
     */
    static Status retryOnViewError(OperationContext* opCtx,
                                   const AggregationRequest& request,
                                   const ResolvedView& resolvedView,
                                   const NamespaceString& requestedNss,
                                   const PrivilegeVector& privileges,
                                   BSONObjBuilder* result,
                                   unsigned numberRetries = 0);

private:
    static void uassertAllShardsSupportExplain(
        const std::vector<AsyncRequestsSender::Response>& shardResults);

    static Status aggPassthrough(OperationContext*,
                                 const Namespaces&,
                                 const ShardId&,
                                 const AggregationRequest&,
                                 const LiteParsedPipeline&,
                                 const PrivilegeVector& privileges,
                                 BSONObjBuilder* result);
};

}  // namespace mongo
