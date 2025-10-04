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
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/create_database_coordinator.h"
#include "mongo/db/global_catalog/ddl/create_database_coordinator_document_gen.h"
#include "mongo/db/global_catalog/ddl/create_database_util.h"
#include "mongo/db/global_catalog/ddl/ddl_lock_manager.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/util/assert_util.h"

#include <string>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

class ConfigSvrCreateDatabaseCommand final : public TypedCommand<ConfigSvrCreateDatabaseCommand> {
public:
    /**
     * We accept any apiVersion, apiStrict, and/or apiDeprecationErrors forwarded with this internal
     * command.
     */
    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    using Request = ConfigsvrCreateDatabase;
    using Response = ConfigsvrCreateDatabaseResponse;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::IllegalOperation,
                    "_configsvrCreateDatabase can only be run on config servers",
                    serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));
            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            // Set the operation context read concern level to local for reads into the config
            // database.
            repl::ReadConcernArgs::get(opCtx) =
                repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

            const auto dbNameStr = request().getCommandParameter();
            const auto dbName = DatabaseNameUtil::deserialize(
                boost::none, dbNameStr, request().getSerializationContext());

            audit::logEnableSharding(opCtx->getClient(), dbNameStr);

            // Checks for restricted and invalid namespaces.
            if (const auto configDatabaseOpt =
                    create_database_util::checkDbNameConstraints(dbName)) {
                return configDatabaseOpt->getVersion();
            }
            const auto optResolvedPrimaryShard =
                create_database_util::resolvePrimaryShard(opCtx, request().getPrimaryShardId());

            {
                // The use isEnabledAndIgnoreFCVUnsafe is intentional, we want to check whether the
                // feature flag is enabled on any version. This is a hack, but SERVER-102647 will
                // get rid of this code before it hits production. The reason we take the DDL lock
                // here is to respect the acquisition order DDL Lock -> FCV Lock, and avoid
                // deadlocks. This is a pessimization, and thus we only do this if
                // ShardAuthoritativeDbMetadataDDL is active in this binary.
                // (Ignore FCV check): We need to know if the feature flag is active in any version.
                // TODO (SERVER-102647): Remove this code.
                boost::optional<DDLLockManager::ScopedBaseDDLLock> ddlLock;
                if (feature_flags::gShardAuthoritativeDbMetadataDDL.isEnabledAndIgnoreFCVUnsafe()) {
                    ddlLock.emplace(
                        opCtx,
                        shard_role_details::getLocker(opCtx),
                        DatabaseNameUtil::deserialize(boost::none,
                                                      str::toLower(dbNameStr),
                                                      request().getSerializationContext()),
                        "createDatabase" /* reason */,
                        MODE_X,
                        true /*waitForRecovery*/);
                }

                boost::optional<FixedFCVRegion> fixedFcvRegion{opCtx};

                const auto fcvSnapshot = (*fixedFcvRegion)->acquireFCVSnapshot();
                // The Operation FCV is currently propagated only for DDL operations,
                // which cannot be nested. Therefore, the VersionContext shouldn't have
                // been initialized yet.
                invariant(!VersionContext::getDecoration(opCtx).isInitialized());
                const auto createDatabaseDDLCoordinatorFeatureFlagEnabled =
                    feature_flags::gCreateDatabaseDDLCoordinator.isEnabled(
                        VersionContext::getDecoration(opCtx), fcvSnapshot);
                const auto authoritativeMetadataAccessLevel =
                    sharding_ddl_util::getGrantedAuthoritativeMetadataAccessLevel(
                        VersionContext::getDecoration(opCtx), fcvSnapshot);

                if (!createDatabaseDDLCoordinatorFeatureFlagEnabled) {
                    // (Ignore FCV check): The use isEnabledAndIgnoreFCVUnsafe is intentional, we
                    // want to check whether the feature flag is enabled on any version.
                    // We need to maintain the FixedFCVRegion during the execution of the command
                    // to guarantee that all in-flight legacy commands are drained after
                    // transitioning to kUpgrading during FCV upgrade.
                    // TODO (SERVER-102647): unconditionally exit the FixedFCVRegion here
                    if (!feature_flags::gShardAuthoritativeDbMetadataDDL
                             .isEnabledAndIgnoreFCVUnsafe()) {
                        fixedFcvRegion.reset();
                    }

                    auto dbt = ShardingCatalogManager::get(opCtx)->createDatabase(
                        opCtx,
                        dbName,
                        optResolvedPrimaryShard,
                        request().getSerializationContext());

                    return Response(dbt.getVersion());
                } else {
                    CreateDatabaseCoordinatorDocument coordinatorDoc;
                    coordinatorDoc.setShardingDDLCoordinatorMetadata(
                        {{NamespaceString(dbName), DDLCoordinatorTypeEnum::kCreateDatabase}});
                    coordinatorDoc.setPrimaryShard(optResolvedPrimaryShard);
                    coordinatorDoc.setUserSelectedPrimary(optResolvedPrimaryShard.is_initialized());
                    coordinatorDoc.setAuthoritativeMetadataAccessLevel(
                        authoritativeMetadataAccessLevel);
                    auto createDatabaseCoordinator =
                        checked_pointer_cast<CreateDatabaseCoordinator>(
                            ShardingDDLCoordinatorService::getService(opCtx)->getOrCreateInstance(
                                opCtx, coordinatorDoc.toBSON(), *fixedFcvRegion));

                    fixedFcvRegion.reset();
                    ddlLock.reset();
                    return createDatabaseCoordinator->getResult(opCtx);
                }
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

private:
    std::string help() const override {
        return "Internal command, which is exported by the sharding config server. Do not call "
               "directly. Create a database.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }
};
MONGO_REGISTER_COMMAND(ConfigSvrCreateDatabaseCommand).forShard();

}  // namespace mongo
