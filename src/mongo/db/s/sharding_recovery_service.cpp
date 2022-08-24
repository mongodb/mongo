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


#include <set>

#include "mongo/platform/basic.h"

#include "mongo/db/s/sharding_recovery_service.h"

#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/collection_critical_section_document_gen.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/sharding_migration_critical_section.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

namespace recoverable_critical_section_util {

bool inRecoveryMode(OperationContext* opCtx) {
    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (!replCoord->isReplEnabled()) {
        return false;
    }

    const auto memberState = replCoord->getMemberState();
    return memberState.startup2() || memberState.rollback();
}

}  // namespace recoverable_critical_section_util

namespace {
const auto serviceDecorator = ServiceContext::declareDecoration<ShardingRecoveryService>();
}

ShardingRecoveryService* ShardingRecoveryService::get(ServiceContext* serviceContext) {
    return &serviceDecorator(serviceContext);
}

ShardingRecoveryService* ShardingRecoveryService::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

const ReplicaSetAwareServiceRegistry::Registerer<ShardingRecoveryService>
    shardingRecoveryServiceRegisterer("ShardingRecoveryService");

void ShardingRecoveryService::acquireRecoverableCriticalSectionBlockWrites(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& reason,
    const WriteConcernOptions& writeConcern,
    const boost::optional<BSONObj>& additionalInfo) {
    LOGV2_DEBUG(5656600,
                3,
                "Acquiring recoverable critical section blocking writes",
                "namespace"_attr = nss,
                "reason"_attr = reason,
                "writeConcern"_attr = writeConcern);

    invariant(!opCtx->lockState()->isLocked());

    {
        Lock::GlobalLock lk(opCtx, MODE_IX);
        AutoGetCollection cCollLock(opCtx, nss, MODE_S);

        DBDirectClient dbClient(opCtx);
        FindCommandRequest findRequest{NamespaceString::kCollectionCriticalSectionsNamespace};
        findRequest.setFilter(
            BSON(CollectionCriticalSectionDocument::kNssFieldName << nss.toString()));
        auto cursor = dbClient.find(std::move(findRequest));

        // if there is a doc with the same nss -> in order to not fail it must have the same reason
        if (cursor->more()) {
            const auto bsonObj = cursor->next();
            const auto collCSDoc = CollectionCriticalSectionDocument::parse(
                IDLParserContext("AcquireRecoverableCSBW"), bsonObj);

            invariant(collCSDoc.getReason().woCompare(reason) == 0,
                      str::stream()
                          << "Trying to acquire a  critical section blocking writes for namespace "
                          << nss << " and reason " << reason
                          << " but it is already taken by another operation with different reason "
                          << collCSDoc.getReason());

            LOGV2_DEBUG(
                5656601,
                3,
                "The recoverable critical section was already acquired to block writes, do nothing",
                "namespace"_attr = nss,
                "reason"_attr = reason,
                "writeConcern"_attr = writeConcern);

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

    LOGV2_DEBUG(5656602,
                2,
                "Acquired recoverable critical section blocking writes",
                "namespace"_attr = nss,
                "reason"_attr = reason,
                "writeConcern"_attr = writeConcern);
}

void ShardingRecoveryService::promoteRecoverableCriticalSectionToBlockAlsoReads(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& reason,
    const WriteConcernOptions& writeConcern) {
    LOGV2_DEBUG(5656603,
                3,
                "Promoting recoverable critical section to also block reads",
                "namespace"_attr = nss,
                "reason"_attr = reason,
                "writeConcern"_attr = writeConcern);

    invariant(!opCtx->lockState()->isLocked());

    {
        AutoGetCollection cCollLock(opCtx, nss, MODE_X);

        DBDirectClient dbClient(opCtx);
        FindCommandRequest findRequest{NamespaceString::kCollectionCriticalSectionsNamespace};
        findRequest.setFilter(
            BSON(CollectionCriticalSectionDocument::kNssFieldName << nss.toString()));
        auto cursor = dbClient.find(std::move(findRequest));

        invariant(
            cursor->more(),
            str::stream() << "Trying to acquire a critical section blocking reads for namespace "
                          << nss << " and reason " << reason
                          << " but the critical section wasn't acquired first blocking writers.");
        BSONObj bsonObj = cursor->next();
        const auto collCSDoc = CollectionCriticalSectionDocument::parse(
            IDLParserContext("AcquireRecoverableCSBR"), bsonObj);

        invariant(
            collCSDoc.getReason().woCompare(reason) == 0,
            str::stream() << "Trying to acquire a critical section blocking reads for namespace "
                          << nss << " and reason " << reason
                          << " but it is already taken by another operation with different reason "
                          << collCSDoc.getReason());

        // if there is a document with the same nss, reason and blocking reads -> do nothing, the CS
        // is already taken!
        if (collCSDoc.getBlockReads()) {
            LOGV2_DEBUG(5656604,
                        3,
                        "The recoverable critical section was already promoted to also block "
                        "reads, do nothing",
                        "namespace"_attr = nss,
                        "reason"_attr = reason,
                        "writeConcern"_attr = writeConcern);
            return;
        }

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

    LOGV2_DEBUG(5656605,
                2,
                "Promoted recoverable critical section to also block reads",
                "namespace"_attr = nss,
                "reason"_attr = reason,
                "writeConcern"_attr = writeConcern);
}

void ShardingRecoveryService::releaseRecoverableCriticalSection(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& reason,
    const WriteConcernOptions& writeConcern) {
    LOGV2_DEBUG(5656606,
                3,
                "Releasing recoverable critical section",
                "namespace"_attr = nss,
                "reason"_attr = reason,
                "writeConcern"_attr = writeConcern);

    invariant(!opCtx->lockState()->isLocked());

    {
        AutoGetCollection collLock(opCtx, nss, MODE_X);

        DBDirectClient dbClient(opCtx);

        const auto queryNss =
            BSON(CollectionCriticalSectionDocument::kNssFieldName << nss.toString());
        FindCommandRequest findRequest{NamespaceString::kCollectionCriticalSectionsNamespace};
        findRequest.setFilter(queryNss);
        auto cursor = dbClient.find(std::move(findRequest));

        // if there is no document with the same nss -> do nothing!
        if (!cursor->more()) {
            LOGV2_DEBUG(5656607,
                        3,
                        "The recoverable critical section was already released, do nothing",
                        "namespace"_attr = nss,
                        "reason"_attr = reason,
                        "writeConcern"_attr = writeConcern);
            return;
        }

        BSONObj bsonObj = cursor->next();
        const auto collCSDoc = CollectionCriticalSectionDocument::parse(
            IDLParserContext("ReleaseRecoverableCS"), bsonObj);

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

    LOGV2_DEBUG(5656608,
                2,
                "Released recoverable critical section",
                "namespace"_attr = nss,
                "reason"_attr = reason,
                "writeConcern"_attr = writeConcern);
}

void ShardingRecoveryService::recoverRecoverableCriticalSections(OperationContext* opCtx) {
    LOGV2_DEBUG(5604000, 2, "Recovering all recoverable critical sections");

    // Release all in-memory critical sections
    const auto collectionNames = CollectionShardingState::getCollectionNames(opCtx);
    for (const auto& collName : collectionNames) {
        try {
            AutoGetCollection collLock(opCtx, collName, MODE_X);
            auto* const csr = CollectionShardingRuntime::get(opCtx, collName);
            auto csrLock = CollectionShardingRuntime::CSRLock::lockExclusive(opCtx, csr);
            csr->exitCriticalSectionNoChecks(csrLock);
        } catch (const ExceptionFor<ErrorCodes::CommandNotSupportedOnView>&) {
            LOGV2_DEBUG(6050800,
                        2,
                        "Skipping attempting to exit critical section for view in "
                        "recoverRecoverableCriticalSections",
                        "namespace"_attr = collName);
        }
    }

    // Map the critical sections that are on disk to memory
    PersistentTaskStore<CollectionCriticalSectionDocument> store(
        NamespaceString::kCollectionCriticalSectionsNamespace);
    store.forEach(opCtx, BSONObj{}, [&opCtx](const CollectionCriticalSectionDocument& doc) {
        const auto& nss = doc.getNss();
        {
            AutoGetCollection collLock(opCtx, nss, MODE_X);
            auto* const csr = CollectionShardingRuntime::get(opCtx, nss);
            auto csrLock = CollectionShardingRuntime::CSRLock::lockExclusive(opCtx, csr);
            csr->enterCriticalSectionCatchUpPhase(csrLock, doc.getReason());
            if (doc.getBlockReads())
                csr->enterCriticalSectionCommitPhase(csrLock, doc.getReason());

            return true;
        }
    });

    LOGV2_DEBUG(5604001, 2, "Recovered all recoverable critical sections");
}

void ShardingRecoveryService::recoverStates(OperationContext* opCtx,
                                            const std::set<NamespaceString>& rollbackNamespaces) {

    if (rollbackNamespaces.find(NamespaceString::kCollectionCriticalSectionsNamespace) !=
        rollbackNamespaces.end()) {
        ShardingRecoveryService::get(opCtx)->recoverRecoverableCriticalSections(opCtx);
    }

    if (rollbackNamespaces.find(NamespaceString::kShardCollectionCatalogNamespace) !=
            rollbackNamespaces.end() ||
        rollbackNamespaces.find(CollectionType::ConfigNS) != rollbackNamespaces.end()) {
        ShardingRecoveryService::get(opCtx)->recoverIndexesCatalog(opCtx);
    }
}

void ShardingRecoveryService::recoverIndexesCatalog(OperationContext* opCtx) {
    LOGV2_DEBUG(6686500, 2, "Recovering all sharding index catalog");

    // Reset all in-memory index versions.
    const auto collectionNames = CollectionShardingState::getCollectionNames(opCtx);
    for (const auto& collName : collectionNames) {
        try {
            AutoGetCollection collLock(opCtx, collName, MODE_IS);
            auto* const csr = CollectionShardingRuntime::get(opCtx, collName);
            auto csrLock = CollectionShardingRuntime::CSRLock::lockExclusive(opCtx, csr);
            csr->setIndexVersion(opCtx, boost::none);
        } catch (const ExceptionFor<ErrorCodes::CommandNotSupportedOnView>&) {
            LOGV2_DEBUG(6686501,
                        2,
                        "Skipping attempting to set index version for view in "
                        "recoverIndexCatalogs",
                        "namespace"_attr = collName);
        }
    }

    DBDirectClient client(opCtx);
    FindCommandRequest findRequest{NamespaceString::kShardCollectionCatalogNamespace};
    findRequest.setProjection(BSON(CollectionTypeBase::kNssFieldName
                                   << 1 << CollectionTypeBase::kIndexVersionFieldName << 1));
    // Map the index versions that are on disk to memory.
    client.find(std::move(findRequest), [&opCtx](const BSONObj& coll) {
        auto nss = NamespaceString(coll[CollectionTypeBase::kNssFieldName].str());
        auto indexVersion = coll[CollectionTypeBase::kIndexVersionFieldName].timestamp();
        AutoGetCollection collLock(opCtx, nss, MODE_IS);
        CollectionShardingRuntime::get(opCtx, nss)->setIndexVersion(opCtx, indexVersion);
    });

    LOGV2_DEBUG(6686502, 2, "Recovered all index versions");
}

}  // namespace mongo
