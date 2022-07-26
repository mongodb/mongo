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


#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/commands/cluster_commands_gen.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

class EnableShardingCmd final : public TypedCommand<EnableShardingCmd> {
public:
    using Request = ClusterCreateDatabase;

    EnableShardingCmd()
        : TypedCommand(ClusterCreateDatabase::kCommandName, ClusterCreateDatabase::kCommandAlias) {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Create a database with the provided options.";
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            const auto dbName = getDbName();

            auto catalogCache = Grid::get(opCtx)->catalogCache();
            ScopeGuard purgeDatabaseOnExit([&] { catalogCache->purgeDatabase(dbName); });

            ConfigsvrCreateDatabase configsvrCreateDatabase{dbName.toString()};
            configsvrCreateDatabase.setDbName(NamespaceString::kAdminDb);
            configsvrCreateDatabase.setPrimaryShardId(request().getPrimaryShard());

            auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
            auto response = uassertStatusOK(configShard->runCommandWithFixedRetryAttempts(
                opCtx,
                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                NamespaceString::kAdminDb.toString(),
                CommandHelpers::appendMajorityWriteConcern(configsvrCreateDatabase.toBSON({})),
                Shard::RetryPolicy::kIdempotent));

            uassertStatusOKWithContext(response.commandStatus,
                                       str::stream()
                                           << "Database " << dbName << " could not be created");
            uassertStatusOK(response.writeConcernStatus);

            auto createDbResponse = ConfigsvrCreateDatabaseResponse::parse(
                IDLParserContext("configsvrCreateDatabaseResponse"), response.response);
            catalogCache->onStaleDatabaseVersion(dbName, createDbResponse.getDatabaseVersion());
            purgeDatabaseOnExit.dismiss();
        }

    private:
        StringData getDbName() const {
            return request().getCommandParameter();
        }
        NamespaceString ns() const override {
            return {getDbName(), ""};
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
                        ResourcePattern::forDatabaseName(getDbName()), ActionType::enableSharding));
        }
    };

} enableShardingCmd;

}  // namespace
}  // namespace mongo
