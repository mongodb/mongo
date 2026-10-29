// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/move_primary_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

class MovePrimaryCommand final : public TypedCommand<MovePrimaryCommand> {
public:
    using Request = MovePrimary;

    MovePrimaryCommand() : TypedCommand(MovePrimary::kCommandName, MovePrimary::kCommandAlias) {}

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            const auto& dbNss = ns();
            const auto& toShardId = request().getTo();

            ScopeGuard onBlockExit([&] {
                // Invalidate the routing table cache entry for this database in order to reload it
                // at the next access, even if sending the command to the primary shard fails (e.g.,
                // NetworkError).
                Grid::get(opCtx)->catalogCache()->purgeDatabase(dbNss.dbName());
            });

            ShardsvrMovePrimary shardsvrRequest{dbNss.dbName()};
            shardsvrRequest.setDbName(DatabaseName::kAdmin);
            shardsvrRequest.getMovePrimaryRequestBase().setTo(toShardId);
            generic_argument_util::setMajorityWriteConcern(shardsvrRequest,
                                                           &opCtx->getWriteConcern());

            sharding::router::DBPrimaryRouter router(opCtx, dbNss.dbName());
            router.route(Request::kCommandParameterFieldName,
                         [&](OperationContext* opCtx, const CachedDatabaseInfo& dbInfo) {
                             const auto commandResponse =
                                 executeCommandAgainstDatabasePrimaryOnlyAttachingDbVersion(
                                     opCtx,
                                     DatabaseName::kAdmin,
                                     dbInfo,
                                     shardsvrRequest.toBSON(),
                                     ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                     Shard::RetryPolicy::kIdempotent);

                             const auto remoteResponse =
                                 uassertStatusOK(commandResponse.swResponse);
                             uassertStatusOK(getStatusFromCommandResult(remoteResponse.data));
                         });
        }

    private:
        NamespaceString ns() const override {
            return NamespaceString(request().getCommandParameter());
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(
                ErrorCodes::Unauthorized,
                "Unauthorized",
                AuthorizationSession::get(opCtx->getClient())
                    ->isAuthorizedForActionsOnResource(
                        ResourcePattern::forDatabaseName(ns().dbName()), ActionType::moveChunk));
        }
    };

private:
    bool adminOnly() const override {
        return true;
    }

    bool skipApiVersionCheck() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext* context) const override {
        return AllowedOnSecondary::kNever;
    }

    std::string help() const override {
        return "Reassigns the primary shard holding all un-sharded collections in the database";
    }
};
MONGO_REGISTER_COMMAND(MovePrimaryCommand).forRouter();

}  // namespace
}  // namespace mongo
