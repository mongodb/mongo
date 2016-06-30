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
#include "mongo/s/balancer/balancer.h"
#include "mongo/s/balancer/balancer_configuration.h"
#include "mongo/s/grid.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

class ConfigSvrBalancerControlCommand : public Command {
public:
    ConfigSvrBalancerControlCommand(StringData name) : Command(name) {}

    void help(std::stringstream& help) const override {
        help << "Internal command, which is exported by the sharding config server. Do not call "
                "directly. Controls the balancer state.";
    }

    bool slaveOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    Status checkAuthForCommand(ClientBasic* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::internal)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    bool run(OperationContext* txn,
             const std::string& unusedDbName,
             BSONObj& cmdObj,
             int options,
             std::string& errmsg,
             BSONObjBuilder& result) final {
        if (cmdObj.firstElementFieldName() != getName()) {
            uasserted(ErrorCodes::InternalError,
                      str::stream() << "Expected to find a " << getName() << " command, but found "
                                    << cmdObj);
        }

        if (serverGlobalParams.clusterRole != ClusterRole::ConfigServer) {
            uasserted(ErrorCodes::IllegalOperation,
                      str::stream() << getName() << " can only be run on config servers");
        }

        _run(txn, &result);

        return true;
    }

private:
    virtual void _run(OperationContext* txn, BSONObjBuilder* result) = 0;
};

class ConfigSvrBalancerStartCommand : public ConfigSvrBalancerControlCommand {
public:
    ConfigSvrBalancerStartCommand() : ConfigSvrBalancerControlCommand("_configsvrBalancerStart") {}

private:
    void _run(OperationContext* txn, BSONObjBuilder* result) override {
        uassertStatusOK(Grid::get(txn)->getBalancerConfiguration()->setBalancerMode(
            txn, BalancerSettingsType::kFull));
    }
};

class ConfigSvrBalancerStopCommand : public ConfigSvrBalancerControlCommand {
public:
    ConfigSvrBalancerStopCommand() : ConfigSvrBalancerControlCommand("_configsvrBalancerStop") {}

private:
    void _run(OperationContext* txn, BSONObjBuilder* result) override {
        uassertStatusOK(Grid::get(txn)->getBalancerConfiguration()->setBalancerMode(
            txn, BalancerSettingsType::kOff));
        Balancer::get(txn)->joinCurrentRound(txn);
    }
};

class ConfigSvrBalancerStatusCommand : public ConfigSvrBalancerControlCommand {
public:
    ConfigSvrBalancerStatusCommand()
        : ConfigSvrBalancerControlCommand("_configsvrBalancerStatus") {}

private:
    void _run(OperationContext* txn, BSONObjBuilder* result) override {
        Balancer::get(txn)->report(txn, result);
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
