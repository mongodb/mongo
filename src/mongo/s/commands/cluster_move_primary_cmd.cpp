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

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/move_primary_gen.h"
#include "mongo/util/scopeguard.h"

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
                Grid::get(opCtx)->catalogCache()->purgeDatabase(dbNss.db());
            });

            const auto dbInfo =
                uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, dbNss.db()));

            ShardsvrMovePrimary shardsvrRequest{dbNss.dbName()};
            shardsvrRequest.setDbName(DatabaseName::kAdmin);
            shardsvrRequest.getMovePrimaryRequestBase().setTo(toShardId);

            const auto commandResponse = executeCommandAgainstDatabasePrimary(
                opCtx,
                DatabaseName::kAdmin.toString(),
                dbInfo,
                CommandHelpers::appendMajorityWriteConcern(shardsvrRequest.toBSON({}),
                                                           opCtx->getWriteConcern()),
                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                Shard::RetryPolicy::kIdempotent);

            const auto remoteResponse = uassertStatusOK(commandResponse.swResponse);
            uassertStatusOK(getStatusFromCommandResult(remoteResponse.data));
        }

    private:
        NamespaceString ns() const override {
            return NamespaceString(request().getCommandParameter());
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forDatabaseName(ns().db()), ActionType::moveChunk));
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
} movePrimaryCommand;

}  // namespace
}  // namespace mongo
