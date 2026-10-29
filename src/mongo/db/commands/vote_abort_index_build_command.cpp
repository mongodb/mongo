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
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"

#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

/**
 * Requests abortion of the index build identified by the provided index build UUID.
 *
 * {
 *     voteAbortIndexBuild: <index_build_uuid>,
 *     hostAndPort: "host:port",
 * }
 */
class VoteAbortIndexBuildCommand final : public TypedCommand<VoteAbortIndexBuildCommand> {
public:
    using Request = VoteAbortIndexBuild;

    std::string help() const override {
        return "Internal intra replica set command to request that the primary abort an index "
               "build with the specified UUID.";
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
            LOGV2_DEBUG(7329200,
                        1,
                        "Received voteAbortIndexBuild request",
                        "buildUUID"_attr = cmd.getCommandParameter(),
                        "host"_attr = cmd.getHostAndPort().toString(),
                        "reason"_attr = cmd.getReason());

            uassertStatusOK(IndexBuildsCoordinator::get(opCtx)->voteAbortIndexBuild(
                opCtx, cmd.getCommandParameter(), cmd.getHostAndPort(), cmd.getReason()));

            auto lastOpAfterRun = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
            // If the client's lastOp before and after abort are equal, this means the index build
            // was already aborted, and no "abortIndexBuild" oplog entry is generated under this
            // client. We must still ensure a correct writeConcern wait on the pre-existing
            // "abortIndexBuild" oplog entry before replying. We guarantee this by waiting for the
            // system's last op time.
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

MONGO_REGISTER_COMMAND(VoteAbortIndexBuildCommand).forShard();

}  // namespace
}  // namespace mongo
