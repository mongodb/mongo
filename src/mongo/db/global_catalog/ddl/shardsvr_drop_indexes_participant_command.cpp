/**
 *    Copyright (C) 2025-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/router_role_api/cluster_commands_helpers.h"
#include "mongo/db/local_catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/local_catalog/drop_indexes.h"
#include "mongo/db/s/forwardable_operation_metadata.h"
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
                auto alternativeClient = opCtx->getServiceContext()
                                             ->getService(ClusterRole::ShardServer)
                                             ->makeClient("DropIndexesParticipant");
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
