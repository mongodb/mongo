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
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/sharded_index_catalog_commands_gen.h"
#include "mongo/db/s/sharding_index_catalog_ddl_util.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/s/sharding_feature_flags_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

enum class RenameIndexCatalogOperationEnum : std::int32_t {
    kRename,
    kClearTo,
    kNoop,
};

class ShardsvrRenameIndexMetadataCommand final
    : public TypedCommand<ShardsvrRenameIndexMetadataCommand> {
public:
    using Request = ShardsvrRenameIndexMetadata;

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command. Do not call directly. Sets a globlal index version for the "
               "shard-role catalog.";
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
            uassert(ErrorCodes::IllegalOperation,
                    "This command can only be executed in steady state shards.",
                    ShardingState::get(opCtx)->canAcceptShardedCommands() == Status::OK());

            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            const auto txnParticipant = TransactionParticipant::get(opCtx);
            uassert(7079501,
                    format(FMT_STRING("{} must be run as a retryable write"),
                           Request::kCommandName.toString()),
                    txnParticipant);

            RenameIndexCatalogOperationEnum renameOp = RenameIndexCatalogOperationEnum::kNoop;
            {
                AutoGetCollection coll(opCtx, ns(), LockMode::MODE_IS);
                auto scopedCsr =
                    CollectionShardingRuntime::assertCollectionLockedAndAcquireShared(opCtx, ns());
                uassert(7079502,
                        format(FMT_STRING("The critical section for collection {} must be taken in "
                                          "order to execute this command"),
                               ns().toStringForErrorMsg()),
                        scopedCsr->getCriticalSectionSignal(
                            opCtx, ShardingMigrationCriticalSection::kWrite));
                if (scopedCsr->getIndexesInCritSec(opCtx)) {
                    renameOp = RenameIndexCatalogOperationEnum::kRename;
                }
            }

            boost::optional<UUID> toUuid;
            {
                AutoGetCollection coll(opCtx, request().getToNss(), LockMode::MODE_IS);
                auto scopedToCsr =
                    CollectionShardingRuntime::assertCollectionLockedAndAcquireShared(
                        opCtx, request().getToNss());
                uassert(7079503,
                        format(FMT_STRING("The critical section for collection {} must be taken in "
                                          "order to execute this command"),
                               ns().toStringForErrorMsg()),
                        scopedToCsr->getCriticalSectionSignal(
                            opCtx, ShardingMigrationCriticalSection::kWrite));
                const auto& indexMetadata = scopedToCsr->getIndexesInCritSec(opCtx);
                if (indexMetadata &&
                    indexMetadata->getCollectionIndexes().uuid() ==
                        request().getIndexVersion().uuid()) {
                    // Rename operation already executed.
                    renameOp = RenameIndexCatalogOperationEnum::kNoop;
                } else if (indexMetadata && renameOp == RenameIndexCatalogOperationEnum::kNoop) {
                    toUuid.emplace(indexMetadata->getCollectionIndexes().uuid());
                    renameOp = RenameIndexCatalogOperationEnum::kClearTo;
                }
            }

            switch (renameOp) {
                case RenameIndexCatalogOperationEnum::kRename:
                    renameCollectionShardingIndexCatalog(
                        opCtx,
                        ns(),
                        request().getToNss(),
                        request().getIndexVersion().indexVersion());
                    break;
                case RenameIndexCatalogOperationEnum::kClearTo:
                    clearCollectionShardingIndexCatalog(
                        opCtx, request().getToNss(), toUuid.value());
                    break;
                case RenameIndexCatalogOperationEnum::kNoop: {
                    // Since no write happened on this txnNumber, we need to make a dummy write
                    // so that secondaries can be aware of this txn.
                    DBDirectClient client(opCtx);
                    client.update(NamespaceString::kServerConfigurationNamespace,
                                  BSON("_id"
                                       << "RenameCollectionMetadataStats"),
                                  BSON("$inc" << BSON("count" << 1)),
                                  true /* upsert */,
                                  false /* multi */);
                    break;
                }
                default:
                    MONGO_UNREACHABLE;
            }
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

} shardsvrRenameIndexMetadataCommand;

}  // namespace
}  // namespace mongo
