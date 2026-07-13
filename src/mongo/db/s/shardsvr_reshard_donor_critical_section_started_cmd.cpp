// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands.h"
#include "mongo/db/repl/intent_guard.h"
#include "mongo/db/replication_state_transition_lock_guard.h"
#include "mongo/db/s/resharding/resharding_donor_recipient_common.h"
#include "mongo/db/s/resharding/resharding_donor_service.h"
#include "mongo/db/s/resharding/shardsvr_resharding_commands_gen.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(errorAfterProcessingReshardDonorCriticalSectionStartedCommand);

class ShardsvrReshardDonorCriticalSectionStartedCommand final
    : public TypedCommand<ShardsvrReshardDonorCriticalSectionStartedCommand> {
public:
    using Request = ShardsvrReshardDonorCriticalSectionStarted;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            boost::optional<rss::consensus::WriteIntentGuard> writeGuard;
            if (gFeatureFlagIntentRegistration.isEnabled()) {
                writeGuard.emplace(opCtx);
            }

            {
                repl::ReplicationStateTransitionLockGuard rstl(opCtx, MODE_IX);
                auto const replCoord = repl::ReplicationCoordinator::get(opCtx);
                uassert(ErrorCodes::InterruptedDueToReplStateChange,
                        "Node is not primary",
                        replCoord->canAcceptWritesForDatabase(opCtx, ns().dbName()));
                opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
            }

            uassert(ErrorCodes::IllegalOperation,
                    "_shardsvrReshardDonorCriticalSectionStarted can only be run on shard servers",
                    serverGlobalParams.clusterRole.has(ClusterRole::ShardServer));

            auto machine = resharding::tryGetReshardingStateMachineAndThrowIfShuttingDown<
                ReshardingDonorService,
                ReshardingDonorService::DonorStateMachine,
                ReshardingDonorDocument>(opCtx, uuid());

            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << "No resharding donor found with id " << uuid(),
                    machine);

            LOGV2(12425400,
                  "Resharding donor received criticalSectionStarted command",
                  "reshardingUUID"_attr = uuid(),
                  "lsid"_attr = opCtx->getLogicalSessionId(),
                  "txnNum"_attr = opCtx->getTxnNumber());

            (*machine)->onCoordinatorStateAdvanced(CoordinatorStateEnum::kBlockingWrites);
            (*machine)->awaitInBlockingWritesOrError().get(opCtx);

            LOGV2(12992414,
                  "Finished executing _shardsvrReshardDonorCriticalSectionStarted command",
                  "reshardingUUID"_attr = uuid(),
                  "lsid"_attr = opCtx->getLogicalSessionId(),
                  "txnNum"_attr = opCtx->getTxnNumber());

            if (MONGO_unlikely(
                    errorAfterProcessingReshardDonorCriticalSectionStartedCommand.shouldFail())) {
                uasserted(
                    ErrorCodes::SocketException,
                    "Hit errorAfterProcessingReshardDonorCriticalSectionStartedCommand failpoint");
            }
        }

    private:
        UUID uuid() const {
            return request().getCommandParameter();
        }

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
                            ActionType::internal));
        }
    };

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command. Do not call directly. "
               "Notifies a resharding donor that the critical section has started.";
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsRetryableWrite() const final {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
};
MONGO_REGISTER_COMMAND(ShardsvrReshardDonorCriticalSectionStartedCommand).forShard();

}  // namespace
}  // namespace mongo
