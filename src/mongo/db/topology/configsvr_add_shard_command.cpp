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
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/sharding_environment/sharding_initialization_mongod.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/topology/add_shard_coordinator.h"
#include "mongo/db/topology/add_shard_gen.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

/**
 * Internal sharding command run on config servers to add a shard to the cluster.
 */
class ConfigSvrAddShardCommand : public TypedCommand<ConfigSvrAddShardCommand> {
public:
    using Request = ConfigsvrAddShard;
    using Response = AddShardResponse;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            uassert(ErrorCodes::IllegalOperation,
                    "_configsvrAddShard can only be run on config servers",
                    serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));
            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            const auto shardIdentity = ShardingInitializationMongoD::getShardIdentityDoc(opCtx);
            invariant(shardIdentity);
            uassert(
                ErrorCodes::IllegalOperation,
                "This configserver was not fully promoted to sharded cluster. Either finish the "
                "process or intervene by removing the shard identity and restart the configserver",
                !shardIdentity->getDeferShardingInitialization().value_or(false));

            // Set the operation context read concern level to local for reads into the config
            // database.
            repl::ReadConcernArgs::get(opCtx) =
                repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

            const auto target = request().getCommandParameter();
            const auto name = request().getName()
                ? boost::make_optional(std::string{*request().getName()})
                : boost::none;

            auto replCoord = repl::ReplicationCoordinator::get(opCtx);
            auto validationStatus = _validate(target, replCoord->getConfig().isLocalHostAllowed());
            uassertStatusOK(validationStatus);

            audit::logAddShard(Client::getCurrent(), name ? name.value() : "", target.toString());

            // TODO(SERVER-97816): remove DDL locking and move the fcv upgrade checking logic to the
            // coordinator
            boost::optional<DDLLockManager::ScopedCollectionDDLLock> ddlLock{
                boost::in_place_init,
                opCtx,
                NamespaceString::kConfigsvrShardsNamespace,
                "addShard",
                LockMode::MODE_X};
            boost::optional<FixedFCVRegion> fcvRegion{boost::in_place_init, opCtx};
            const auto fcvSnapshot = (*fcvRegion)->acquireFCVSnapshot();

            // (Generic FCV reference): These FCV checks should exist across LTS binary versions.
            uassert(ErrorCodes::ConflictingOperationInProgress,
                    "Cannot add shard while in upgrading/downgrading FCV state",
                    !fcvSnapshot.isUpgradingOrDowngrading());

            if (feature_flags::gUseTopologyChangeCoordinators.isEnabled(
                    VersionContext::getDecoration(opCtx), fcvSnapshot)) {
                return _runNewPath(opCtx, ddlLock, fcvRegion, target, name);
            }

            return _runOldPath(opCtx, *fcvRegion, target, name);
        }

    private:
        Response _runOldPath(OperationContext* opCtx,
                             const FixedFCVRegion& fcvRegion,
                             const mongo::ConnectionString& target,
                             boost::optional<std::string> name) {
            StatusWith<std::string> addShardResult = ShardingCatalogManager::get(opCtx)->addShard(
                opCtx, fcvRegion, name ? &(name.value()) : nullptr, target, false);

            Status status = addShardResult.getStatus();

            if (!status.isOK()) {
                LOGV2(21920,
                      "addShard request failed",
                      "request"_attr = request(),
                      "error"_attr = status);
                uassertStatusOK(status);
            }

            Response result;
            result.setShardAdded(addShardResult.getValue());

            return result;
        }

        Response _runNewPath(OperationContext* opCtx,
                             boost::optional<DDLLockManager::ScopedCollectionDDLLock>& ddlLock,
                             boost::optional<FixedFCVRegion>& fcvRegion,
                             const mongo::ConnectionString& target,
                             boost::optional<std::string> name) {
            invariant(ddlLock);
            invariant(fcvRegion);

            // Since the addShardCoordinator will call functions that will take the FixedFCVRegion
            // the ordering of locks will be DDLLock, FcvLock. We want to maintain this lock
            // ordering to avoid deadlocks. If we only take the FixedFCVRegion before creating the
            // addShardCoordinator, then if it starts to run before we can release the
            // FixedFCVRegion the lock ordering will be reversed (FcvLock, DDLLock). It is safe to
            // take the DDLLock before create the coordinator, as it will only prevent the running
            // of the coordinator while we hold the FixedFCVRegion (FcvLock, DDLLock -> waiting for
            // DDLLock in coordinator). After this we release the locks in reversed order, so we are
            // sure that we are not holding the FixedFCVRegion while we acquire the DDLLock.
            const auto addShardCoordinator = AddShardCoordinator::create(
                opCtx, *fcvRegion, target, name, /*isConfigShard*/ false);

            fcvRegion.reset();
            ddlLock.reset();

            const auto finalName = addShardCoordinator->getResult(opCtx);

            Response result;
            result.setShardAdded(finalName);

            return result;
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        NamespaceString ns() const override {
            return {};
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::internal));
        }

        static Status _validate(const ConnectionString& target, bool allowLocalHost) {
            // Check that if one of the new shard's hosts is localhost, we are allowed to use
            // localhost as a hostname. (Using localhost requires that every server in the cluster
            // uses localhost).
            for (const auto& serverAddr : target.getServers()) {
                if (serverAddr.isLocalHost() != allowLocalHost) {
                    std::string errmsg = str::stream()
                        << "Can't use localhost as a shard since all shards need to"
                        << " communicate. Either use all shards and configdbs in localhost"
                        << " or all in actual IPs. host: " << serverAddr.toString()
                        << " isLocalHost:" << serverAddr.isLocalHost();
                    return Status(ErrorCodes::InvalidOptions, errmsg);
                }
            }
            return Status::OK();
        }
    };

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command, which is exported by the sharding config server. Do not call "
               "directly. Validates and adds a new shard to the cluster.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }
};
MONGO_REGISTER_COMMAND(ConfigSvrAddShardCommand).forShard();

}  // namespace mongo
