// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/vote_index_build_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"

#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

/**
 * Confirms that a specified replica set member is ready for commit of the index build identified by
 * the provided index build UUID.
 *
 * {
 *     voteCommitIndexBuild: <index_build_uuid>,
 *     hostAndPort: "host:port",
 * }
 */
class VoteCommitIndexBuildCommand final : public TypedCommand<VoteCommitIndexBuildCommand> {
public:
    using Request = VoteCommitIndexBuild;

    std::string help() const override {
        return "Internal intra replica set command to signal to the primary that a member is ready "
               "to commit an index build.";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            auto lastOpBeforeRun = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();

            const auto& cmd = request();
            LOGV2_DEBUG(3856208,
                        1,
                        "Received voteCommitIndexBuild request",
                        "buildUUID"_attr = cmd.getCommandParameter(),
                        "host"_attr = cmd.getHostAndPort().toString());
            auto voteStatus = IndexBuildsCoordinator::get(opCtx)->voteCommitIndexBuild(
                opCtx, cmd.getCommandParameter(), cmd.getHostAndPort());

            // No need to wait for majority write concern if we fail to persist the voter's info.
            uassertStatusOK(voteStatus);

            auto lastOpAfterRun = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
            // Update the client's last optime to last oplog entry's opTime.
            // This case can hit only if the member has already voted for that index build. So, to
            // make sure the voter's info won't be rolled back, we wait for the oplog's last entry's
            // opTime to be majority replicated.
            if (lastOpBeforeRun == lastOpAfterRun) {
                repl::ReplClientInfo::forClient(opCtx->getClient())
                    .setLastOpToSystemLastOpTime(opCtx);
            }
        }

    private:
        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::internal));
        }
    };
};
MONGO_REGISTER_COMMAND(VoteCommitIndexBuildCommand).forShard();

}  // namespace
}  // namespace mongo
