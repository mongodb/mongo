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


#include "mongo/platform/basic.h"

#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/drop_database_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

class DropDatabaseCmd : public DropDatabaseCmdVersion1Gen<DropDatabaseCmd> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }
    class Invocation final : public InvocationBaseGen {
    public:
        using InvocationBaseGen::InvocationBaseGen;
        bool supportsWriteConcern() const final {
            return true;
        }
        NamespaceString ns() const final {
            return NamespaceString(request().getDbName());
        }
        void doCheckAuthorization(OperationContext* opCtx) const final {
            uassert(ErrorCodes::Unauthorized,
                    str::stream() << "Not authorized to drop database '"
                                  << request().getDbName().toStringForErrorMsg() << "'",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnNamespace(ns(), ActionType::dropDatabase));
        }
        Reply typedRun(OperationContext* opCtx) final {
            auto dbName = request().getDbName();

            uassert(ErrorCodes::IllegalOperation,
                    "Cannot drop the config database",
                    dbName != DatabaseName::kConfig);
            uassert(ErrorCodes::IllegalOperation,
                    "Cannot drop the admin database",
                    dbName != DatabaseName::kAdmin);
            uassert(ErrorCodes::BadValue,
                    "Must pass 1 as the 'dropDatabase' parameter",
                    request().getCommandParameter() == 1);

            try {
                const CachedDatabaseInfo dbInfo =
                    uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabase(
                        opCtx, dbName.toStringWithTenantId()));

                // Invalidate the database metadata so the next access kicks off a full reload, even
                // if sending the command to the config server fails due to e.g. a NetworkError.
                ON_BLOCK_EXIT([opCtx, dbName] {
                    Grid::get(opCtx)->catalogCache()->purgeDatabase(dbName.toStringWithTenantId());
                });

                // Send it to the primary shard
                ShardsvrDropDatabase dropDatabaseCommand;
                dropDatabaseCommand.setDbName(dbName);

                auto cmdResponse = executeCommandAgainstDatabasePrimary(
                    opCtx,
                    dbName.db(),
                    dbInfo,
                    CommandHelpers::appendMajorityWriteConcern(dropDatabaseCommand.toBSON({}),
                                                               opCtx->getWriteConcern()),
                    ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                    Shard::RetryPolicy::kIdempotent);

                const auto remoteResponse = uassertStatusOK(cmdResponse.swResponse);
                uassertStatusOK(getStatusFromCommandResult(remoteResponse.data));
            } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                // If the namespace isn't found, treat the drop as a success
            }
            return {};
        }
    };
} clusterDropDatabaseCmd;

}  // namespace
}  // namespace mongo
