// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
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
#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

const ReadPreferenceSetting kPrimaryOnlyReadPreference{ReadPreference::PrimaryOnly};

class BalancerControlCommand : public BasicCommand {
public:
    BalancerControlCommand(std::string_view name,
                           std::string_view configsvrCommandName,
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
    const std::string_view _configsvrCommandName;
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
