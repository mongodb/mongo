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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/sharding_ddl_util.h"

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/collection_critical_section_document_gen.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/sharding_util.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/impersonated_user_metadata.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/set_allow_migrations_gen.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"

namespace mongo {

namespace sharding_ddl_util {

namespace {

void updateTags(OperationContext* opCtx,
                const NamespaceString& fromNss,
                const NamespaceString& toNss) {
    // TODO very inefficient function, refactor using a cluster write with bulk update
    auto catalogClient = Grid::get(opCtx)->catalogClient();
    auto tags = uassertStatusOK(catalogClient->getTagsForCollection(opCtx, fromNss));

    if (tags.empty()) {
        return;
    }

    // Wait for majority just for last tag
    auto lastTag = tags.back();
    tags.pop_back();
    for (auto& tag : tags) {
        uassertStatusOK(catalogClient->updateConfigDocument(
            opCtx,
            TagsType::ConfigNS,
            BSON(TagsType::ns(fromNss.ns()) << TagsType::min(tag.getMinKey())),
            BSON("$set" << BSON(TagsType::ns << toNss.ns())),
            false /* upsert */,
            ShardingCatalogClient::kLocalWriteConcern));
    }
    uassertStatusOK(catalogClient->updateConfigDocument(
        opCtx,
        TagsType::ConfigNS,
        BSON(TagsType::ns(fromNss.ns()) << TagsType::min(lastTag.getMinKey())),
        BSON("$set" << BSON(TagsType::ns << toNss.ns())),
        false /* upsert */,
        ShardingCatalogClient::kMajorityWriteConcern));
}

void deleteChunks(OperationContext* opCtx, const NamespaceStringOrUUID& nssOrUUID) {
    const auto catalogClient = Grid::get(opCtx)->catalogClient();

    // Remove config.chunks entries
    const auto chunksQuery = [&]() {
        auto optUUID = nssOrUUID.uuid();
        if (optUUID) {
            return BSON(ChunkType::collectionUUID << *optUUID);
        }

        auto optNss = nssOrUUID.nss();
        invariant(optNss);
        return BSON(ChunkType::ns(optNss->ns()));
    }();

    uassertStatusOK(catalogClient->removeConfigDocuments(
        opCtx, ChunkType::ConfigNS, chunksQuery, ShardingCatalogClient::kMajorityWriteConcern));
}

void deleteCollection(OperationContext* opCtx, const NamespaceString& nss) {
    const auto catalogClient = Grid::get(opCtx)->catalogClient();

    // Remove config.collection entry
    uassertStatusOK(
        catalogClient->removeConfigDocuments(opCtx,
                                             CollectionType::ConfigNS,
                                             BSON(CollectionType::kNssFieldName << nss.ns()),
                                             ShardingCatalogClient::kMajorityWriteConcern));
}

}  // namespace

void sendAuthenticatedCommandToShards(OperationContext* opCtx,
                                      StringData dbName,
                                      const BSONObj& command,
                                      const std::vector<ShardId>& shardIds,
                                      const std::shared_ptr<executor::TaskExecutor>& executor) {
    // The AsyncRequestsSender ignore impersonation metadata so we need to manually attach them to
    // the command
    BSONObjBuilder bob(command);
    rpc::writeAuthDataToImpersonatedUserMetadata(opCtx, &bob);
    auto authenticatedCommand = bob.obj();
    sharding_util::sendCommandToShards(opCtx, dbName, authenticatedCommand, shardIds, executor);
}

void removeTagsMetadataFromConfig(OperationContext* opCtx, const NamespaceString& nss) {
    const auto catalogClient = Grid::get(opCtx)->catalogClient();

    // Remove config.tags entries
    uassertStatusOK(
        catalogClient->removeConfigDocuments(opCtx,
                                             TagsType::ConfigNS,
                                             BSON(TagsType::ns(nss.ns())),
                                             ShardingCatalogClient::kMajorityWriteConcern));
}

void removeCollMetadataFromConfig(OperationContext* opCtx, const CollectionType& coll) {
    IgnoreAPIParametersBlock ignoreApiParametersBlock(opCtx);
    const auto& nss = coll.getNss();

    ON_BLOCK_EXIT(
        [&] { Grid::get(opCtx)->catalogCache()->invalidateCollectionEntry_LINEARIZABLE(nss); });

    const NamespaceStringOrUUID nssOrUUID = coll.getTimestamp()
        ? NamespaceStringOrUUID(nss.db().toString(), coll.getUuid())
        : NamespaceStringOrUUID(nss);

    deleteCollection(opCtx, nss);

    deleteChunks(opCtx, nssOrUUID);

    removeTagsMetadataFromConfig(opCtx, nss);
}

bool removeCollMetadataFromConfig(OperationContext* opCtx, const NamespaceString& nss) {
    IgnoreAPIParametersBlock ignoreApiParametersBlock(opCtx);
    const auto catalogClient = Grid::get(opCtx)->catalogClient();

    ON_BLOCK_EXIT(
        [&] { Grid::get(opCtx)->catalogCache()->invalidateCollectionEntry_LINEARIZABLE(nss); });

    try {
        auto coll = catalogClient->getCollection(opCtx, nss);
        removeCollMetadataFromConfig(opCtx, coll);
        return true;
    } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        // The collection is not sharded or doesn't exist, just tags need to be removed
        removeTagsMetadataFromConfig(opCtx, nss);
        return false;
    }
}

void shardedRenameMetadata(OperationContext* opCtx,
                           CollectionType& fromCollType,
                           const NamespaceString& toNss) {
    auto catalogClient = Grid::get(opCtx)->catalogClient();
    auto fromNss = fromCollType.getNss();

    // Delete eventual TO chunk/collection entries referring a dropped collection
    removeCollMetadataFromConfig(opCtx, toNss);

    // Clone FROM tags to TO
    updateTags(opCtx, fromNss, toNss);

    // Insert the TO collection entry
    fromCollType.setNss(toNss);
    uassertStatusOK(
        catalogClient->insertConfigDocument(opCtx,
                                            CollectionType::ConfigNS,
                                            fromCollType.toBSON(),
                                            ShardingCatalogClient::kMajorityWriteConcern));

    deleteCollection(opCtx, fromNss);
}

void checkShardedRenamePreconditions(OperationContext* opCtx,
                                     const NamespaceString& toNss,
                                     const bool dropTarget) {
    if (!dropTarget) {
        // Check that the sharded target collection doesn't exist
        auto catalogCache = Grid::get(opCtx)->catalogCache();
        try {
            catalogCache->getShardedCollectionRoutingInfo(opCtx, toNss);
            // If no exception is thrown, the collection exists and is sharded
            uasserted(ErrorCodes::CommandFailed,
                      str::stream() << "Sharded target collection " << toNss.ns()
                                    << " exists but dropTarget is not set");
        } catch (const DBException& ex) {
            auto code = ex.code();
            if (code != ErrorCodes::NamespaceNotFound && code != ErrorCodes::NamespaceNotSharded) {
                throw;
            }
        }

        // Check that the unsharded target collection doesn't exist
        auto collectionCatalog = CollectionCatalog::get(opCtx);
        auto targetColl = collectionCatalog->lookupCollectionByNamespace(opCtx, toNss);
        uassert(ErrorCodes::CommandFailed,
                str::stream() << "Target collection " << toNss.ns()
                              << " exists but dropTarget is not set",
                !targetColl);
    }

    // Check that there are no tags associated to the target collection
    auto catalogClient = Grid::get(opCtx)->catalogClient();
    auto tags = uassertStatusOK(catalogClient->getTagsForCollection(opCtx, toNss));
    uassert(ErrorCodes::CommandFailed,
            str::stream() << "Can't rename to target collection " << toNss.ns()
                          << " because it must not have associated tags",
            tags.empty());
}

void checkDbPrimariesOnTheSameShard(OperationContext* opCtx,
                                    const NamespaceString& fromNss,
                                    const NamespaceString& toNss) {
    const auto fromDB =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, fromNss.db()));

    const auto toDB = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getDatabaseWithRefresh(opCtx, toNss.db()));

    uassert(ErrorCodes::CommandFailed,
            "Source and destination collections must be on same shard",
            fromDB.primaryId() == toDB.primaryId());
}

