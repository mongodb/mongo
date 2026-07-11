// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_set.h"
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
#include "mongo/rpc/op_msg.h"
#include "mongo/s/request_types/configure_collection_balancing_gen.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>
#include <string_view>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

class ConfigCollectionBalancingCmd final : public TypedCommand<ConfigCollectionBalancingCmd> {
public:
    using Request = ConfigureCollectionBalancing;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        std::string_view kStatusField = "status"sv;

        void typedRun(OperationContext* opCtx) {
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
            const NamespaceString& nss = ns();

            ConfigsvrConfigureCollectionBalancing configsvrRequest(nss);
            configsvrRequest.setCollBalancingParams(request().getCollBalancingParams());
            configsvrRequest.setDbName(request().getDbName());

            auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
            auto cmdResponse = uassertStatusOK(
                configShard->runCommand(opCtx,
                                        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                        DatabaseName::kAdmin,
                                        configsvrRequest.toBSON(),
                                        Shard::RetryPolicy::kIdempotent));

            uassertStatusOK(cmdResponse.commandStatus);
        }

    private:
        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            ActionSet actions({ActionType::splitChunk});
            if (request().getDefragmentCollection().get_value_or(false) ||
                request().getEnableBalancing().has_value()) {
                actions.addAction(ActionType::moveChunk);
            }
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(ns()),
                                                           actions));
        }
    };

    std::string help() const override {
        return "command to check whether the chunks of a given collection are in a quiesced state "
               "or there are any which need to be moved because of (1) draining shards, (2) zone "
               "violation or (3) imbalance between shards";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
};
MONGO_REGISTER_COMMAND(ConfigCollectionBalancingCmd).forRouter();

}  // namespace
}  // namespace mongo
