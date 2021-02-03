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
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/shard_metadata_util.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"

namespace mongo {
namespace {

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

            auto req = request();
            const auto fromNss = ns();
            const auto toNss = req.getTo();
            RenameCollectionOptions options;
            options.dropTarget = req.getDropTarget();
            options.stayTemp = req.getStayTemp();

            {
                // Take the source collection critical section
                AutoGetCollection sourceCollLock(opCtx, fromNss, MODE_X);
                auto* const fromCsr = CollectionShardingRuntime::get(opCtx, fromNss);
                auto fromCsrLock =
                    CollectionShardingRuntime::CSRLock::lockExclusive(opCtx, fromCsr);
                if (!fromCsr->getCurrentMetadataIfKnown()) {
                    // Setting metadata to UNSHARDED (can't be UNKNOWN when taking the critical
                    // section)
                    fromCsr->setFilteringMetadata(opCtx, CollectionMetadata());
                }
                fromCsr->enterCriticalSectionCatchUpPhase(fromCsrLock);
                fromCsr->enterCriticalSectionCommitPhase(fromCsrLock);
                fromCsr->clearFilteringMetadata(opCtx);
            }

            {
                // Take the destination collection critical section
                AutoGetCollection targetCollLock(opCtx, toNss, MODE_X);
                auto* const toCsr = CollectionShardingRuntime::get(opCtx, toNss);
                auto toCsrLock = CollectionShardingRuntime::CSRLock::lockExclusive(opCtx, toCsr);
                if (!toCsr->getCurrentMetadataIfKnown()) {
                    // Setting metadata to UNSHARDED (can't be UNKNOWN when taking the critical
                    // section)
                    toCsr->setFilteringMetadata(opCtx, CollectionMetadata());
                }
                toCsr->enterCriticalSectionCatchUpPhase(toCsrLock);
                toCsr->enterCriticalSectionCommitPhase(toCsrLock);
                toCsr->clearFilteringMetadata(opCtx);
            }

            // Rename the collection locally and clear the cache
            validateAndRunRenameCollection(opCtx, fromNss, toNss, options);
            uassertStatusOK(shardmetadatautil::dropChunksAndDeleteCollectionsEntry(opCtx, fromNss));
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

            const auto& req = request();
            const auto& fromNss = ns();
            const auto& toNss = req.getTo();

            {
                AutoGetCollection sourceCollLock(opCtx, fromNss, MODE_X);
                auto* const fromCsr = CollectionShardingRuntime::get(opCtx, fromNss);
                auto fromCsrLock =
                    CollectionShardingRuntime::CSRLock::lockExclusive(opCtx, fromCsr);
                fromCsr->exitCriticalSection(opCtx);

                AutoGetCollection targetCollLock(opCtx, toNss, MODE_X);
                auto* const toCsr = CollectionShardingRuntime::get(opCtx, toNss);
                auto toCsrLock = CollectionShardingRuntime::CSRLock::lockExclusive(opCtx, toCsr);
                toCsr->exitCriticalSection(opCtx);
            }

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
