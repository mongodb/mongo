// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/database_name_util.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/cluster_commands_gen.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>

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

            ConfigsvrCreateDatabase configsvrCreateDatabase{
                DatabaseNameUtil::serialize(dbName, request().getSerializationContext())};
            configsvrCreateDatabase.setDbName(DatabaseName::kAdmin);
            configsvrCreateDatabase.setPrimaryShardId(request().getPrimaryShard());
            generic_argument_util::setMajorityWriteConcern(configsvrCreateDatabase);

            auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
            auto response = uassertStatusOK(
                configShard->runCommand(opCtx,
                                        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                        DatabaseName::kAdmin,
                                        configsvrCreateDatabase.toBSON(),
                                        Shard::RetryPolicy::kIdempotent));

            uassertStatusOKWithContext(response.commandStatus,
                                       str::stream() << "Database " << dbName.toStringForErrorMsg()
                                                     << " could not be created");
            uassertStatusOK(response.writeConcernStatus);

            auto createDbResponse = ConfigsvrCreateDatabaseResponse::parse(
                response.response, IDLParserContext("configsvrCreateDatabaseResponse"));
            catalogCache->onStaleDatabaseVersion(dbName, createDbResponse.getDatabaseVersion());
            purgeDatabaseOnExit.dismiss();
        }

    private:
        DatabaseName getDbName() const {
            const auto& cmd = request();
            return DatabaseNameUtil::deserialize(cmd.getDbName().tenantId(),
                                                 cmd.getCommandParameter(),
                                                 cmd.getSerializationContext());
        }
        NamespaceString ns() const override {
            return NamespaceString(getDbName());
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
};
MONGO_REGISTER_COMMAND(EnableShardingCmd).forRouter();

}  // namespace
}  // namespace mongo
