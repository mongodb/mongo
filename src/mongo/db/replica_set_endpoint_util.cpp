/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/replica_set_endpoint_util.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/replica_set_endpoint_sharding_state.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/util/namespace_string_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace replica_set_endpoint {

namespace {

/**
 * Returns true if this is an operation from an internal client.
 */
bool isInternalClient(OperationContext* opCtx) {
    return !opCtx->getClient()->session() || opCtx->getClient()->isInternalClient() ||
        opCtx->getClient()->isInDirectClient();
}

/**
 * Returns true if this is a request for the local database.
 */
bool isLocalDatabaseCommandRequest(const OpMsgRequest& opMsgReq) {
    return opMsgReq.parseDbName().isLocalDB();
}

/**
 * Returns true if this is a request for a command that needs to run directly on the mongod it
 * arrives on.
 */
bool isTargetedCommandRequest(OperationContext* opCtx, const OpMsgRequest& opMsgReq) {
    return kTargetedCmdNames.contains(opMsgReq.getCommandName());
}

/**
 * Returns the service for the specified role.
 */
Service* getService(OperationContext* opCtx, ClusterRole role) {
    auto service = opCtx->getServiceContext()->getService(role);
    invariant(service);
    return service;
}

/**
 * Returns the router service.
 */
Service* getRouterService(OperationContext* opCtx) {
    return getService(opCtx, ClusterRole::RouterServer);
}

/**
 * Returns the shard service.
 */
Service* getShardService(OperationContext* opCtx) {
    return getService(opCtx, ClusterRole::ShardServer);
}

/**
 * Returns true if this is a request for a command that does not exist on a router.
 */
bool isRoutableCommandRequest(OperationContext* opCtx, const OpMsgRequest& opMsgReq) {
    return CommandHelpers::findCommand(getRouterService(opCtx), opMsgReq.getCommandName());
}

}  // namespace

ScopedSetRouterService::ScopedSetRouterService(OperationContext* opCtx)
    : _opCtx(opCtx), _originalService(opCtx->getService()) {
    // Verify that the opCtx is not using the router service already.
    invariant(!_originalService->role().has(ClusterRole::RouterServer));
    ClientLock lk(_opCtx->getClient());
    _opCtx->getClient()->setService(getRouterService(opCtx));
    _opCtx->setRoutedByReplicaSetEndpoint(true);
}

ScopedSetRouterService::~ScopedSetRouterService() {
    // Verify that the opCtx is still using the router service.
    ClientLock lk(_opCtx->getClient());
    invariant(_opCtx->getService()->role().has(ClusterRole::RouterServer));
    _opCtx->getClient()->setService(_originalService);
    _opCtx->setRoutedByReplicaSetEndpoint(false);
}

bool isReplicaSetEndpointClient(const VersionContext& vCtx, Client* client) {
    return false;
}

bool shouldRouteRequest(OperationContext* opCtx, const OpMsgRequest& opMsgReq) {
    return false;
}

void checkIfCanRunCommand(OperationContext* opCtx, const OpMsgRequest& opMsgReq) {
    auto shardCommand = CommandHelpers::findCommand(replica_set_endpoint::getShardService(opCtx),
                                                    opMsgReq.getCommandName());
    if (shardCommand &&
        shardCommand->secondaryAllowed(opCtx->getServiceContext()) ==
            BasicCommand::AllowedOnSecondary::kNever) {
        // On the shard service, this is a primary-only command. Make it fail with a
        // NotWritablePrimary error if this mongod is not the primary. This check is necessary
        // for providing replica set user experience (i.e. writes should fail on secondaries)
        // since by going through the router code paths the command would get routed to the
        // primary and succeed whether or not this mongod is the primary. This check only needs
        // to be best-effort since if this mongod steps down after the check, the write would be
        // routed to the new primary. For this reason, just use
        // canAcceptWritesForDatabase_UNSAFE to avoid taking the RSTL lock or the
        // ReplicationCoordinator's mutex.
        uassert(ErrorCodes::NotWritablePrimary,
                "This command is only allowed on a primary",
                repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesForDatabase_UNSAFE(
                    opCtx, opMsgReq.parseDbName()));
    }
}

}  // namespace replica_set_endpoint
}  // namespace mongo
