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


#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/s/rename_collection_participant_service.h"
#include "mongo/db/s/sharded_rename_collection_gen.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/write_concern.h"
#include "mongo/logv2/log.h"

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
            uassertStatusOK(shardingState->canAcceptShardedCommands());
            auto const& req = request();

            const NamespaceString& fromNss = ns();
            RenameCollectionParticipantDocument participantDoc(
                fromNss, ForwardableOperationMetadata(opCtx), req.getSourceUUID());
            participantDoc.setTargetUUID(req.getTargetUUID());
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
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }
    };

} shardsvrRenameCollectionParticipantCommand;

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
            uassertStatusOK(shardingState->canAcceptShardedCommands());

            const NamespaceString& fromNss = ns();
            const auto& req = request();

            const auto service = RenameCollectionParticipantService::getService(opCtx);
            const auto id = BSON("_id" << fromNss.ns());
            const auto optRenameCollectionParticipant =
                RenameParticipantInstance::lookup(opCtx, service, id);
            if (optRenameCollectionParticipant) {
                uassert(ErrorCodes::CommandFailed,
                        "Provided UUID does not match",
                        optRenameCollectionParticipant.value()->sourceUUID() ==
                            req.getSourceUUID());
                optRenameCollectionParticipant.value()->getUnblockCrudFuture().get(opCtx);
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
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }
    };

} shardsvrRenameCollectionUnblockParticipantCommand;

}  // namespace
}  // namespace mongo
