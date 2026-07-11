// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/resharding/shardsvr_resharding_commands_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/commit_collection_metadata_locally.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

class ShardsvrReshardCleanupStaleChunksCommand final
    : public TypedCommand<ShardsvrReshardCleanupStaleChunksCommand> {
public:
    using Request = ShardsvrReshardCleanupStaleChunks;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();

            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            uassert(ErrorCodes::IllegalOperation,
                    "expected to be called within a retryable write",
                    TransactionParticipant::get(opCtx));

            LOGV2(12992408,
                  "Received _shardsvrReshardCleanupStaleChunks command",
                  "sourceUUID"_attr = uuid(),
                  "sourceNss"_attr = ns(),
                  "lsid"_attr = opCtx->getLogicalSessionId(),
                  "txnNum"_attr = opCtx->getTxnNumber());

            {
                auto newClient = getGlobalServiceContext()->getService()->makeClient(
                    "ShardsvrReshardCleanupStaleChunks");
                AlternativeClientRegion acr(newClient);
                auto newOpCtx = CancelableOperationContext(
                    cc().makeOperationContext(),
                    opCtx->getCancellationToken(),
                    Grid::get(opCtx->getServiceContext())->getExecutorPool()->getFixedExecutor());
                newOpCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

                shard_catalog_commit_for_resharding::commitDropOfStaleChunksForRename(
                    newOpCtx.get(), ns(), uuid());
                LOGV2(12779000,
                      "Cleanup of stale chunks finished for resharding",
                      "sourceUUID"_attr = uuid(),
                      "sourceNss"_attr = ns(),
                      "lsid"_attr = opCtx->getLogicalSessionId(),
                      "txnNum"_attr = opCtx->getTxnNumber());
            }

            // Since no write happened on this txnNumber, we need to make a dummy write so that
            // secondaries can be aware of this txn.
            DBDirectClient client(opCtx);
            client.update(NamespaceString::kServerConfigurationNamespace,
                          BSON("_id" << Request::kCommandName),
                          BSON("$inc" << BSON("count" << 1)),
                          true /* upsert */,
                          false /* multi */);
        }

    private:
        UUID uuid() const {
            return request().getOldUUID();
        }

        NamespaceString ns() const override {
            return request().getCommandParameter();
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
        return "Internal command run by coordinator against all nodes to cleanup stale chunks";
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
MONGO_REGISTER_COMMAND(ShardsvrReshardCleanupStaleChunksCommand).forShard();

}  // namespace
}  // namespace mongo
