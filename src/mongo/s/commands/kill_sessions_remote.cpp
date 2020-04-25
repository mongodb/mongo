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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/s/commands/kill_sessions_remote.h"
#include "mongo/s/commands/kill_sessions_remote_gen.h"

#include "mongo/db/client.h"
#include "mongo/db/kill_sessions_common.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/async_multicaster.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_cursor_manager.h"

namespace mongo {

namespace {

/**
 * Get all hosts in the cluster.
 */
std::vector<HostAndPort> getAllClusterHosts(OperationContext* opCtx) {
    auto registry = Grid::get(opCtx)->shardRegistry();

    std::vector<ShardId> shardIds;
    registry->getAllShardIds(opCtx, &shardIds);

    std::vector<HostAndPort> servers;
    for (const auto& shardId : shardIds) {
        auto shard = uassertStatusOK(registry->getShard(opCtx, shardId));

        auto cs = shard->getConnString();
        for (auto&& host : cs.getServers()) {
            servers.emplace_back(host);
        }
    }

    return servers;
}

/**
 * A function for running an arbitrary command on all shards.  Only returns which hosts failed.
 */
SessionKiller::Result parallelExec(OperationContext* opCtx,
                                   const BSONObj& cmd,
                                   SessionKiller::UniformRandomBitGenerator* urbg) {
    // Grab an arbitrary executor.
    auto executor = Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor();

    // Grab all hosts in the cluster.
    auto servers = getAllClusterHosts(opCtx);
    std::shuffle(servers.begin(), servers.end(), *urbg);

    // To indicate which hosts fail.
    std::vector<HostAndPort> failed;

    executor::AsyncMulticaster::Options options;
    options.maxConcurrency = gKillSessionsMaxConcurrency;
    auto results =
        executor::AsyncMulticaster(executor, options)
            .multicast(servers, "admin", cmd, opCtx, Milliseconds(gKillSessionsPerHostTimeoutMS));

    for (const auto& result : results) {
        if (!std::get<1>(result).isOK()) {
            failed.push_back(std::get<0>(result));
        }
    }

    return failed;
}

Status killSessionsRemoteKillCursor(OperationContext* opCtx,
                                    const SessionKiller::Matcher& matcher) {
    return Grid::get(opCtx)
        ->getCursorManager()
        ->killCursorsWithMatchingSessions(opCtx, matcher)
        .first;
}

}  // namespace

/**
 * This kill function (meant for mongos), kills matching local ops first, then fans out to all other
 * nodes in the cluster to kill them as well.
 */
SessionKiller::Result killSessionsRemote(OperationContext* opCtx,
                                         const SessionKiller::Matcher& matcher,
                                         SessionKiller::UniformRandomBitGenerator* urbg) {
    // First kill local sessions.
    uassertStatusOK(killSessionsRemoteKillCursor(opCtx, matcher));
    uassertStatusOK(killSessionsLocalKillOps(opCtx, matcher));

    // Generate the kill command.
    KillAllSessionsByPatternCmd cmd;
    cmd.setKillAllSessionsByPattern(std::vector<KillAllSessionsByPattern>{
        matcher.getPatterns().begin(), matcher.getPatterns().end()});

    return parallelExec(opCtx, cmd.toBSON(), urbg);
}

}  // namespace mongo
