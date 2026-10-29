// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/commands/query_cmd/cluster_aggregate_cmd.h"
#include "mongo/util/assert_util.h"

#include <set>
#include <string>
#include <string_view>

#include <absl/container/node_hash_map.h>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

/**
 * Implements the cluster aggregate command on mongod.
 */
struct ClusterAggregateCommandD {
    using Request = AggregateCommandRequest;
    static constexpr std::string_view kCommandName = "clusterAggregate"sv;

    static const std::set<std::string>& getApiVersions() {
        return kNoApiVersions;
    }

    static void doCheckAuthorization(OperationContext* opCtx,
                                     const OpMsgRequest& opMsgRequest,
                                     const PrivilegeVector& privileges) {
        uassert(ErrorCodes::Unauthorized,
                "Unauthorized",
                AuthorizationSession::get(opCtx->getClient())
                    ->isAuthorizedForActionsOnResource(
                        ResourcePattern::forClusterResource(opMsgRequest.getValidatedTenantId()),
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
                  "Cannot explain a cluster aggregate command on a mongod");
    }
};
MONGO_REGISTER_COMMAND(ClusterAggregateCommandBase<ClusterAggregateCommandD>).forShard();

}  // namespace
}  // namespace mongo
