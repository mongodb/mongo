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
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/commands.h"
#include "mongo/db/s/shard_metadata_util.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"

namespace mongo {
namespace {

void dropCollectionLocally(OperationContext* opCtx, const NamespaceString& nss) {
    bool knownNss = [&]() {
        try {
            DropReply result;
            uassertStatusOK(
                dropCollection(opCtx,
                               nss,
                               &result,
                               DropCollectionSystemCollectionMode::kDisallowSystemCollectionDrops));
            return true;
        } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
            return false;
        }
    }();

    if (knownNss) {
        uassertStatusOK(shardmetadatautil::dropChunksAndDeleteCollectionsEntry(opCtx, nss));
    }

    LOGV2_DEBUG(5448800,
                1,
                "Dropped target collection locally on renameCollection participant",
                "namespace"_attr = nss,
                "collectionExisted"_attr = knownNss);
}

class ShardsvrRenameCollectionParticipantCommand final
    : public TypedCommand<ShardsvrRenameCollectionParticipantCommand> {
public:
    using Request = ShardsvrRenameCollectionParticipant;

    bool adminOnly() const override {
        return false;
    }

    std::string help() const override {
        return "Internal command. Do not call directly. Locally renames a collection.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << Request::kCommandName
                                  << " must be called with majority writeConcern, got "
                                  << opCtx->getWriteConcern().wMode,
                    opCtx->getWriteConcern().wMode == WriteConcernOptions::kMajority);

            auto const shardingState = ShardingState::get(opCtx);
            uassertStatusOK(shardingState->canAcceptShardedCommands());

            const auto& req = request();
            const auto& fromNss = ns();
            const auto& toNss = req.getTo();
            const RenameCollectionOptions options{req.getDropTarget(), req.getStayTemp()};

            // Acquire source/target critical sections
            sharding_ddl_util::acquireCriticalSection(opCtx, fromNss);
            sharding_ddl_util::acquireCriticalSection(opCtx, toNss);

            dropCollectionLocally(opCtx, toNss);

            try {
                // Rename the collection locally and clear the cache
                validateAndRunRenameCollection(opCtx, fromNss, toNss, options);
                uassertStatusOK(
                    shardmetadatautil::dropChunksAndDeleteCollectionsEntry(opCtx, fromNss));
            } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                // It's ok for a participant shard to have no knowledge about a collection
                LOGV2_DEBUG(
                    5448801,
                    1,
                    "Source namespace not found while trying to rename collection on participant",
                    "namespace"_attr = fromNss);
            }
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

    std::string help() const override {
        return "Internal command. Do not call directly. Releases the critical section of source "
               "and target collections.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << Request::kCommandName
                                  << " must be called with majority writeConcern, got "
                                  << opCtx->getWriteConcern().wMode,
                    opCtx->getWriteConcern().wMode == WriteConcernOptions::kMajority);

            auto const shardingState = ShardingState::get(opCtx);
            uassertStatusOK(shardingState->canAcceptShardedCommands());

            const auto& fromNss = ns();
            const auto& toNss = request().getTo();

            // Release source/target critical sections
            sharding_ddl_util::releaseCriticalSection(opCtx, fromNss);
            sharding_ddl_util::releaseCriticalSection(opCtx, toNss);

            auto catalog = Grid::get(opCtx)->catalogCache();
            uassertStatusOK(catalog->getCollectionRoutingInfoWithRefresh(opCtx, toNss));
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
