// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/query_settings_cmds_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/cluster_parameters/cluster_server_parameter_refresher.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace {

void refreshClusterParameters(OperationContext* opCtx) {
    const bool kEnsureReadYourWritesConsistency = true;
    auto* clusterParameterRefresher = ClusterServerParameterRefresher::get(opCtx);

    // Early exit if 'clusterParameterRefresher' is not initialized, which may be the case in
    // embedded routers.
    if (!clusterParameterRefresher) {
        return;
    }

    auto refreshStatus =
        clusterParameterRefresher->refreshParameters(opCtx, kEnsureReadYourWritesConsistency);
    if (!refreshStatus.isOK()) {
        LOGV2_WARNING(8472500,
                      "Error occurred when fetching the latest version of query settings",
                      "error_code"_attr = refreshStatus.code(),
                      "reason"_attr = refreshStatus.reason());
    }
}

class SetQuerySettingsCommand final : public TypedCommand<SetQuerySettingsCommand> {
public:
    using Request = SetQuerySettingsCommandRequest;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Sets the query settings for the query shape of a given query.";
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        SetQuerySettingsCommandReply typedRun(OperationContext* opCtx) {
            const auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
            const auto cmdResponse = uassertStatusOK(configShard->runCommand(
                opCtx,
                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                DatabaseName::kAdmin,
                CommandHelpers::filterCommandRequestForPassthrough(unparsedRequest().body),
                Shard::RetryPolicy::kIdempotent));
            uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(cmdResponse));
            auto reply = SetQuerySettingsCommandReply::parse(cmdResponse.response,
                                                             IDLParserContext("setQuerySettings"));

            // Force the cluster paramaters refresh to ensure the router now observes the latest
            // QuerySettings.
            refreshClusterParameters(opCtx);
            return reply;
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        NamespaceString ns() const override {
            return NamespaceString::kEmpty;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForPrivilege(Privilege{
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::querySettings}));
        }
    };
};
MONGO_REGISTER_COMMAND(SetQuerySettingsCommand).forRouter();

class RemoveQuerySettingsCommand final : public TypedCommand<RemoveQuerySettingsCommand> {
public:
    using Request = RemoveQuerySettingsCommandRequest;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Removes the query settings for the query shape of a given query.";
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            const auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
            const auto cmdResponse = uassertStatusOK(configShard->runCommand(
                opCtx,
                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                DatabaseName::kAdmin,
                CommandHelpers::filterCommandRequestForPassthrough(request().toBSON()),
                Shard::RetryPolicy::kIdempotent));

            uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(cmdResponse));

            // Force the cluster paramaters refresh to ensure the router now observes the latest
            // QuerySettings.
            refreshClusterParameters(opCtx);
            return;
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        NamespaceString ns() const override {
            return NamespaceString::kEmpty;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForPrivilege(Privilege{
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::querySettings}));
        }
    };
};
MONGO_REGISTER_COMMAND(RemoveQuerySettingsCommand).forRouter();

}  // namespace
}  // namespace mongo
