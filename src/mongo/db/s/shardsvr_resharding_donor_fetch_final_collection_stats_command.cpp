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
#include "mongo/db/s/resharding/resharding_donor_service.h"
#include "mongo/db/s/resharding/shardsvr_resharding_commands_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/util/assert_util.h"

#include <string>

#include <boost/move/utility_core.hpp>


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {

namespace {

class ShardsvrReshardingDonorFetchFinalCollectionStatsCommand
    : public TypedCommand<ShardsvrReshardingDonorFetchFinalCollectionStatsCommand> {
public:
    using Request = ShardsvrReshardingDonorFetchFinalCollectionStats;
    using Response = ShardsvrReshardingDonorFetchFinalCollectionStatsResponse;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Internal command for querying a resharding donor shard for the change in the "
               "number of documents in the collection being resharded between the start of the "
               "cloning phase and the start of the critical section.";
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            uassert(ErrorCodes::IllegalOperation,
                    "_shardsvrReshardingDonorFetchFinalCollectionStats is only supported on "
                    "shardsvr mongod",
                    serverGlobalParams.clusterRole.has(ClusterRole::ShardServer));

            auto donorMachine = resharding::tryGetReshardingStateMachineAndThrowIfShuttingDown<
                ReshardingDonorService,
                ReshardingDonorService::DonorStateMachine,
                ReshardingDonorDocument>(opCtx, request().getReshardingUUID());

            uassert(9858407,
                    str::stream() << "No resharding operation in progress with uuid "
                                  << request().getReshardingUUID(),
                    donorMachine);

            LOGV2(9858408,
                  "Start waiting for the resharding change streams monitor to complete",
                  "reshardingUUID"_attr = request().getReshardingUUID());
            auto documentsDelta = (*donorMachine)->awaitChangeStreamsMonitorCompleted().get(opCtx);
            LOGV2(9858409,
                  "Finished waiting for the resharding change streams monitor to complete",
                  "reshardingUUID"_attr = request().getReshardingUUID(),
                  "documentsDelta"_attr = documentsDelta);
            return {documentsDelta};
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

MONGO_REGISTER_COMMAND(ShardsvrReshardingDonorFetchFinalCollectionStatsCommand)
    .requiresFeatureFlag(resharding::gFeatureFlagReshardingVerification)
    .forShard();

}  // namespace
}  // namespace mongo
