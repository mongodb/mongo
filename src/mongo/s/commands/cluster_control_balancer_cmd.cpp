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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/base/init.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"

namespace mongo {
namespace {

const ReadPreferenceSetting kPrimaryOnlyReadPreference{ReadPreference::PrimaryOnly};

class BalancerControlCommand : public Command {
public:
    BalancerControlCommand(StringData name,
                           StringData configsvrCommandName,
                           ActionType authorizationAction)
        : Command(name),
          _configsvrCommandName(configsvrCommandName),
          _authorizationAction(authorizationAction) {}

    bool slaveOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    void help(std::stringstream& help) const override {
        help << "Starts or stops the sharding balancer.";
    }

    Status checkAuthForCommand(ClientBasic* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(NamespaceString("config", "settings")),
                _authorizationAction)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    bool run(OperationContext* txn,
             const std::string& dbname,
             BSONObj& cmdObj,
             int options,
             std::string& errmsg,
             BSONObjBuilder& result) override {
        auto configShard = Grid::get(txn)->shardRegistry()->getConfigShard();
        auto cmdResponse =
            uassertStatusOK(configShard->runCommand(txn,
                                                    kPrimaryOnlyReadPreference,
                                                    "admin",
                                                    BSON(_configsvrCommandName << 1),
                                                    Shard::RetryPolicy::kIdempotent));
        uassertStatusOK(cmdResponse.commandStatus);

        // Append any return value from the response, which the config server returned
        result.appendElements(cmdResponse.response);

        return true;
    }

private:
    const StringData _configsvrCommandName;
    const ActionType _authorizationAction;
};

class BalancerStartCommand : public BalancerControlCommand {
public:
    BalancerStartCommand()
        : BalancerControlCommand("balancerStart", "_configsvrBalancerStart", ActionType::update) {}
};

class BalancerStopCommand : public BalancerControlCommand {
public:
    BalancerStopCommand()
        : BalancerControlCommand("balancerStop", "_configsvrBalancerStop", ActionType::update) {}
};

class BalancerStatusCommand : public BalancerControlCommand {
public:
    BalancerStatusCommand()
        : BalancerControlCommand("balancerStatus", "_configsvrBalancerStatus", ActionType::find) {}
};

MONGO_INITIALIZER(ClusterBalancerControlCommands)(InitializerContext* context) {
    new BalancerStartCommand();
    new BalancerStopCommand();
    new BalancerStatusCommand();

    return Status::OK();
}

}  // namespace
}  // namespace mongo
