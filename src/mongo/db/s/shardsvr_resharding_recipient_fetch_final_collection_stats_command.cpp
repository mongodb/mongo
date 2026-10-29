// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/resharding/resharding_donor_recipient_common.h"
#include "mongo/db/s/resharding/resharding_recipient_service.h"
#include "mongo/db/s/resharding/shardsvr_resharding_commands_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/util/assert_util.h"

#include <string>

#include <boost/move/utility_core.hpp>


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {

namespace {

class ShardsvrReshardingRecipientFetchFinalCollectionStatsCommand
    : public TypedCommand<ShardsvrReshardingRecipientFetchFinalCollectionStatsCommand> {
public:
    using Request = ShardsvrReshardingRecipientFetchFinalCollectionStats;
    using Response = ShardsvrReshardingRecipientFetchFinalCollectionStatsResponse;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Internal command for querying a resharding recipient shard for the change in the "
               "number of documents in the collection being resharded during the applying phase.";
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            uassert(ErrorCodes::IllegalOperation,
                    "_shardsvrReshardingRecipientFetchFinalCollectionStats is only supported on "
                    "shardsvr mongod",
                    serverGlobalParams.clusterRole.has(ClusterRole::ShardServer));

            auto recipientMachine = resharding::tryGetReshardingStateMachineAndThrowIfShuttingDown<
                ReshardingRecipientService,
                ReshardingRecipientService::RecipientStateMachine,
                ReshardingRecipientDocument>(opCtx, request().getReshardingUUID());

            uassert(12723301,
                    str::stream() << "No resharding operation in progress with uuid "
                                  << request().getReshardingUUID(),
                    recipientMachine);

            LOGV2(12723401,
                  "Start waiting for the resharding change streams monitor to complete on "
                  "recipient",
                  "reshardingUUID"_attr = request().getReshardingUUID());
            auto documentsDelta =
                (*recipientMachine)->awaitChangeStreamsMonitorCompleted().get(opCtx);
            LOGV2(12723402,
                  "Finished waiting for the resharding change streams monitor to complete on "
                  "recipient",
                  "reshardingUUID"_attr = request().getReshardingUUID(),
                  "documentsDelta"_attr = documentsDelta);
            return Response{documentsDelta};
        }

    private:
        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::internal));
        }
    };
};

MONGO_REGISTER_COMMAND(ShardsvrReshardingRecipientFetchFinalCollectionStatsCommand)
    .requiresFeatureFlag(resharding::gFeatureFlagReshardingVerification)
    .forShard();

}  // namespace
}  // namespace mongo
