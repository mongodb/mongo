/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/service_entry_point_rs_endpoint.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/fsync_locked.h"
#include "mongo/db/curop.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/catalog_cache/shard_cannot_refresh_due_to_locks_held_exception.h"
#include "mongo/db/global_catalog/router_role_api/gossiped_routing_cache_gen.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/local_catalog/shard_role_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_concern.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/speculative_majority_read_info.h"
#include "mongo/db/replica_set_endpoint_util.h"
#include "mongo/db/s/resharding/resharding_metrics_helpers.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_entry_point_shard_role.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
#include "mongo/db/write_concern.h"
#include "mongo/idl/generic_argument_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/check_allowed_op_query_cmd.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/s/service_entry_point_router_role.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/namespace_string_util.h"

#include <memory>
#include <mutex>
#include <string>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {

Future<DbResponse> ServiceEntryPointRSEndpoint::_replicaSetEndpointHandleRequest(
    OperationContext* opCtx, const Message& m, Date_t started) try {
    // TODO (SERVER-81551): Move the OpMsgRequest parsing above ServiceEntryPoint::handleRequest().
    auto opMsgReq = rpc::opMsgRequestFromAnyProtocol(m, opCtx->getClient());
    if (m.operation() == dbQuery) {
        checkAllowedOpQueryCommand(*opCtx->getClient(), opMsgReq.getCommandName());
    }

    auto shouldRoute = replica_set_endpoint::shouldRouteRequest(opCtx, opMsgReq);
    LOGV2_DEBUG(8555601,
                3,
                "Using replica set endpoint",
                "opId"_attr = opCtx->getOpID(),
                "cmdName"_attr = opMsgReq.getCommandName(),
                "dbName"_attr = opMsgReq.readDatabaseForLogging(),
                "cmdObj"_attr = redact(opMsgReq.body.toString()),
                "shouldRoute"_attr = shouldRoute);
    if (shouldRoute) {
        replica_set_endpoint::ScopedSetRouterService service(opCtx);
        auto routerService = opCtx->getServiceContext()->getService(ClusterRole::RouterServer);
        return routerService->getServiceEntryPoint()->handleRequest(opCtx, m, started);
    }
    return _shardSep->handleRequest(opCtx, m, started);
} catch (const DBException& ex) {
    if (OpMsg::isFlagSet(m, OpMsg::kMoreToCome)) {
        return DbResponse{};  // Don't reply.
    }

    // Try to generate a response based on the status. If encounter another error (e.g.
    // UnsupportedFormat) while trying to generate the response, just return the status.
    try {
        auto replyBuilder = rpc::makeReplyBuilder(rpc::protocolForMessage(m));
        replyBuilder->setCommandReply(ex.toStatus(), {});
        DbResponse dbResponse;
        dbResponse.response = replyBuilder->done();
        return dbResponse;
    } catch (...) {
    }
    return ex.toStatus();
}
Future<DbResponse> ServiceEntryPointRSEndpoint::handleRequest(OperationContext* opCtx,
                                                              const Message& m,
                                                              Date_t started) {
    if (replica_set_endpoint::isReplicaSetEndpointClient(VersionContext::getDecoration(opCtx),
                                                         opCtx->getClient())) {
        return _replicaSetEndpointHandleRequest(opCtx, m, started);
    }
    return _shardSep->handleRequest(opCtx, m, started);
}

}  // namespace mongo
