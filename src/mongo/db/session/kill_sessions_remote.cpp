// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/session/kill_sessions_remote.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/session/kill_sessions.h"
#include "mongo/db/session/kill_sessions_common.h"
#include "mongo/db/session/kill_sessions_gen.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/executor/async_multicaster.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/s/query/exec/cluster_cursor_manager.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"

#include <algorithm>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include <absl/container/node_hash_set.h>
#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

/**
 * Get all hosts in the cluster.
 */
std::vector<HostAndPort> getAllClusterHosts(OperationContext* opCtx) {
    auto registry = Grid::get(opCtx)->shardRegistry();

    const auto shardIds = registry->getAllShardIds(opCtx);

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
    auto results = executor::AsyncMulticaster(executor, options)
                       .multicast(servers,
                                  DatabaseName::kAdmin,
                                  cmd,
                                  opCtx,
                                  Milliseconds(gKillSessionsPerHostTimeoutMS));

    for (const auto& result : results) {
        if (!std::get<1>(result).isOK() ||
            !getStatusFromCommandResult(std::get<1>(result).data).isOK()) {
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

SessionKiller::Result killSessionsRemote(OperationContext* opCtx,
                                         const SessionKiller::Matcher& matcher,
                                         SessionKiller::UniformRandomBitGenerator* urbg) {
    // First kill local sessions.
    uassertStatusOK(killSessionsRemoteKillCursor(opCtx, matcher));
    uassertStatusOK(killSessionsLocalKillOps(opCtx, matcher));

    // Generate the kill command.
    KillAllSessionsByPatternCmd cmd;
    std::vector<KillAllSessionsByPattern> patterns;
    for (auto& item : matcher.getPatterns()) {
        patterns.push_back(std::move(item.pattern));
    }
    cmd.setKillAllSessionsByPattern(std::move(patterns));
    return parallelExec(opCtx, cmd.toBSON(), urbg);
}

}  // namespace mongo
