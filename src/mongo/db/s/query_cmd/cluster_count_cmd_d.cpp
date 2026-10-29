// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/s/commands/query_cmd/cluster_count_cmd.h"
#include "mongo/util/assert_util.h"

#include <set>
#include <string>
#include <string_view>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

/**
 * Implements the cluster count command on mongod.
 */
struct ClusterCountCmdD {
    using Request = CountCommandRequest;
    using Reply = CountCommandRequest::Reply;
    static constexpr std::string_view kCommandName = "clusterCount"sv;

    static const std::set<std::string>& getApiVersions() {
        return kNoApiVersions;
    }

    static Status checkAuthForOperation(OperationContext* opCtx,
                                        const DatabaseName& dbName,
                                        const Request& req) {
        auto* as = AuthorizationSession::get(opCtx->getClient());
        if (!as->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(dbName.tenantId()), ActionType::internal)) {
            return {ErrorCodes::Unauthorized, "unauthorized"};
        }

        return Status::OK();
    }

    static void checkCanRunHere(OperationContext* opCtx) {
        Grid::get(opCtx)->assertShardingIsInitialized();

        // A cluster command on the config server may attempt to use a ShardLocal to target itself,
        // which triggers an invariant, so only shard servers can run this.
        ShardingState::get(opCtx)->assertCanAcceptShardedCommands();
    }

    static void checkCanExplainHere(OperationContext* opCtx) {
        uasserted(ErrorCodes::CommandNotSupported,
                  "Cannot explain a cluster count command on a mongod");
    }
};
MONGO_REGISTER_COMMAND(ClusterCountCmdBase<ClusterCountCmdD>).forShard();

}  // namespace
}  // namespace mongo
