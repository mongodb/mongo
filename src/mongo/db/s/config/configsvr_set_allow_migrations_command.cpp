/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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


#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/sharding_util.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/transaction/transaction_participant_resource_yielder.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/set_allow_migrations_gen.h"
#include "mongo/util/assert_util.h"

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
                auto newClient = opCtx->getServiceContext()->makeClient("SetAllowMigrations");
                AlternativeClientRegion acr(newClient);
                auto executor =
                    Grid::get(opCtx->getServiceContext())->getExecutorPool()->getFixedExecutor();
                auto newOpCtxPtr = CancelableOperationContext(
                    cc().makeOperationContext(), opCtx->getCancellationToken(), executor);

                AuthorizationSession::get(newOpCtxPtr.get()->getClient())
                    ->grantInternalAuthorization(newOpCtxPtr.get()->getClient());
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

            tellShardsToRefresh(opCtx, ns(), ConfigsvrSetAllowMigrations::kCommandName.toString());

            // Since we no write happened on this txnNumber, we need to make a dummy write to
            // protect against older requests with old txnNumbers.
            DBDirectClient client(opCtx);
            client.update(NamespaceString::kServerConfigurationNamespace,
                          BSON("_id"
                               << "SetAllowMigrationsStats"),
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
