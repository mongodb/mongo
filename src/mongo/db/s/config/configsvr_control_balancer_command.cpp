// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/s/balancer/balancer.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_logging.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string>
#include <string_view>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace {

class ConfigSvrBalancerControlCommand : public BasicCommand {
public:
    ConfigSvrBalancerControlCommand(std::string_view name) : BasicCommand(name) {}

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command, which is exported by the sharding config server. Do not call "
               "directly. Controls the balancer state.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj&) const override {
        if (!AuthorizationSession::get(opCtx->getClient())
                 ->isAuthorizedForActionsOnResource(
                     ResourcePattern::forClusterResource(dbName.tenantId()),
                     ActionType::internal)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) final {
        uassert(ErrorCodes::InternalError,
                str::stream() << "Expected to find a " << getName() << " command, but found "
                              << cmdObj,
                cmdObj.firstElementFieldName() == getName());

        uassert(ErrorCodes::IllegalOperation,
                str::stream() << getName() << " can only be run on config servers",
                serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));

        _run(opCtx, &result);

        return true;
    }

private:
    virtual void _run(OperationContext* opCtx, BSONObjBuilder* result) = 0;
};

class ConfigSvrBalancerStartCommand : public ConfigSvrBalancerControlCommand {
public:
    ConfigSvrBalancerStartCommand() : ConfigSvrBalancerControlCommand("_configsvrBalancerStart") {}

private:
    void _run(OperationContext* opCtx, BSONObjBuilder* result) override {
        auto balancerConfig = Grid::get(opCtx)->getBalancerConfiguration();
        uassertStatusOK(balancerConfig->setBalancerMode(opCtx, BalancerModeEnum::kFull));
        uassertStatusOK(balancerConfig->changeAutoMergeSettings(opCtx, true));
        Balancer::get(opCtx)->notifyPersistedBalancerSettingsChanged(opCtx);
        auto catalogManager = ShardingCatalogManager::get(opCtx);
        ShardingLogging::get(opCtx)
            ->logAction(opCtx,
                        "balancer.start",
                        NamespaceString::kEmpty,
                        BSONObj(),
                        catalogManager->localConfigShard(),
                        catalogManager->localCatalogClient())
            .ignore();
    }
};

class ConfigSvrBalancerStopCommand : public ConfigSvrBalancerControlCommand {
public:
    ConfigSvrBalancerStopCommand() : ConfigSvrBalancerControlCommand("_configsvrBalancerStop") {}

private:
    void _run(OperationContext* opCtx, BSONObjBuilder* result) override {

        // Set the operation context read concern level to local for reads into the config database.
        repl::ReadConcernArgs::get(opCtx) =
            repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

        auto balancerConfig = Grid::get(opCtx)->getBalancerConfiguration();
        uassertStatusOK(balancerConfig->setBalancerMode(opCtx, BalancerModeEnum::kOff));
        uassertStatusOK(balancerConfig->changeAutoMergeSettings(opCtx, false));

        Balancer::get(opCtx)->notifyPersistedBalancerSettingsChanged(opCtx);
        Balancer::get(opCtx)->joinCurrentRound(opCtx);

        auto catalogManager = ShardingCatalogManager::get(opCtx);
        ShardingLogging::get(opCtx)
            ->logAction(opCtx,
                        "balancer.stop",
                        NamespaceString::kEmpty,
                        BSONObj(),
                        catalogManager->localConfigShard(),
                        catalogManager->localCatalogClient())
            .ignore();
    }
};

class ConfigSvrBalancerStatusCommand : public ConfigSvrBalancerControlCommand {
public:
    ConfigSvrBalancerStatusCommand()
        : ConfigSvrBalancerControlCommand("_configsvrBalancerStatus") {}

private:
    void _run(OperationContext* opCtx, BSONObjBuilder* result) override {
        Balancer::get(opCtx)->report(opCtx, result);
    }
};

MONGO_REGISTER_COMMAND(ConfigSvrBalancerStartCommand).forShard();
MONGO_REGISTER_COMMAND(ConfigSvrBalancerStopCommand).forShard();
MONGO_REGISTER_COMMAND(ConfigSvrBalancerStatusCommand).forShard();

}  // namespace
}  // namespace mongo
