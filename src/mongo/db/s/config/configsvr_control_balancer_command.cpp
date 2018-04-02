/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/base/init.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/s/balancer/balancer.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/grid.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

class ConfigSvrBalancerControlCommand : public BasicCommand {
public:
    ConfigSvrBalancerControlCommand(StringData name) : BasicCommand(name) {}

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

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::internal)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const std::string& unusedDbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) final {
        uassert(ErrorCodes::InternalError,
                str::stream() << "Expected to find a " << getName() << " command, but found "
                              << cmdObj,
                cmdObj.firstElementFieldName() == getName());

        uassert(ErrorCodes::IllegalOperation,
                str::stream() << getName() << " can only be run on config servers",
                serverGlobalParams.clusterRole == ClusterRole::ConfigServer);

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
        uassertStatusOK(Grid::get(opCtx)->getBalancerConfiguration()->setBalancerMode(
            opCtx, BalancerSettingsType::kFull));
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

        uassertStatusOK(Grid::get(opCtx)->getBalancerConfiguration()->setBalancerMode(
            opCtx, BalancerSettingsType::kOff));
        Balancer::get(opCtx)->joinCurrentRound(opCtx);
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

MONGO_INITIALIZER(ClusterBalancerControlCommands)(InitializerContext* context) {
    new ConfigSvrBalancerStartCommand();
    new ConfigSvrBalancerStopCommand();
    new ConfigSvrBalancerStatusCommand();

    return Status::OK();
}

}  // namespace
}  // namespace mongo
