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

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/shard_role/shard_catalog/commit_collection_metadata_locally.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class ShardsvrFetchCollMetadataCommand final
    : public TypedCommand<ShardsvrFetchCollMetadataCommand> {
public:
    using Request = ShardsvrFetchCollMetadata;

    bool skipApiVersionCheck() const override {
        return true;
    }

    std::string help() const override {
        return "Internal command. Fetches collection and chunk metadata for a specific namespace "
               "from the global catalog, persists it locally in the shard catalog, installs "
               "it authoritatively on this node's in-memory CollectionShardingRuntime, and "
               "invalidates the collection metadata on secondaries.";
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

        void typedRun(OperationContext* opCtx) {
            // Ensure shard is ready to accept sharded commands.
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();

            // Ensure interruption on step down/up.
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            // Check command write concern.
            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            // Assert that the command is running in a retryable write context.
            uassert(10303100,
                    "_shardsvrFetchCollMetadata expected to be called within a retryable write",
                    TransactionParticipant::get(opCtx));

            const auto nss = ns();

            // Assert that migrations are disabled.
            uassert(10140200,
                    "_shardsvrFetchCollMetadata can only run when migrations are disabled",
                    !sharding_ddl_util::checkAllowMigrations(opCtx, nss));

            // Use an AlternativeClientRegion to perform the shard catalog writes outside the
            // retryable write session. The shard catalog commit contains its own idempotency
            // logic, and running inside the parent session would conflict with the dummy write
            // we issue below to mark the txn on secondaries.
            {
                auto newClient = getGlobalServiceContext()->getService()->makeClient(
                    "ShardsvrFetchCollMetadata");
                AlternativeClientRegion acr(newClient);
                auto newOpCtx = CancelableOperationContext(
                    cc().makeOperationContext(),
                    opCtx->getCancellationToken(),
                    Grid::get(opCtx->getServiceContext())->getExecutorPool()->getFixedExecutor());
                newOpCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

                shard_catalog_commit::commitCollectionMetadataLocally(newOpCtx.get(), nss);
            }

            LOGV2_INFO(10140202, "Persisted metadata locally on shard", "ns"_attr = nss);

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
MONGO_REGISTER_COMMAND(ShardsvrFetchCollMetadataCommand).forShard();

}  // namespace
}  // namespace mongo
