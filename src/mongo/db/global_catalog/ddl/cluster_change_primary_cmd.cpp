/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/ddl/move_primary_gen.h"
#include "mongo/db/global_catalog/router_role_api/cluster_commands_helpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
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

class ChangePrimaryCommand final : public TypedCommand<ChangePrimaryCommand> {
public:
    using Request = ChangePrimary;

    ChangePrimaryCommand() : TypedCommand(ChangePrimary::kCommandName) {}

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

            ShardsvrChangePrimary shardsvrRequest{dbNss.dbName()};
            shardsvrRequest.setDbName(DatabaseName::kAdmin);
            shardsvrRequest.getMovePrimaryRequestBase().setTo(toShardId);
            generic_argument_util::setMajorityWriteConcern(shardsvrRequest,
                                                           &opCtx->getWriteConcern());

            sharding::router::DBPrimaryRouter router(opCtx->getServiceContext(), dbNss.dbName());
            router.route(opCtx,
                         Request::kCommandParameterFieldName,
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
MONGO_REGISTER_COMMAND(ChangePrimaryCommand).forRouter();

}  // namespace
}  // namespace mongo