boost::optional<CreateCollectionResponse> checkIfCollectionAlreadySharded(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& key,
    const BSONObj& collation,
    bool unique) {
    auto cm = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithRefresh(opCtx, nss));

    if (!cm.isSharded()) {
        return boost::none;
    }

    auto defaultCollator =
        cm.getDefaultCollator() ? cm.getDefaultCollator()->getSpec().toBSON() : BSONObj();

    // If the collection is already sharded, fail if the deduced options in this request do not
    // match the options the collection was originally sharded with.
    uassert(ErrorCodes::AlreadyInitialized,
            str::stream() << "sharding already enabled for collection " << nss,
            SimpleBSONObjComparator::kInstance.evaluate(cm.getShardKeyPattern().toBSON() == key) &&
                SimpleBSONObjComparator::kInstance.evaluate(defaultCollator == collation) &&
                cm.isUnique() == unique);

    CreateCollectionResponse response(cm.getVersion());
    response.setCollectionUUID(cm.getUUID());
    return response;
}

void acquireRecoverableCriticalSectionBlockWrites(OperationContext* opCtx,
                                                  const NamespaceString& nss,
                                                  const BSONObj& reason,
                                                  const WriteConcernOptions& writeConcern,
                                                  const boost::optional<BSONObj>& additionalInfo) {
    invariant(!opCtx->lockState()->isLocked());

    {
        Lock::GlobalLock lk(opCtx, MODE_IX);
        AutoGetCollection cCollLock(opCtx, nss, MODE_S);

        DBDirectClient dbClient(opCtx);
        auto cursor = dbClient.query(
            NamespaceString::kCollectionCriticalSectionsNamespace,
            BSON(CollectionCriticalSectionDocument::kNssFieldName << nss.toString()));

        // if there is a doc with the same nss -> in order to not fail it must have the same reason
        if (cursor->more()) {
            const auto bsonObj = cursor->next();
            const auto collCSDoc = CollectionCriticalSectionDocument::parse(
                IDLParserErrorContext("AcquireRecoverableCSBW"), bsonObj);

            invariant(collCSDoc.getReason().woCompare(reason) == 0,
                      str::stream()
                          << "Trying to acquire a  critical section blocking writes for namespace "
                          << nss << " and reason " << reason
                          << " but it is already taken by another operation with different reason "
                          << collCSDoc.getReason());

            // Do nothing, the persisted document is already there!
            return;
        }

        // The collection critical section is not taken, try to acquire it.

        // The following code will try to add a doc to config.criticalCollectionSections:
        // - If everything goes well, the shard server op observer will acquire the in-memory CS.
        // - Otherwise this call will fail and the CS won't be taken (neither persisted nor in-mem)
        CollectionCriticalSectionDocument newDoc(nss, reason, false /* blockReads */);
        newDoc.setAdditionalInfo(additionalInfo);

        const auto commandResponse = dbClient.runCommand([&] {
            write_ops::InsertCommandRequest insertOp(
                NamespaceString::kCollectionCriticalSectionsNamespace);
            insertOp.setDocuments({newDoc.toBSON()});
            return insertOp.serialize({});
        }());

        const auto commandReply = commandResponse->getCommandReply();
        uassertStatusOK(getStatusFromWriteCommandReply(commandReply));

        BatchedCommandResponse batchedResponse;
        std::string unusedErrmsg;
        batchedResponse.parseBSON(commandReply, &unusedErrmsg);
        invariant(batchedResponse.getN() > 0,
                  str::stream() << "Insert did not add any doc to collection "
                                << NamespaceString::kCollectionCriticalSectionsNamespace
                                << " for namespace " << nss << " and reason " << reason);
    }

    WriteConcernResult ignoreResult;
    const auto latestOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    uassertStatusOK(waitForWriteConcern(opCtx, latestOpTime, writeConcern, &ignoreResult));
}

