// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/getmore_command_gen.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/s/commands/query_cmd/cluster_getmore_cmd.h"
#include "mongo/util/assert_util.h"

#include <set>
#include <string>
#include <string_view>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

/**
 * Implements the cluster getMore command on mongod.
 */
struct ClusterGetMoreCmdD {
    using Request = GetMoreCommandRequest;
    using Reply = GetMoreCommandRequest::Reply;
    static constexpr std::string_view kCommandName = "clusterGetMore"sv;

    static const std::set<std::string>& getApiVersions() {
        return kNoApiVersions;
    }

    static void doCheckAuthorization(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     long long cursorID,
                                     bool hasTerm) {
        uassert(ErrorCodes::Unauthorized,
                "Unauthorized",
                AuthorizationSession::get(opCtx->getClient())
                    ->isAuthorizedForActionsOnResource(
                        ResourcePattern::forClusterResource(nss.tenantId()), ActionType::internal));
    }

    static void checkCanRunHere(OperationContext* opCtx) {
        Grid::get(opCtx)->assertShardingIsInitialized();

        // A cluster command on the config server may attempt to use a ShardLocal to target itself,
        // which triggers an invariant, so only shard servers can run this.
        ShardingState::get(opCtx)->assertCanAcceptShardedCommands();
    }
};
MONGO_REGISTER_COMMAND(ClusterGetMoreCmdBase<ClusterGetMoreCmdD>).forShard();

}  // namespace
}  // namespace mongo
