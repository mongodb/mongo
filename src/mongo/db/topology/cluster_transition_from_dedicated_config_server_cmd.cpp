// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/topology/transition_from_dedicated_config_server_gen.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

const ReadPreferenceSetting kPrimaryOnlyReadPreference{ReadPreference::PrimaryOnly};

class TransitionFromDedicatedConfigServerCommand
    : public TypedCommand<TransitionFromDedicatedConfigServerCommand> {
public:
    using Request = TransitionFromDedicatedConfigServer;

    std::string help() const override {
        return "transition from dedicated config server to config shard";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            // (Ignore FCV check): TODO(SERVER-75389): add why FCV is ignored here.
            uassert(8454804,
                    "The transition to config shard feature is disabled",
                    gFeatureFlagTransitionToCatalogShard.isEnabledAndIgnoreFCVUnsafe());

            ConfigsvrTransitionFromDedicatedConfigServer cmdToSend;
            cmdToSend.setDbName(DatabaseName::kAdmin);
            generic_argument_util::setMajorityWriteConcern(cmdToSend, &opCtx->getWriteConcern());

            auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

            // Force a reload of this node's shard list cache at the end of this command.
            auto cmdResponseWithStatus = configShard->runCommand(opCtx,
                                                                 kPrimaryOnlyReadPreference,
                                                                 DatabaseName::kAdmin,
                                                                 cmdToSend.toBSON(),
                                                                 Shard::RetryPolicy::kIdempotent);

            Grid::get(opCtx)->shardRegistry()->reload(opCtx);

            uassertStatusOK(cmdResponseWithStatus);
            uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(cmdResponseWithStatus));
        }

    private:
        NamespaceString ns() const override {
            return {};
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::transitionFromDedicatedConfigServer));
        }
    };
};
MONGO_REGISTER_COMMAND(TransitionFromDedicatedConfigServerCommand).forRouter();

}  // namespace
}  // namespace mongo
