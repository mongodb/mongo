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
#include "mongo/logv2/log.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/util/assert_util.h"

#include <string>

#include <boost/move/utility_core.hpp>


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {

namespace {

class ShardsvrReshardingDonorStartChangeStreamsMonitorCommand
    : public TypedCommand<ShardsvrReshardingDonorStartChangeStreamsMonitorCommand> {
public:
    using Request = ShardsvrReshardingDonorStartChangeStreamsMonitor;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool supportsRetryableWrite() const final {
        return true;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Internal command for making a resharding donor shard start monitoring the changes "
               "in the collection being resharded between the start of the cloning phase and the "
               "start of the critical section";
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            uassert(ErrorCodes::IllegalOperation,
                    "_shardsvrReshardingDonorStartChangStreamsMonitor is only supported on "
                    "shardsvr mongod",
                    serverGlobalParams.clusterRole.has(ClusterRole::ShardServer));

            LOGV2(12992420,
                  "Received _shardsvrReshardingDonorStartChangeStreamsMonitor command",
                  "reshardingUUID"_attr = request().getReshardingUUID(),
                  "lsid"_attr = opCtx->getLogicalSessionId(),
                  "txnNum"_attr = opCtx->getTxnNumber());

            auto donorMachine = resharding::tryGetReshardingStateMachineAndThrowIfShuttingDown<
                ReshardingDonorService,
                ReshardingDonorService::DonorStateMachine,
                ReshardingDonorDocument>(opCtx, request().getReshardingUUID());

            uassert(9858406,
                    str::stream() << "No resharding operation in progress with uuid "
                                  << request().getReshardingUUID(),
                    donorMachine);

            (*donorMachine)
                ->createAndStartChangeStreamsMonitor(request().getCloneTimestamp())
                .get(opCtx);

            LOGV2(12992421,
                  "Finished executing _shardsvrReshardingDonorStartChangeStreamsMonitor command",
                  "reshardingUUID"_attr = request().getReshardingUUID(),
                  "lsid"_attr = opCtx->getLogicalSessionId(),
                  "txnNum"_attr = opCtx->getTxnNumber());
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

MONGO_REGISTER_COMMAND(ShardsvrReshardingDonorStartChangeStreamsMonitorCommand)
    .requiresFeatureFlag(resharding::gFeatureFlagReshardingVerification)
    .forShard();

}  // namespace
}  // namespace mongo
