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
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
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

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace {

class ConfigSvrBalancerControlCommand : public BasicCommand {
public:
    ConfigSvrBalancerControlCommand(StringData name) : BasicCommand(name) {}

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
        uassertStatusOK(balancerConfig->setBalancerMode(opCtx, BalancerSettingsType::kFull));
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
        uassertStatusOK(balancerConfig->setBalancerMode(opCtx, BalancerSettingsType::kOff));
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
