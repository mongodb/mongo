// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/set_allow_migrations_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/ddl/sharding_util.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/transaction/transaction_participant_resource_yielder.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

void tellShardsToRefresh(OperationContext* opCtx,
                         const NamespaceString& nss,
                         const std::string& cmdName) {
    // If we have a session checked out, we need to yield it, considering we'll be doing a network
    // operation that may block.
    std::unique_ptr<TransactionParticipantResourceYielder> resourceYielder;
    if (TransactionParticipant::get(opCtx)) {
        resourceYielder = TransactionParticipantResourceYielder::make(cmdName);
        resourceYielder->yield(opCtx);
    }

    // Trigger a refresh on every shard. We send this to every shard and not just shards that own
    // chunks for the collection because the set of shards owning chunks is updated before the
    // critical section is released during chunk migrations. If the last chunk is moved off of a
    // shard and this flush is not sent to that donor, stopMigrations will not wait for the critical
    // section to finish on that shard (SERVER-73984).
    const auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    const auto allShardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
    sharding_util::tellShardsToRefreshCollection(opCtx, allShardIds, nss, executor);
    if (resourceYielder) {
        resourceYielder->unyield(opCtx);
    }
}

class ConfigsvrSetAllowMigrationsCommand final
    : public TypedCommand<ConfigsvrSetAllowMigrationsCommand> {
public:
    using Request = ConfigsvrSetAllowMigrations;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            const NamespaceString& nss = ns();

            uassert(ErrorCodes::IllegalOperation,
                    "_configsvrSetAllowMigrations can only be run on config servers",
                    serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));
            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            {
                // Use ACR to have a thread holding the session while we do the metadata updates so
                // we can serialize concurrent requests to setAllowMigrations (i.e. a stepdown
                // happens and the new primary sends a setAllowMigrations with the same sessionId).
                // We could think about weakening the serialization guarantee in the future because
                // the replay protection comes from the oplog write with a specific txnNumber. Using
                // ACR also prevents having deadlocks with the shutdown thread because the
                // cancellation of the new operation context is linked to the parent one.
                auto newClient =
                    opCtx->getServiceContext()->getService()->makeClient("SetAllowMigrations");
                AlternativeClientRegion acr(newClient);
                auto executor =
                    Grid::get(opCtx->getServiceContext())->getExecutorPool()->getFixedExecutor();
                auto newOpCtxPtr = CancelableOperationContext(
                    cc().makeOperationContext(), opCtx->getCancellationToken(), executor);

                AuthorizationSession::get(newOpCtxPtr.get()->getClient())
                    ->grantInternalAuthorization();
                newOpCtxPtr->setWriteConcern(opCtx->getWriteConcern());

                // Set the operation context read concern level to local for reads into the config
                // database.
                repl::ReadConcernArgs::get(newOpCtxPtr.get()) =
                    repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

                const auto allowMigrations = request().getAllowMigrations();
                const auto& collectionUUID = request().getCollectionUUID();

                ShardingCatalogManager::get(newOpCtxPtr.get())
                    ->setAllowMigrationsAndBumpOneChunk(
                        newOpCtxPtr.get(), nss, collectionUUID, allowMigrations);
            }

            tellShardsToRefresh(
                opCtx, ns(), std::string{ConfigsvrSetAllowMigrations::kCommandName});

            // Since we no write happened on this txnNumber, we need to make a dummy write to
            // protect against older requests with old txnNumbers.
            DBDirectClient client(opCtx);
            client.update(NamespaceString::kServerConfigurationNamespace,
                          BSON("_id" << "SetAllowMigrationsStats"),
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

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command, which is exported by the sharding config server. Do not call "
               "directly. Sets the allowMigrations flag on the specified collection.";
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
MONGO_REGISTER_COMMAND(ConfigsvrSetAllowMigrationsCommand).forShard();

}  // namespace
}  // namespace mongo
