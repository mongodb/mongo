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

#include <fmt/format.h>

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/s/shard_local_catalog_operations.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/s/catalog/type_database_gen.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/s/sharding_state.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {
using namespace fmt::literals;

class ShardsvrCommitToShardLocalCatalogCommand final
    : public TypedCommand<ShardsvrCommitToShardLocalCatalogCommand> {
public:
    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    std::string help() const override {
        return "Internal command. This command aims to commit metadata to the shard-local catalog.";
    }

    bool supportsRetryableWrite() const final {
        return true;
    }

    using Request = ShardsvrCommitToShardLocalCatalog;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();

            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            uassert(
                ErrorCodes::InvalidOptions,
                "{} expected to be called within a retryable write "_format(Request::kCommandName),
                TransactionParticipant::get(opCtx));

            {
                // Using the original operation context, the write operations to update the
                // shard-local catalog would fail since retryable writes are not compatible with
                // applying the WriteUnitOfWork as a transaction (kGroupForTransaction). A tactical
                // solution is to use an alternative client as well as a new operation context.

                auto newClient = getGlobalServiceContext()
                                     ->getService(ClusterRole::ShardServer)
                                     ->makeClient("ShardsvrCommitToShardLocalCatalog");
                AlternativeClientRegion acr(newClient);
                auto newOpCtx = CancelableOperationContext(
                    cc().makeOperationContext(),
                    opCtx->getCancellationToken(),
                    Grid::get(opCtx->getServiceContext())->getExecutorPool()->getFixedExecutor());
                newOpCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

                switch (request().getOperation()) {
                    case CommitToShardLocalCatalogOpEnum::kInsertDatabaseMetadata: {
                        tassert(1003890,
                                "Expecting database metadata in the command",
                                request().getDbMetadata());

                        const auto db = *request().getDbMetadata();

                        tassert(1003891,
                                "Expecting to be in the shard from the primary shard of the "
                                "database metadata",
                                db.getPrimary() == ShardingState::get(newOpCtx.get())->shardId());

                        shard_local_catalog_operations::insertDatabaseMetadata(newOpCtx.get(), db);
                        break;
                    }
                    case CommitToShardLocalCatalogOpEnum::kRemoveDatabaseMetadata: {
                        shard_local_catalog_operations::removeDatabaseMetadata(
                            newOpCtx.get(), request().getDbName());
                        break;
                    }
                    default:
                        MONGO_UNREACHABLE;
                }
            }

            // Since no write that generated a retryable write oplog entry with this sessionId and
            // txnNumber happened, we need to make a dummy write so that the session gets durably
            // persisted on the oplog. This must be the last operation done on this command.
            DBDirectClient dbClient(opCtx);
            dbClient.update(NamespaceString::kServerConfigurationNamespace,
                            BSON("_id" << Request::kCommandName),
                            BSON("$inc" << BSON("count" << 1)),
                            true /* upsert */,
                            false /* multi */);
        }

    private:
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
};

MONGO_REGISTER_COMMAND(ShardsvrCommitToShardLocalCatalogCommand).forShard();

}  // namespace
}  // namespace mongo
