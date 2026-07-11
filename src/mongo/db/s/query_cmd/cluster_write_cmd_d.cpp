// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/s/commands/query_cmd/cluster_write_cmd.h"
#include "mongo/util/assert_util.h"

#include <set>
#include <string>
#include <string_view>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

struct ClusterInsertCmdD {
    static constexpr std::string_view kName = "clusterInsert"sv;

    static const std::set<std::string>& getApiVersions() {
        return kNoApiVersions;
    }

    static void doCheckAuthorization(AuthorizationSession* authzSession,
                                     bool bypass,
                                     const write_ops::InsertCommandRequest& op) {
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
MONGO_REGISTER_COMMAND(ClusterInsertCmdBase<ClusterInsertCmdD>).forShard();

struct ClusterUpdateCmdD {
    static constexpr std::string_view kName = "clusterUpdate"sv;

    static const std::set<std::string>& getApiVersions() {
        return kNoApiVersions;
    }

    static void doCheckAuthorization(AuthorizationSession* authzSession,
                                     bool bypass,
                                     const write_ops::UpdateCommandRequest& op) {
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
        uasserted(ErrorCodes::CommandNotSupported, "Explain on a clusterDelete is not supported");
    }
};
MONGO_REGISTER_COMMAND(ClusterUpdateCmdBase<ClusterUpdateCmdD>).forShard();

struct ClusterDeleteCmdD {
    static constexpr std::string_view kName = "clusterDelete"sv;

    static const std::set<std::string>& getApiVersions() {
        return kNoApiVersions;
    }

    static void doCheckAuthorization(AuthorizationSession* authzSession,
                                     bool bypass,
                                     const write_ops::DeleteCommandRequest& op) {
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
                  "Cannot explain a cluster delete command on a mongod");
    }
};
MONGO_REGISTER_COMMAND(ClusterDeleteCmdBase<ClusterDeleteCmdD>).forShard();

}  // namespace
}  // namespace mongo
