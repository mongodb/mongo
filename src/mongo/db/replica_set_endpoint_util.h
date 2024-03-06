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

#pragma once

#include "mongo/db/operation_context.h"

namespace mongo {
namespace replica_set_endpoint {

// The set of commands that need to run on the mongod it arrives on (i.e. must not go through the
// router code paths).
const StringDataSet kTargetedCmdNames = {
    "buildinfo",
    "clearLog",
    "compact",  // The command also exists on a router but all it does is throwing
                // CommandNotSupported.
    "configureFailPoint",
    "connectionStatus",
    "fsync",
    "fsyncUnlock",
    "getDiagnosticData",  // TODO (SERVER-79353): Support role-aware serverStatus on mongod with
                          // router role. Evaluate this command should go through the router code
                          // paths.
    "getLog",
    "getParameter",
    "getShardVersion",
    "hello",
    "isMaster",
    "ismaster",
    "logout",
    "ping",
    "profile",
    "reapLogicalSessionCacheNow",
    "refreshLogicalSessionCacheNow",
    "replSetGetStatus",
    "saslStart",
    "saslContinue",
    "setParameter",
    "serverStatus",  // TODO (SERVER-79353): Support role-aware serverStatus on mongod with
                     // router role. Evaluate this command should go through the router code
                     // paths.
    "splitVector",   // TODO (SERVER-84090): Investigate whether to change or deprecate router's
                     // splitVector command.
    "validate",
    "waitForFailPoint",
    "_flushRoutingTableCacheUpdates"};

/**
 * RAII type for making the OperationContext it is instantiated with use the router service util it
 * goes out of scope. Throws an invariant error if the OperationContext is already using the router
 * service.
 */
class ScopedSetRouterService {
public:
    ScopedSetRouterService(OperationContext* opCtx);
    ~ScopedSetRouterService();

private:
    OperationContext* const _opCtx;
    Service* const _originalService;
};

/**
 * Returns true if this is a client on the shard port of a shardsvr mongod that supports
 * replica set endpoint.
 */
bool isReplicaSetEndpointClient(Client* client);

/**
 * Returns true if a request on the shard port of a shardsvr mongod should go through the router
 * code paths instead of the shard code paths.
 */
bool shouldRouteRequest(OperationContext* opCtx, const OpMsgRequest& opMsgReq);

}  // namespace replica_set_endpoint
}  // namespace mongo
