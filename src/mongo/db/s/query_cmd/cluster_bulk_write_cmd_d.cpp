// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/s/commands/query_cmd/cluster_bulk_write_cmd.h"

#include <string_view>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

struct ClusterBulkWriteCmdD {
    static constexpr std::string_view kName = "clusterBulkWrite"sv;

    static const std::set<std::string>& getApiVersions() {
        return kNoApiVersions;
    }

    static void doCheckAuthorization(AuthorizationSession* authzSession,
                                     bool bypass,
                                     const BulkWriteCommandRequest& op) {
        uassert(ErrorCodes::Unauthorized,
                "Unauthorized",
                authzSession->isAuthorizedForActionsOnResource(
                    ResourcePattern::forClusterResource(op.getDbName().tenantId()),
                    ActionType::internal));
    }

    static void checkCanRunHere(OperationContext* opCtx) {
        Grid::get(opCtx)->assertShardingIsInitialized();

        // A cluster command on the config server may attempt to use a ShardLocal to target itself,
        // which triggers an invariant, so only shard servers can run this.
        ShardingState::get(opCtx)->assertCanAcceptShardedCommands();
    }

    static void checkCanExplainHere(OperationContext* opCtx) {
        uasserted(ErrorCodes::CommandNotSupported,
                  "Cannot explain a cluster insert command on a mongod");
    }
};
MONGO_REGISTER_COMMAND(ClusterBulkWriteCmd<ClusterBulkWriteCmdD>).forShard();

}  // namespace
}  // namespace mongo