void acquireRecoverableCriticalSectionBlockReads(OperationContext* opCtx,
                                                 const NamespaceString& nss,
                                                 const BSONObj& reason,
                                                 const WriteConcernOptions& writeConcern) {
    invariant(!opCtx->lockState()->isLocked());

    {
        AutoGetCollection cCollLock(opCtx, nss, MODE_X);

        DBDirectClient dbClient(opCtx);
        auto cursor = dbClient.query(
            NamespaceString::kCollectionCriticalSectionsNamespace,
            BSON(CollectionCriticalSectionDocument::kNssFieldName << nss.toString()));

        invariant(
            cursor->more(),
            str::stream() << "Trying to acquire a critical section blocking reads for namespace "
                          << nss << " and reason " << reason
                          << " but the critical section wasn't acquired first blocking writers.");
        BSONObj bsonObj = cursor->next();
        const auto collCSDoc = CollectionCriticalSectionDocument::parse(
            IDLParserErrorContext("AcquireRecoverableCSBR"), bsonObj);

        invariant(
            collCSDoc.getReason().woCompare(reason) == 0,
            str::stream() << "Trying to acquire a critical section blocking reads for namespace "
                          << nss << " and reason " << reason
                          << " but it is already taken by another operation with different reason "
                          << collCSDoc.getReason());

        // if there is a document with the same nss, reason and blocking reads -> do nothing, the CS
        // is already taken!
        if (collCSDoc.getBlockReads())
            return;

        // The CS is in the catch-up phase, try to advance it to the commit phase.

        // The following code will try to update a doc from config.criticalCollectionSections:
        // - If everything goes well, the shard server op observer will advance the in-memory CS to
        // the
        //   commit phase (blocking readers).
        // - Otherwise this call will fail and the CS won't be advanced (neither persisted nor
        // in-mem)
        auto commandResponse = dbClient.runCommand([&] {
            const auto query = BSON(
                CollectionCriticalSectionDocument::kNssFieldName
                << nss.toString() << CollectionCriticalSectionDocument::kReasonFieldName << reason);
            const auto update = BSON(
                "$set" << BSON(CollectionCriticalSectionDocument::kBlockReadsFieldName << true));

            write_ops::UpdateCommandRequest updateOp(
                NamespaceString::kCollectionCriticalSectionsNamespace);
            auto updateModification = write_ops::UpdateModification::parseFromClassicUpdate(update);
            write_ops::UpdateOpEntry updateEntry(query, updateModification);
            updateOp.setUpdates({updateEntry});

            return updateOp.serialize({});
        }());

        const auto commandReply = commandResponse->getCommandReply();
        uassertStatusOK(getStatusFromWriteCommandReply(commandReply));

        BatchedCommandResponse batchedResponse;
        std::string unusedErrmsg;
        batchedResponse.parseBSON(commandReply, &unusedErrmsg);
        invariant(batchedResponse.getNModified() > 0,
                  str::stream() << "Update did not modify any doc from collection "
                                << NamespaceString::kCollectionCriticalSectionsNamespace
                                << " for namespace " << nss << " and reason " << reason);
    }

    WriteConcernResult ignoreResult;
    const auto latestOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    uassertStatusOK(waitForWriteConcern(opCtx, latestOpTime, writeConcern, &ignoreResult));
}


