// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/resharding/resharding_donor_recipient_common.h"
#include "mongo/db/s/resharding/resharding_recipient_service.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/s/resharding/shardsvr_resharding_commands_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

class ShardsvrReshardRecipientCriticalSectionStartedCommand final
    : public TypedCommand<ShardsvrReshardRecipientCriticalSectionStartedCommand> {
public:
    using Request = ShardsvrReshardRecipientCriticalSectionStarted;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            uassert(
                ErrorCodes::IllegalOperation,
                "_shardsvrReshardRecipientCriticalSectionStarted can only be run on shard servers",
                serverGlobalParams.clusterRole.has(ClusterRole::ShardServer));

            if (auto machine = resharding::tryGetReshardingStateMachineAndThrowIfShuttingDown<
                    ReshardingRecipientService,
                    ReshardingRecipientService::RecipientStateMachine,
                    ReshardingRecipientDocument>(opCtx, uuid())) {

                LOGV2(11400401,
                      "Resharding recipient start executing "
                      "_shardsvrRecipientCriticalSectionStarted command.",
                      "reshardingUUID"_attr = uuid(),
                      "lsid"_attr = opCtx->getLogicalSessionId(),
                      "txnNum"_attr = opCtx->getTxnNumber());

                (*machine)->onCoordinatorStateAdvanced(CoordinatorStateEnum::kBlockingWrites);
                (*machine)->awaitInStrictConsistencyOrError().get(opCtx);

                LOGV2(11400402,
                      "Resharding recipient finished executing "
                      "_shardsvrRecipientCriticalSectionStarted command.",
                      "reshardingUUID"_attr = uuid(),
                      "lsid"_attr = opCtx->getLogicalSessionId(),
                      "txnNum"_attr = opCtx->getTxnNumber());
            } else {
                // If state machine does not exist, either this message was delayed and the
                // resharding operation is done, or this node is no longer a primary.
                uasserted(ErrorCodes::NotWritablePrimary,
                          "Internal Error: No matching resharding operation.");
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
        return "Internal command run by coordinator against recipients to notify them that the "
               "critical section has started. Do not call directly.";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool supportsRetryableWrite() const final {
        return true;
    }
};
MONGO_REGISTER_COMMAND(ShardsvrReshardRecipientCriticalSectionStartedCommand).forShard();

}  // namespace
}  // namespace mongo
