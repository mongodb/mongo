/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/commands.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/dist_lock_manager.h"
#include "mongo/db/s/shard_metadata_util.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/s/sharded_collections_ddl_parameters_gen.h"

namespace mongo {
namespace {

bool isCollectionSharded(OperationContext* opCtx, const NamespaceString& nss) {
    AutoGetCollectionForRead lock(opCtx, nss);
    return opCtx->writesAreReplicated() &&
        CollectionShardingState::get(opCtx, nss)->getCollectionDescription(opCtx).isSharded();
}

RenameCollectionResponse renameUnshardedCollection(OperationContext* opCtx,
                                                   const ShardsvrRenameCollection& request,
                                                   const NamespaceString& fromNss) {
    const auto& toNss = request.getTo();

    const auto fromDB = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getDatabaseWithRefresh(opCtx, fromNss.db()));

    const auto toDB = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getDatabaseWithRefresh(opCtx, toNss.db()));

    uassert(13137,
            "Source and destination collections must be on same shard",
            fromDB.primaryId() == toDB.primaryId());

    // Make sure that source and target collection are not sharded
    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "source namespace '" << fromNss << "' must not be sharded",
            !isCollectionSharded(opCtx, fromNss));
    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "cannot rename to sharded collection '" << toNss << "'",
            !isCollectionSharded(opCtx, toNss));

    RenameCollectionOptions options{request.getDropTarget(), request.getStayTemp()};
    validateAndRunRenameCollection(opCtx, fromNss, toNss, options);

    return RenameCollectionResponse(ChunkVersion::UNSHARDED());
}

void sendCommandToParticipants(OperationContext* opCtx,
                               StringData db,
                               StringData cmdName,
                               const BSONObj& cmd) {
    const auto selfShardId = ShardingState::get(opCtx)->shardId();
    auto shardRegistry = Grid::get(opCtx)->shardRegistry();
    const auto allShardIds = shardRegistry->getAllShardIds(opCtx);

    for (const auto& shardId : allShardIds) {
        if (shardId == selfShardId) {
            continue;
        }

        auto shard = uassertStatusOK(shardRegistry->getShard(opCtx, shardId));
        const auto cmdResponse = uassertStatusOK(shard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            db.toString(),
            CommandHelpers::appendMajorityWriteConcern(cmd),
            Shard::RetryPolicy::kNoRetry));
        uassertStatusOKWithContext(Shard::CommandResponse::getEffectiveStatus(cmdResponse),
                                   str::stream() << "Error processing " << cmdName << " on shard "
                                                 << shardId);
    }
}

