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
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

const ReadPreferenceSetting kPrimaryOnlyReadPreference{ReadPreference::PrimaryOnly};

class BalancerControlCommand : public BasicCommand {
public:
    BalancerControlCommand(StringData name,
                           StringData configsvrCommandName,
                           ActionType authorizationAction,
                           bool logCommand)
        : BasicCommand(name),
          _configsvrCommandName(configsvrCommandName),
          _authorizationAction(authorizationAction),
          _logCommand(logCommand) {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    std::string help() const override {
        return "Starts or stops the sharding balancer.";
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        if (!AuthorizationSession::get(opCtx->getClient())
                 ->isAuthorizedForActionsOnResource(
                     ResourcePattern::forExactNamespace(NamespaceString::kConfigSettingsNamespace),
                     _authorizationAction)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

        if (_logCommand)
            LOGV2(5054300,
                  "About to run balancer control command",
                  "cmd"_attr = _configsvrCommandName);

        auto cmdResponse =
            uassertStatusOK(configShard->runCommand(opCtx,
                                                    kPrimaryOnlyReadPreference,
                                                    DatabaseName::kAdmin,
                                                    BSON(_configsvrCommandName << 1),
                                                    Shard::RetryPolicy::kIdempotent));
        uassertStatusOK(cmdResponse.commandStatus);

        // Append any return value from the response, which the config server returned
        CommandHelpers::filterCommandReplyForPassthrough(cmdResponse.response, &result);

        return true;
    }

private:
    const StringData _configsvrCommandName;
    const ActionType _authorizationAction;
    const bool _logCommand;
};

class BalancerStartCommand : public BalancerControlCommand {
public:
    BalancerStartCommand()
        : BalancerControlCommand(
              "balancerStart", "_configsvrBalancerStart", ActionType::update, true /* log cmd */) {}
};

class BalancerStopCommand : public BalancerControlCommand {
public:
    BalancerStopCommand()
        : BalancerControlCommand(
              "balancerStop", "_configsvrBalancerStop", ActionType::update, true /* log cmd */) {}
};

class BalancerStatusCommand : public BalancerControlCommand {
public:
    BalancerStatusCommand()
        : BalancerControlCommand("balancerStatus",
                                 "_configsvrBalancerStatus",
                                 ActionType::find,
                                 false /* do not log cmd */) {}
};

MONGO_REGISTER_COMMAND(BalancerStartCommand).forRouter();
MONGO_REGISTER_COMMAND(BalancerStopCommand).forRouter();
MONGO_REGISTER_COMMAND(BalancerStatusCommand).forRouter();

}  // namespace
}  // namespace mongo
