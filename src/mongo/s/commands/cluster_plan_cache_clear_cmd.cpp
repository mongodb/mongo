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

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/stale_exception.h"

namespace mongo {
namespace {

/**
 * Mongos implementation of the 'planCacheClear' command. Forwards the command to one node in each
 * targeted shard. For example, with the default read preference ("primary"), clears plan cache
 * entries on the primary node of each shard.
 */
class ClusterPlanCacheClearCmd final : public BasicCommand {
    ClusterPlanCacheClearCmd(const ClusterPlanCacheClearCmd&) = delete;
    ClusterPlanCacheClearCmd& operator=(const ClusterPlanCacheClearCmd&) = delete;

public:
    ClusterPlanCacheClearCmd() : BasicCommand("planCacheClear") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kOptIn;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    std::string help() const override {
        return "Drops one or all plan cache entries for a collection.";
    }

    NamespaceString parseNs(const DatabaseName& dbName, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsCollectionRequired(dbName, cmdObj);
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);
        ResourcePattern pattern = parseResourcePattern(dbname, cmdObj);

        if (authzSession->isAuthorizedForActionsOnResource(pattern, ActionType::planCacheWrite)) {
            return Status::OK();
        }

        return Status(ErrorCodes::Unauthorized, "unauthorized");
    }

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result);
} clusterPlanCacheClearCmd;

bool ClusterPlanCacheClearCmd::run(OperationContext* opCtx,
                                   const DatabaseName& dbName,
                                   const BSONObj& cmdObj,
                                   BSONObjBuilder& result) {
    const NamespaceString nss(CommandHelpers::parseNsCollectionRequired(dbName, cmdObj));
    const BSONObj query;
    const auto routingInfo =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
    auto shardResponses = scatterGatherVersionedTargetByRoutingTable(
        opCtx,
        nss.db(),
        nss,
        routingInfo,
        applyReadWriteConcern(
            opCtx, this, CommandHelpers::filterCommandRequestForPassthrough(cmdObj)),
        ReadPreferenceSetting::get(opCtx),
        Shard::RetryPolicy::kIdempotent,
        query,
        CollationSpec::kSimpleSpec);

    // Sort shard responses by shard id.
    std::sort(shardResponses.begin(),
              shardResponses.end(),
              [](const AsyncRequestsSender::Response& response1,
                 const AsyncRequestsSender::Response& response2) {
                  return response1.shardId < response2.shardId;
              });

    // Set value of first shard result's "ok" field.
    bool clusterCmdResult = true;

    for (auto i = shardResponses.begin(); i != shardResponses.end(); ++i) {
        const auto& response = *i;
        auto status = response.swResponse.getStatus();
        uassertStatusOK(status.withContext(str::stream() << "failed on: " << response.shardId));
        const auto& cmdResult = response.swResponse.getValue().data;

        // In absence of sensible aggregation strategy, promote first shard's result to top level.
        if (i == shardResponses.begin()) {
            CommandHelpers::filterCommandReplyForPassthrough(cmdResult, &result);
            status = getStatusFromCommandResult(cmdResult);
            clusterCmdResult = status.isOK();
        }

        // Append shard result as a sub object. Name the field after the shard.
        result.append(response.shardId, cmdResult);
    }

    return clusterCmdResult;
}

}  // namespace
}  // namespace mongo
