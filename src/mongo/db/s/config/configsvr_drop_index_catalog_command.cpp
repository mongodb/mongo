/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/s/sharded_index_catalog_commands_gen.h"
#include "mongo/db/s/sharding_index_catalog_util.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/transaction/transaction_participant_resource_yielder.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/s/sharding_feature_flags_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

/**
 * Drops an index from the local catalog and bumps the indexVersion in the collections collection
 * transactionally.
 */
void dropIndexInTransaction(OperationContext* opCtx,
                            std::shared_ptr<executor::TaskExecutor> executor,
                            const NamespaceString& userCollectionNss,
                            const std::string& name,
                            const UUID& collectionUUID,
                            const Timestamp& lastmod) {
    // TODO SERVER-75189: remove the usage of shared_ptr once the executor is inlined, so the
    // variable will never be out of scope.
    auto deleteOp = std::make_shared<write_ops::DeleteCommandRequest>(
        NamespaceString::kConfigsvrIndexCatalogNamespace);
    deleteOp->setDeletes({[&] {
        write_ops::DeleteOpEntry entry;
        entry.setQ(BSON(IndexCatalogType::kCollectionUUIDFieldName
                        << collectionUUID << IndexCatalogType::kNameFieldName << name));
        entry.setMulti(false);
        return entry;
    }()});

    auto updateCollectionOp =
        std::make_shared<write_ops::UpdateCommandRequest>(CollectionType::ConfigNS);
    updateCollectionOp->setUpdates({[&] {
        write_ops::UpdateOpEntry entry;
        entry.setQ(BSON(CollectionType::kNssFieldName << userCollectionNss.ns()
                                                      << CollectionType::kUuidFieldName
                                                      << collectionUUID));
        entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(
            BSON("$set" << BSON(CollectionType::kUuidFieldName
                                << collectionUUID << CollectionType::kIndexVersionFieldName
                                << lastmod))));
        entry.setUpsert(true);
        entry.setMulti(false);
        return entry;
    }()});

    auto inlineExecutor = std::make_shared<executor::InlineExecutor>();
    auto sleepInlineExecutor = inlineExecutor->getSleepableExecutor(executor);

    txn_api::SyncTransactionWithRetries txn(
        opCtx,
        sleepInlineExecutor,
        TransactionParticipantResourceYielder::make("dropIndexCatalogEntry"),
        inlineExecutor);

    txn.run(opCtx,
            [updateCollectionOp, deleteOp](const txn_api::TransactionClient& txnClient,
                                           ExecutorPtr txnExec) {
                return txnClient.runCRUDOp(*deleteOp, {0})
                    .thenRunOn(txnExec)
                    .then([&txnClient, updateCollectionOp](auto deleteResponse) {
                        uassertStatusOK(deleteResponse.toStatus());
                        return txnClient.runCRUDOp(*updateCollectionOp, {1});
                    })
                    .thenRunOn(txnExec)
                    .then([](auto updateResponse) { uassertStatusOK(updateResponse.toStatus()); })
                    .semi();
            });
}

class ConfigsvrDropIndexCatalogEntryCommand final
    : public TypedCommand<ConfigsvrDropIndexCatalogEntryCommand> {
public:
    using Request = ConfigsvrDropIndexCatalogEntry;

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command. Do not call directly. Drops an index entry in the sharding "
               "catalog.";
    }

    bool adminOnly() const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool supportsRetryableWrite() const final {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::CommandNotSupported,
                    format(FMT_STRING("{} command not enabled"), definition()->getName()),
                    feature_flags::gGlobalIndexesShardingCatalog.isEnabled(
                        serverGlobalParams.featureCompatibility));
            uassert(
                ErrorCodes::IllegalOperation,
                format(FMT_STRING("{} can only be run on config servers"), definition()->getName()),
                serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));

            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            const auto txnParticipant = TransactionParticipant::get(opCtx);
            uassert(6711909,
                    str::stream() << Request::kCommandName << " must be run as a retryable write",
                    txnParticipant);

            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            dropIndexInTransaction(opCtx,
                                   Grid::get(opCtx)->getExecutorPool()->getFixedExecutor(),
                                   ns(),
                                   request().getName().toString(),
                                   request().getCollectionUUID(),
                                   request().getLastmod());

            // Since no write that generated a retryable write oplog entry with this sessionId
            // and txnNumber happened, we need to make a dummy write so that the session gets
            // durably persisted on the oplog. This must be the last operation done on this
            // command.
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
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }
    };

} configsvrDropIndexCatalogEntryCommand;

}  // namespace
}  // namespace mongo