void releaseRecoverableCriticalSection(OperationContext* opCtx,
                                       const NamespaceString& nss,
                                       const BSONObj& reason,
                                       const WriteConcernOptions& writeConcern) {
    invariant(!opCtx->lockState()->isLocked());

    {
        AutoGetCollection collLock(opCtx, nss, MODE_X);

        DBDirectClient dbClient(opCtx);

        const auto queryNss =
            BSON(CollectionCriticalSectionDocument::kNssFieldName << nss.toString());
        auto cursor =
            dbClient.query(NamespaceString::kCollectionCriticalSectionsNamespace, queryNss);

        // if there is no document with the same nss -> do nothing!
        if (!cursor->more())
            return;

        BSONObj bsonObj = cursor->next();
        const auto collCSDoc = CollectionCriticalSectionDocument::parse(
            IDLParserErrorContext("ReleaseRecoverableCS"), bsonObj);

        invariant(
            collCSDoc.getReason().woCompare(reason) == 0,
            str::stream() << "Trying to release a critical for namespace " << nss << " and reason "
                          << reason
                          << " but it is already taken by another operation with different reason "
                          << collCSDoc.getReason());


        // The collection critical section is taken (in any phase), try to release it.

        // The following code will try to remove a doc from config.criticalCollectionSections:
        // - If everything goes well, the shard server op observer will release the in-memory CS
        // - Otherwise this call will fail and the CS won't be released (neither persisted nor
        // in-mem)

        auto commandResponse = dbClient.runCommand([&] {
            write_ops::DeleteCommandRequest deleteOp(
                NamespaceString::kCollectionCriticalSectionsNamespace);

            deleteOp.setDeletes({[&] {
                write_ops::DeleteOpEntry entry;
                entry.setQ(queryNss);
                entry.setMulti(true);
                return entry;
            }()});

            return deleteOp.serialize({});
        }());

        const auto commandReply = commandResponse->getCommandReply();
        uassertStatusOK(getStatusFromWriteCommandReply(commandReply));

        BatchedCommandResponse batchedResponse;
        std::string unusedErrmsg;
        batchedResponse.parseBSON(commandReply, &unusedErrmsg);
        invariant(batchedResponse.getN() > 0,
                  str::stream() << "Delete did not remove any doc from collection "
                                << NamespaceString::kCollectionCriticalSectionsNamespace
                                << " for namespace " << nss << " and reason " << reason);
    }

    WriteConcernResult ignoreResult;
    const auto latestOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    uassertStatusOK(waitForWriteConcern(opCtx, latestOpTime, writeConcern, &ignoreResult));
}

