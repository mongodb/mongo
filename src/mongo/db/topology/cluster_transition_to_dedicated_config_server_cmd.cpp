// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/topology/transition_to_dedicated_config_server_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

const ReadPreferenceSetting kPrimaryOnlyReadPreference{ReadPreference::PrimaryOnly};

class TransitionToDedicatedConfigServerCmd
    : public TypedCommand<TransitionToDedicatedConfigServerCmd> {
public:
    using Request = TransitionToDedicatedConfigServer;
    using Response = RemoveShardResponse;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            // (Ignore FCV check): TODO(SERVER-75389): add why FCV is ignored here.
            uassert(7368401,
                    "The transition to config shard feature is disabled",
                    gFeatureFlagTransitionToCatalogShard.isEnabledAndIgnoreFCVUnsafe());

            auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();


            ConfigsvrTransitionToDedicatedConfig transitionToDedicatedConfigServer;
            transitionToDedicatedConfigServer.setGenericArguments(
                CommandInvocation::get(opCtx)->getGenericArguments());
            transitionToDedicatedConfigServer.setDbName(DatabaseName::kAdmin);
            generic_argument_util::setMajorityWriteConcern(transitionToDedicatedConfigServer,
                                                           &opCtx->getWriteConcern());


            // Force a reload of this node's shard list cache at the end of this command.
            auto cmdResponseWithStatus =
                configShard->runCommand(opCtx,
                                        kPrimaryOnlyReadPreference,
                                        DatabaseName::kAdmin,
                                        CommandHelpers::filterCommandRequestForPassthrough(
                                            transitionToDedicatedConfigServer.toBSON()),
                                        Shard::RetryPolicy::kIdempotent);

            Grid::get(opCtx)->shardRegistry()->reload(opCtx);

            uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(cmdResponseWithStatus));

            BSONObjBuilder filteredResponse;
            CommandHelpers::filterCommandReplyForPassthrough(
                cmdResponseWithStatus.getValue().response, &filteredResponse);

            return Response::parse(filteredResponse.obj(),
                                   IDLParserContext("TransitionToDedicatedConfigServerCmd"));
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
                            ActionType::transitionToDedicatedConfigServer));
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Transition to dedicated config server.";
    }
};
MONGO_REGISTER_COMMAND(TransitionToDedicatedConfigServerCmd).forRouter();

}  // namespace
}  // namespace mongo
