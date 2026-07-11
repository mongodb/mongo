// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/rwc_defaults_commands_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/read_write_concern_defaults_gen.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>
#include <utility>
#include <variant>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

/**
 * Implements the setDefaultRWConcern command on mongos. Inherits from BasicCommand because this
 * command forwards the user's request to the config server and does not need to parse it.
 */
class ClusterSetDefaultRWConcernCommand : public BasicCommand {
public:
    ClusterSetDefaultRWConcernCommand() : BasicCommand("setDefaultRWConcern") {}

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
        auto cmdResponse = uassertStatusOK(
            configShard->runCommand(opCtx,
                                    ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                    DatabaseName::kAdmin,
                                    // TODO SERVER-91373: Remove appendMajorityWriteConcern
                                    CommandHelpers::appendMajorityWriteConcern(
                                        CommandHelpers::filterCommandRequestForPassthrough(cmdObj),
                                        opCtx->getWriteConcern()),
                                    Shard::RetryPolicy::kNotIdempotent));

        CommandHelpers::filterCommandReplyForPassthrough(cmdResponse.response, &result);

        if (!cmdResponse.commandStatus.isOK() || !cmdResponse.writeConcernStatus.isOK()) {
            return false;
        }

        // Quickly pick up the new defaults by setting them in the cache.
        auto newDefaults = RWConcernDefault::parse(cmdResponse.response,
                                                   IDLParserContext("ClusterSetDefaultRWConcern"));
        if (auto optWC = newDefaults.getDefaultWriteConcern()) {
            if (optWC->hasCustomWriteMode()) {
                LOGV2_WARNING(
                    6081700,
                    "A custom write concern is being set as the default write concern in a sharded "
                    "cluster. This set is unchecked, but if the custom write concern does not "
                    "exist on all shards in the cluster, errors will occur upon writes",
                    "customWriteConcern"_attr = get<std::string>(optWC->w));
            }
        }
        ReadWriteConcernDefaults::get(opCtx).setDefault(opCtx, std::move(newDefaults));

        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj&) const override {
        if (!AuthorizationSession::get(opCtx->getClient())
                 ->isAuthorizedForPrivilege(
                     Privilege{ResourcePattern::forClusterResource(dbName.tenantId()),
                               ActionType::setDefaultRWConcern})) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    std::string help() const override {
        return "Sets the default read or write concern for a cluster";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
};
MONGO_REGISTER_COMMAND(ClusterSetDefaultRWConcernCommand).forRouter();

/**
 * Implements the getDefaultRWConcern command on mongos.
 */
class ClusterGetDefaultRWConcernCommand final
    : public TypedCommand<ClusterGetDefaultRWConcernCommand> {
public:
    using Request = GetDefaultRWConcern;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        GetDefaultRWConcernResponse typedRun(OperationContext* opCtx) {
            auto& rwcDefaults = ReadWriteConcernDefaults::get(opCtx);
            if (request().getInMemory().value_or(false)) {
                const auto rwcDefault = rwcDefaults.getDefault(opCtx);
                GetDefaultRWConcernResponse response;
                response.setRWConcernDefault(rwcDefault);
                response.setLocalUpdateWallClockTime(rwcDefault.localUpdateWallClockTime());
                response.setInMemory(true);
                return response;
            }

            // If not asking for the in-memory defaults, fetch them from the config server
            GetDefaultRWConcern configsvrRequest;
            configsvrRequest.setDbName(request().getDbName());
            setReadWriteConcern(opCtx, configsvrRequest, this);

            auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
            auto cmdResponse = uassertStatusOK(
                configShard->runCommand(opCtx,
                                        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                        DatabaseName::kAdmin,
                                        configsvrRequest.toBSON(),
                                        Shard::RetryPolicy::kIdempotent));

            uassertStatusOK(cmdResponse.commandStatus);

            return GetDefaultRWConcernResponse::parse(
                cmdResponse.response, IDLParserContext("ClusterGetDefaultRWConcernResponse"));
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForPrivilege(Privilege{
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::getDefaultRWConcern}));
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }
    };

    std::string help() const override {
        return "Gets the default read or write concern for a cluster";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
};
MONGO_REGISTER_COMMAND(ClusterGetDefaultRWConcernCommand).forRouter();

}  // namespace
}  // namespace mongo