void retakeInMemoryRecoverableCriticalSections(OperationContext* opCtx) {

    LOGV2_DEBUG(5549400, 2, "Starting re-acquisition of recoverable critical sections");

    PersistentTaskStore<CollectionCriticalSectionDocument> store(
        NamespaceString::kCollectionCriticalSectionsNamespace);
    store.forEach(opCtx, Query{}, [&opCtx](const CollectionCriticalSectionDocument& doc) {
        const auto& nss = doc.getNss();
        {
            // Entering into the catch-up phase: blocking writes
            Lock::GlobalLock lk(opCtx, MODE_IX);
            AutoGetCollection cCollLock(opCtx, nss, MODE_S);
            auto* const csr = CollectionShardingRuntime::get(opCtx, nss);
            auto csrLock = CollectionShardingRuntime ::CSRLock::lockExclusive(opCtx, csr);
            // It may happen that the ReplWriterWorker enters the critical section before drain mode
            // upon committing a recoverable critical section oplog entry (SERVER-56104)
            if (!csr->getCriticalSectionSignal(
                    opCtx, ShardingMigrationCriticalSection::Operation::kWrite)) {
                csr->enterCriticalSectionCatchUpPhase(csrLock);
            }
        }

        if (doc.getBlockReads()) {
            // Entering into the commit phase: blocking reads
            AutoGetCollection cCollLock(opCtx, nss, MODE_X);
            auto* const csr = CollectionShardingRuntime::get(opCtx, nss);
            auto csrLock = CollectionShardingRuntime ::CSRLock::lockExclusive(opCtx, csr);
            // It may happen that the ReplWriterWorker enters the critical section before drain mode
            // upon committing a recoverable critical section oplog entry (SERVER-56104)
            if (!csr->getCriticalSectionSignal(
                    opCtx, ShardingMigrationCriticalSection::Operation::kRead)) {
                csr->enterCriticalSectionCommitPhase(csrLock);
            }

            CollectionShardingRuntime::get(opCtx, nss)->clearFilteringMetadata(opCtx);
        }

        return true;
    });

    LOGV2_DEBUG(5549401, 2, "Finished re-acquisition of recoverable critical sections");
}

void stopMigrations(OperationContext* opCtx, const NamespaceString& nss) {
    const ConfigsvrSetAllowMigrations configsvrSetAllowMigrationsCmd(nss,
                                                                     false /* allowMigrations */);
    const auto swSetAllowMigrationsResult =
        Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            NamespaceString::kAdminDb.toString(),
            CommandHelpers::appendMajorityWriteConcern(configsvrSetAllowMigrationsCmd.toBSON({})),
            Shard::RetryPolicy::kIdempotent  // Although ConfigsvrSetAllowMigrations is not really
                                             // idempotent (because it will cause the collection
                                             // version to be bumped), it is safe to be retried.
        );

    uassertStatusOKWithContext(
        Shard::CommandResponse::getEffectiveStatus(std::move(swSetAllowMigrationsResult)),
        str::stream() << "Error setting allowMigrations to false for collection "
                      << nss.toString());
}

DropReply dropCollectionLocally(OperationContext* opCtx, const NamespaceString& nss) {
    DropReply result;
    uassertStatusOK(dropCollection(
        opCtx, nss, &result, DropCollectionSystemCollectionMode::kDisallowSystemCollectionDrops));

    {
        // Clear CollectionShardingRuntime entry
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());
        Lock::DBLock dbLock(opCtx, nss.db(), MODE_IX);
        Lock::CollectionLock collLock(opCtx, nss, MODE_IX);
        auto* csr = CollectionShardingRuntime::get(opCtx, nss);
        csr->clearFilteringMetadata(opCtx);
    }

    return result;
}

}  // namespace sharding_ddl_util
}  // namespace mongo
