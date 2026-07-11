// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/rename_collection_participant_service.h"
#include "mongo/db/global_catalog/ddl/sharded_rename_collection_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/forwardable_operation_metadata.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/commit_collection_metadata_locally.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

class ShardsvrRenameCollectionParticipantCommand final
    : public TypedCommand<ShardsvrRenameCollectionParticipantCommand> {
public:
    using Request = ShardsvrRenameCollectionParticipant;

    bool adminOnly() const override {
        return false;
    }

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command. Do not call directly. Locally renames a collection.";
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
            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            const auto txnParticipant = TransactionParticipant::get(opCtx);
            uassert(6077302,
                    str::stream() << Request::kCommandName << " must be run as a retryable write",
                    txnParticipant);

            auto const shardingState = ShardingState::get(opCtx);
            shardingState->assertCanAcceptShardedCommands();
            auto const& req = request();

            const NamespaceString& fromNss = ns();
            RenameCollectionParticipantDocument participantDoc(
                fromNss, ForwardableOperationMetadata(opCtx), req.getSourceUUID());
            participantDoc.setTargetUUID(req.getTargetUUID());
            participantDoc.setNewTargetCollectionUuid(req.getNewTargetCollectionUuid());
            participantDoc.setFromMigrate(req.getFromMigrate());
            participantDoc.setClearCollMetadata(req.getClearCollMetadata());
            participantDoc.setRenameCollectionRequest(req.getRenameCollectionRequest());

            const auto service = RenameCollectionParticipantService::getService(opCtx);
            const auto participantDocBSON = participantDoc.toBSON();
            const auto renameCollectionParticipant =
                RenameParticipantInstance::getOrCreate(opCtx, service, participantDocBSON);
            bool hasSameOptions = renameCollectionParticipant->hasSameOptions(participantDocBSON);
            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << "Another rename participant for namespace "
                                  << fromNss.toStringForErrorMsg()
                                  << "is instantiated with different parameters: `"
                                  << renameCollectionParticipant->doc() << "` vs `"
                                  << participantDocBSON << "`",
                    hasSameOptions);

            renameCollectionParticipant->getBlockCRUDAndRenameCompletionFuture().get(opCtx);

            // Since no write that generated a retryable write oplog entry with this sessionId and
            // txnNumber happened, we need to make a dummy write so that the session gets durably
            // persisted on the oplog. This must be the last operation done on this command.
            DBDirectClient client(opCtx);
            client.update(NamespaceString::kServerConfigurationNamespace,
                          BSON("_id" << Request::kCommandName),
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
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::internal));
        }
    };
};
MONGO_REGISTER_COMMAND(ShardsvrRenameCollectionParticipantCommand).forShard();

class ShardsvrRenameCollectionUnblockParticipantCommand final
    : public TypedCommand<ShardsvrRenameCollectionUnblockParticipantCommand> {
public:
    using Request = ShardsvrRenameCollectionUnblockParticipant;

    bool adminOnly() const override {
        return false;
    }

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command. Do not call directly. Releases the critical section of source "
               "and target collections.";
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
            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            const auto txnParticipant = TransactionParticipant::get(opCtx);
            uassert(6077303,
                    str::stream() << Request::kCommandName << " must be run as a retryable write",
                    txnParticipant);

            auto const shardingState = ShardingState::get(opCtx);
            shardingState->assertCanAcceptShardedCommands();

            const NamespaceString& fromNss = ns();
            const auto& req = request();

            const auto service = RenameCollectionParticipantService::getService(opCtx);
            const auto id = BSON("_id" << NamespaceStringUtil::serialize(
                                     fromNss, SerializationContext::stateDefault()));
            const auto [optRenameCollectionParticipant, _] =
                RenameParticipantInstance::lookup(opCtx, service, id);
            if (optRenameCollectionParticipant) {

                const auto targetUUID = optRenameCollectionParticipant.value()->getTargetUUID();

                auto optUnblockCrudFuture =
                    optRenameCollectionParticipant.value()->getUnblockCrudFutureFor(
                        req.getSourceUUID());
                uassert(ErrorCodes::CommandFailed,
                        "Provided UUID does not match",
                        optUnblockCrudFuture.has_value());
                optUnblockCrudFuture->get(opCtx);

                if (targetUUID) {
                    LOGV2_INFO(12295705,
                               "Cleaning up stale chunk information for replaced collection",
                               "uuid"_attr = *targetUUID);

                    // Remove the old chunks now since they are now garbage to be cleaned up.
                    // This is safe to do outside of the critical section because no other operation
                    // can access the data as the UUID in the chunks point to a non-existent
                    // collection.
                    auto newClient = getGlobalServiceContext()->getService()->makeClient(
                        "ShardsvrRenameCollectionUnblockParticipantCommand");
                    AlternativeClientRegion acr(newClient);
                    auto newOpCtx = CancelableOperationContext(cc().makeOperationContext(),
                                                               opCtx->getCancellationToken(),
                                                               Grid::get(opCtx->getServiceContext())
                                                                   ->getExecutorPool()
                                                                   ->getFixedExecutor());
                    newOpCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

                    shard_catalog_commit::commitDropOfStaleChunksForRename(newOpCtx.get(),
                                                                           *targetUUID);
                }
            }

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
MONGO_REGISTER_COMMAND(ShardsvrRenameCollectionUnblockParticipantCommand).forShard();

}  // namespace
}  // namespace mongo
