// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/s/forwardable_operation_metadata.h"
#include "mongo/db/shard_role/shard_catalog/drop_indexes.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/timeseries/timeseries_request_util.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/transaction/transaction_participant.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class ShardsvrDropIndexesParticipantCommand final
    : public TypedCommand<ShardsvrDropIndexesParticipantCommand> {
public:
    using Request = ShardsvrDropIndexesParticipant;
    using Response = DropIndexesReply;

    std::string help() const override {
        return "Internal command for dropping indexes on participant shards. Do not call "
               "directly.";
    }

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    bool supportsRetryableWrite() const final {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();

            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            const auto& req = request();

            bool isDryRun = req.getDryRun().value_or(false);

            if (isDryRun) {
                return dropIndexesDryRun(
                    opCtx,
                    ns(),
                    req.getCollectionUUID(),
                    req.getIndex(),
                    req.getShardKeyPattern(),
                    timeseries::isRawDataRequest(opCtx, req) /* forceRawDataMode*/);
            }

            auto txnParticipant = TransactionParticipant::get(opCtx);

            uassert(ErrorCodes::InvalidOptions,
                    "dropIndexesParticipant must be called with a transaction number",
                    txnParticipant);

            BSONObj response;
            bool commandSucceeded = false;

            {
                // Use an AlternativeClientRegion because the dropIndexes command cannot run inside
                // a transaction.
                // To ensure this operation is properly recorded in the oplog, we will perform a
                // dummy write later using the original opCtx.
                auto alternativeClient =
                    opCtx->getServiceContext()->getService()->makeClient("DropIndexesParticipant");
                AlternativeClientRegion acr(alternativeClient);

                auto alternativeOpCtx = CancelableOperationContext(
                    cc().makeOperationContext(),
                    opCtx->getCancellationToken(),
                    Grid::get(opCtx->getServiceContext())->getExecutorPool()->getFixedExecutor());
                alternativeOpCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

                ForwardableOperationMetadata forwardableOpMetadata(opCtx);
                forwardableOpMetadata.setOn(alternativeOpCtx.get());

                OperationShardingState::setShardRole(
                    alternativeOpCtx.get(),
                    ns(),
                    OperationShardingState::get(opCtx).getShardVersion(ns()),
                    OperationShardingState::get(opCtx).getDbVersion(ns().dbName()));

                DBDirectClient client(alternativeOpCtx.get());

                DropIndexes dropIndexesCmd(ns());
                dropIndexesCmd.setDropIndexesRequest(req.getDropIndexesRequest());
                setReadWriteConcern(opCtx, dropIndexesCmd, this);

                commandSucceeded =
                    client.runCommand(ns().dbName(), dropIndexesCmd.toBSON(), response);
            }

            // Since no write that generated a retryable write oplog entry with this
            // sessionId and txnNumber happened, we need to make a dummy write so that the
            // session gets durably persisted on the oplog. This must be the last operation
            // done on this command.
            if (commandSucceeded) {
                DBDirectClient dbClient(opCtx);
                dbClient.update(NamespaceString::kServerConfigurationNamespace,
                                BSON("_id" << Request::kCommandName),
                                BSON("$inc" << BSON("count" << 1)),
                                true /* upsert */,
                                false /* multi */);

                LOGV2(10666601,
                      "Successfully dropped indexes on participant shard",
                      "dropIndexesRequest"_attr = req.getDropIndexesRequest().toBSON());
            }

            uassertStatusOK(getStatusFromCommandResult(response));

            Response dropIndexesResponse =
                DropIndexesReply::parse(response, IDLParserContext("DropIndexesReply"));

            return dropIndexesResponse;
        }

    private:
        NamespaceString ns() const override {
            return request().getNamespace();
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
};
MONGO_REGISTER_COMMAND(ShardsvrDropIndexesParticipantCommand).forShard();

}  // namespace
}  // namespace mongo
