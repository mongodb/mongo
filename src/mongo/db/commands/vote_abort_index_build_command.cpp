/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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


#include <memory>
#include <string>

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/vote_index_build_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/net/hostandport.h"

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
                        "Received voteAbortIndexBuild request for index build: {buildUUID}, "
                        "from host: {host}",
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

MONGO_REGISTER_COMMAND(VoteAbortIndexBuildCommand)
    .requiresFeatureFlag(&feature_flags::gIndexBuildGracefulErrorHandling)
    .forShard();

}  // namespace
}  // namespace mongo
