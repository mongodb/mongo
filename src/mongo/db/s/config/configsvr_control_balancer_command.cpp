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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/s/balancer/balancer.h"
#include "mongo/s/balancer/balancer_configuration.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/control_balancer_request_type.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

class ConfigSvrControlBalancerCommand : public Command {
public:
    ConfigSvrControlBalancerCommand() : Command("_configsvrControlBalancer") {}

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
             BSONObjBuilder& result) override {
        if (serverGlobalParams.clusterRole != ClusterRole::ConfigServer) {
            return appendCommandStatus(
                result,
                Status(ErrorCodes::IllegalOperation,
                       "_configsvrControlBalancer can only be run on config servers"));
        }

        const auto request =
            uassertStatusOK(ControlBalancerRequest::parseFromConfigCommand(cmdObj));
        auto balancerConfig = Grid::get(txn)->getBalancerConfiguration();

        Status writeBalancerConfigStatus{ErrorCodes::InternalError, "Not initialized"};

        switch (request.getAction()) {
            case ControlBalancerRequest::kStart:
                writeBalancerConfigStatus = balancerConfig->setBalancerActive(txn, true);
                uassertStatusOK(Balancer::get(txn)->startThread(txn));
                break;
            case ControlBalancerRequest::kStop:
                writeBalancerConfigStatus = balancerConfig->setBalancerActive(txn, false);
                Balancer::get(txn)->stopThread();
                Balancer::get(txn)->joinThread();
                break;
            default:
                MONGO_UNREACHABLE;
        }

        if (!writeBalancerConfigStatus.isOK()) {
            uasserted(writeBalancerConfigStatus.code(),
                      str::stream()
                          << "Balancer thread state was changed successfully, but the balancer "
                             "configuration setting could not be persisted due to error '"
                          << writeBalancerConfigStatus.reason()
                          << "'");
        }

        return true;
    }

} configSvrControlBalancerCmd;

}  // namespace
}  // namespace mongo
