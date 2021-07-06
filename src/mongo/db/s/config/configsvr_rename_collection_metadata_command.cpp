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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"


namespace mongo {
namespace {

class ConfigsvrRenameCollectionMetadataCommand final
    : public TypedCommand<ConfigsvrRenameCollectionMetadataCommand> {
public:
    using Request = ConfigsvrRenameCollectionMetadata;

    std::string help() const override {
        return "Internal command. Do not call directly. Renames a collection.";
    }

    bool adminOnly() const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::IllegalOperation,
                    "_configsvrRenameCollectionMetadata can only be run on config servers",
                    serverGlobalParams.clusterRole == ClusterRole::ConfigServer);
            uassert(ErrorCodes::InvalidOptions,
                    "_configsvrRenameCollectionMetadata must be called with majority writeConcern",
                    opCtx->getWriteConcern().wMode == WriteConcernOptions::kMajority);

            opCtx->setAlwaysInterruptAtStepDownOrUp();

            const auto& req = request();

            // Set the operation context read concern level to local for reads into the config
            // database.
            repl::ReadConcernArgs::get(opCtx) =
                repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

            auto txnParticipant = TransactionParticipant::get(opCtx);
            if (!txnParticipant) {
                // old binaries will not send the txnNumber
                ShardingCatalogManager::get(opCtx)->renameShardedMetadata(
                    opCtx,
                    ns(),
                    req.getTo(),
                    ShardingCatalogClient::kMajorityWriteConcern,
                    req.getOptFromCollection());
                return;
            }

            {
                auto newClient = opCtx->getServiceContext()->makeClient("RenameCollectionMetadata");
                {
                    stdx::lock_guard<Client> lk(*newClient.get());
                    newClient->setSystemOperationKillableByStepdown(lk);
                }

                AlternativeClientRegion acr(newClient);
                auto executor =
                    Grid::get(opCtx->getServiceContext())->getExecutorPool()->getFixedExecutor();
                CancelableOperationContext newOpCtxPtr(
                    cc().makeOperationContext(), opCtx->getCancellationToken(), executor);

                ShardingCatalogManager::get(newOpCtxPtr.get())
                    ->renameShardedMetadata(newOpCtxPtr.get(),
                                            ns(),
                                            req.getTo(),
                                            ShardingCatalogClient::kLocalWriteConcern,
                                            req.getOptFromCollection());
            }

            // Since we no write happened on this txnNumber, we need to make a dummy write so that
            // secondaries can be aware of this txn.
            DBDirectClient client(opCtx);
            client.update(NamespaceString::kServerConfigurationNamespace.ns(),
                          BSON("_id"
                               << "RenameCollectionMetadataStats"),
                          BSON("$inc" << BSON("count" << 1)),
                          true /* upsert */,
                          false /* multi */);
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
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }
    };

} _configsvrRenameCollectionMetadata;

}  // namespace
}  // namespace mongo
