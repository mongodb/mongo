// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/query/parsed_find_command.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/s/commands/query_cmd/cluster_find_cmd.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

/**
 * Implements the cluster find command on mongod.
 */
struct ClusterFindCmdD {
    using Request = FindCommandRequest;
    static constexpr std::string_view kCommandName = "clusterFind"sv;

    static const std::set<std::string>& getApiVersions() {
        return kNoApiVersions;
    }

    static void doCheckAuthorization(OperationContext* opCtx,
                                     bool hasTerm,
                                     const NamespaceString& nss) {
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

    static void checkCanExplainHere(OperationContext* opCtx) {
        uasserted(ErrorCodes::CommandNotSupported,
                  "Cannot explain a cluster find command on a mongod");
    }
};
MONGO_REGISTER_COMMAND(ClusterFindCmdBase<ClusterFindCmdD>).forShard();

}  // namespace
}  // namespace mongo