RenameCollectionResponse renameShardedCollection(OperationContext* opCtx,
                                                 const ShardsvrRenameCollection& request,
                                                 const NamespaceString& fromNss) {
    const auto toNss = request.getTo();

    uassert(ErrorCodes::CommandFailed,
            "Source and destination collections must be on the same database.",
            fromNss.db() == toNss.db());

    auto distLockManager = DistLockManager::get(opCtx->getServiceContext());
    const auto dbDistLock = uassertStatusOK(distLockManager->lock(
        opCtx, fromNss.db(), "RenameCollection", DistLockManager::kDefaultLockTimeout));
    const auto fromCollDistLock = uassertStatusOK(distLockManager->lock(
        opCtx, fromNss.ns(), "RenameCollection", DistLockManager::kDefaultLockTimeout));
    const auto toCollDistLock = uassertStatusOK(distLockManager->lock(
        opCtx, toNss.ns(), "RenameCollection", DistLockManager::kDefaultLockTimeout));

    RenameCollectionOptions options{request.getDropTarget(), request.getStayTemp()};

    sharding_ddl_util::checkShardedRenamePreconditions(opCtx, toNss, options.dropTarget);

    {
        // Take the source collection critical section
        AutoGetCollection sourceCollLock(opCtx, fromNss, MODE_X);
        auto* const fromCsr = CollectionShardingRuntime::get(opCtx, fromNss);
        auto fromCsrLock = CollectionShardingRuntime::CSRLock::lockExclusive(opCtx, fromCsr);
        fromCsr->enterCriticalSectionCatchUpPhase(fromCsrLock);
        fromCsr->enterCriticalSectionCommitPhase(fromCsrLock);
    }

    {
        // Take the destination collection critical section
        AutoGetCollection targetCollLock(opCtx, toNss, MODE_X);
        auto* const toCsr = CollectionShardingRuntime::get(opCtx, toNss);
        auto toCsrLock = CollectionShardingRuntime::CSRLock::lockExclusive(opCtx, toCsr);
        if (!toCsr->getCurrentMetadataIfKnown()) {
            // Setting metadata to UNSHARDED (can't be UNKNOWN when taking the critical section)
            toCsr->setFilteringMetadata(opCtx, CollectionMetadata());
        }
        toCsr->enterCriticalSectionCatchUpPhase(toCsrLock);
        toCsr->enterCriticalSectionCommitPhase(toCsrLock);
    }

    // Rename the collection locally and clear the cache
    validateAndRunRenameCollection(opCtx, fromNss, toNss, options);
    uassertStatusOK(shardmetadatautil::dropChunksAndDeleteCollectionsEntry(opCtx, fromNss));

    // Rename the collection locally on all other shards
    ShardsvrRenameCollectionParticipant renameCollParticipantRequest(fromNss);
    renameCollParticipantRequest.setDbName(fromNss.db());
    renameCollParticipantRequest.setDropTarget(request.getDropTarget());
    renameCollParticipantRequest.setStayTemp(request.getStayTemp());
    renameCollParticipantRequest.setTo(request.getTo());
    sendCommandToParticipants(opCtx,
                              fromNss.db(),
                              ShardsvrRenameCollectionParticipant::kCommandName,
                              renameCollParticipantRequest.toBSON({}));

    sharding_ddl_util::shardedRenameMetadata(opCtx, fromNss, toNss);

    // Unblock participants for r/w on source and destination collections
    ShardsvrRenameCollectionUnblockParticipant unblockParticipantRequest(fromNss);
    unblockParticipantRequest.setDbName(fromNss.db());
    unblockParticipantRequest.setTo(toNss);
    sendCommandToParticipants(opCtx,
                              fromNss.db(),
                              ShardsvrRenameCollectionUnblockParticipant::kCommandName,
                              unblockParticipantRequest.toBSON({}));

    {
        // Clear source critical section
        AutoGetCollection sourceCollLock(opCtx, fromNss, MODE_X);
        auto* const fromCsr = CollectionShardingRuntime::get(opCtx, fromNss);
        fromCsr->exitCriticalSection(opCtx);
        fromCsr->clearFilteringMetadata(opCtx);
    }

    {
        // Clear target critical section
        AutoGetCollection targetCollLock(opCtx, toNss, MODE_X);
        auto* const toCsr = CollectionShardingRuntime::get(opCtx, toNss);
        toCsr->exitCriticalSection(opCtx);
        toCsr->clearFilteringMetadata(opCtx);
    }

    auto catalog = Grid::get(opCtx)->catalogCache();
    auto cm = uassertStatusOK(catalog->getCollectionRoutingInfoWithRefresh(opCtx, toNss));
    return RenameCollectionResponse(cm.getVersion());
}

class ShardsvrRenameCollectionCommand final : public TypedCommand<ShardsvrRenameCollectionCommand> {
public:
    using Request = ShardsvrRenameCollection;
    using Response = RenameCollectionResponse;

    bool adminOnly() const override {
        return false;
    }

    std::string help() const override {
        return "Internal command. Do not call directly. Renames a collection.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            const auto& req = request();
            const auto& fromNss = ns();

            auto const shardingState = ShardingState::get(opCtx);
            uassertStatusOK(shardingState->canAcceptShardedCommands());
            bool newPath = feature_flags::gShardingFullDDLSupport.isEnabled(
                serverGlobalParams.featureCompatibility);

            if (!isCollectionSharded(opCtx, fromNss) || !newPath) {
                return renameUnshardedCollection(opCtx, req, fromNss);
            }

            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << Request::kCommandName
                                  << " must be called with majority writeConcern, got "
                                  << opCtx->getWriteConcern().wMode,
                    opCtx->getWriteConcern().wMode == WriteConcernOptions::kMajority);

            return renameShardedCollection(opCtx, req, fromNss);
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

} shardsvrRenameCollectionCommand;

}  // namespace
}  // namespace mongo
