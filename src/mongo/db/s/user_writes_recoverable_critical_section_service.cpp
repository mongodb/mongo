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


#include "mongo/platform/basic.h"

#include "mongo/db/s/user_writes_recoverable_critical_section_service.h"

#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/global_user_write_block_state.h"
#include "mongo/db/s/user_writes_critical_section_document_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/sharding_catalog_client.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

namespace user_writes_recoverable_critical_section_util {

bool inRecoveryMode(OperationContext* opCtx) {
    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (!replCoord->isReplEnabled()) {
        return false;
    }

    const auto memberState = replCoord->getMemberState();
    return memberState.startup2() || memberState.rollback();
}

}  // namespace user_writes_recoverable_critical_section_util

namespace {
MONGO_FAIL_POINT_DEFINE(skipRecoverUserWriteCriticalSections);

const auto serviceDecorator =
    ServiceContext::declareDecoration<UserWritesRecoverableCriticalSectionService>();

BSONObj findRecoverableCriticalSectionDoc(OperationContext* opCtx, const NamespaceString& nss) {
    DBDirectClient dbClient(opCtx);

    const auto queryNss =
        BSON(UserWriteBlockingCriticalSectionDocument::kNssFieldName << nss.toString());
    FindCommandRequest findRequest{NamespaceString::kUserWritesCriticalSectionsNamespace};
    findRequest.setFilter(queryNss);
    return dbClient.findOne(std::move(findRequest));
}

void setBlockUserWritesDocumentField(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     bool blockUserWrites) {
    PersistentTaskStore<UserWriteBlockingCriticalSectionDocument> store(
        NamespaceString::kUserWritesCriticalSectionsNamespace);
    store.update(
        opCtx,
        BSON(UserWriteBlockingCriticalSectionDocument::kNssFieldName << nss.toString()),
        BSON("$set" << BSON(UserWriteBlockingCriticalSectionDocument::kBlockUserWritesFieldName
                            << blockUserWrites)),
        ShardingCatalogClient::kLocalWriteConcern);
}

void acquireRecoverableCriticalSection(OperationContext* opCtx,
                                       const NamespaceString& nss,
                                       bool blockShardedDDL,
                                       bool blockUserWrites) {
    LOGV2_DEBUG(6351900,
                3,
                "Acquiring user writes recoverable critical section",
                "namespace"_attr = nss,
                "blockShardedDDL"_attr = blockShardedDDL,
                "blockUserWrites"_attr = blockUserWrites);

    invariant(nss == UserWritesRecoverableCriticalSectionService::kGlobalUserWritesNamespace);
    invariant(!opCtx->lockState()->isLocked());

    {
        // If we intend to start blocking user writes, take the GlobalLock in MODE_X in order to
        // ensure that any ongoing writes have completed.
        Lock::GlobalLock globalLock(opCtx, blockUserWrites ? MODE_X : MODE_IX);

        const auto bsonObj = findRecoverableCriticalSectionDoc(opCtx, nss);
        if (!bsonObj.isEmpty()) {
            const auto collCSDoc = UserWriteBlockingCriticalSectionDocument::parse(
                IDLParserContext("AcquireUserWritesCS"), bsonObj);

            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << "Cannot acquire user writes critical section with different "
                                     "options than the already existing one. blockShardedDDL: "
                                  << blockShardedDDL
                                  << ", current: " << collCSDoc.getBlockNewUserShardedDDL(),
                    !blockShardedDDL || collCSDoc.getBlockNewUserShardedDDL() == blockShardedDDL);

            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << "Cannot acquire user writes critical section with different "
                                     "options than the already existing one. blockUserWrites: "
                                  << blockUserWrites
                                  << ", current: " << collCSDoc.getBlockNewUserShardedDDL(),
                    !blockUserWrites || collCSDoc.getBlockUserWrites() == blockUserWrites);

            LOGV2_DEBUG(6351914,
                        3,
                        "The user writes recoverable critical section was already acquired",
                        "namespace"_attr = nss);
            return;
        }

        // Acquire the critical section by inserting the critical section document. The OpObserver
        // will take the in-memory CS when reacting to the insert event.
        UserWriteBlockingCriticalSectionDocument newDoc(nss);
        newDoc.setBlockNewUserShardedDDL(blockShardedDDL);
        newDoc.setBlockUserWrites(blockUserWrites);

        PersistentTaskStore<UserWriteBlockingCriticalSectionDocument> store(
            NamespaceString::kUserWritesCriticalSectionsNamespace);
        store.add(opCtx, newDoc, ShardingCatalogClient::kLocalWriteConcern);
    }

    LOGV2_DEBUG(6351901,
                2,
                "Acquired user writes recoverable critical section",
                "namespace"_attr = nss,
                "blockShardedDDL"_attr = blockShardedDDL,
                "blockUserWrites"_attr = blockUserWrites);
}
}  // namespace

const NamespaceString UserWritesRecoverableCriticalSectionService::kGlobalUserWritesNamespace =
    NamespaceString();

UserWritesRecoverableCriticalSectionService* UserWritesRecoverableCriticalSectionService::get(
    ServiceContext* serviceContext) {
    return &serviceDecorator(serviceContext);
}

UserWritesRecoverableCriticalSectionService* UserWritesRecoverableCriticalSectionService::get(
    OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

const ReplicaSetAwareServiceRegistry::Registerer<UserWritesRecoverableCriticalSectionService>
    UserWritesRecoverableCriticalSectionServiceServiceRegisterer(
        "UserWritesRecoverableCriticalSectionService");

bool UserWritesRecoverableCriticalSectionService::shouldRegisterReplicaSetAwareService() const {
    return serverGlobalParams.clusterRole != ClusterRole::ConfigServer;
}

void UserWritesRecoverableCriticalSectionService::
    acquireRecoverableCriticalSectionBlockingUserWrites(OperationContext* opCtx,
                                                        const NamespaceString& nss) {
    invariant(serverGlobalParams.clusterRole == ClusterRole::None,
              "Acquiring the user writes recoverable critical section directly to start blocking "
              "writes is only allowed on non-sharded cluster.");

    acquireRecoverableCriticalSection(
        opCtx, nss, false /* blockShardedDDL */, true /* blockUserWrites */);
}

void UserWritesRecoverableCriticalSectionService::
    acquireRecoverableCriticalSectionBlockNewShardedDDL(OperationContext* opCtx,
                                                        const NamespaceString& nss) {
    invariant(serverGlobalParams.clusterRole != ClusterRole::None,
              "Acquiring the user writes recoverable critical section blocking only sharded DDL is "
              "only allowed on sharded clusters");

    // Take the user writes critical section blocking only ShardingDDLCoordinators.
    acquireRecoverableCriticalSection(
        opCtx, nss, true /* blockShardedDDL */, false /* blockUserWrites */);
}

void UserWritesRecoverableCriticalSectionService::
    promoteRecoverableCriticalSectionToBlockUserWrites(OperationContext* opCtx,
                                                       const NamespaceString& nss) {
    invariant(serverGlobalParams.clusterRole != ClusterRole::None,
              "Promoting the user writes recoverable critical section to also block user writes is "
              "only allowed on sharded clusters");

    LOGV2_DEBUG(6351902,
                3,
                "Promoting user writes recoverable critical section to also block reads",
                "namespace"_attr = nss);

    invariant(nss == UserWritesRecoverableCriticalSectionService::kGlobalUserWritesNamespace);
    invariant(!opCtx->lockState()->isLocked());

    {
        // Take the GlobalLock in MODE_X in order to ensure that any ongoing writes have completed
        // before starting to block new writes.
        Lock::GlobalLock globalLock(opCtx, MODE_X);

        const auto bsonObj = findRecoverableCriticalSectionDoc(opCtx, nss);
        uassert(ErrorCodes::IllegalOperation,
                "Cannot promote user writes critical section to block user writes if critical "
                "section document not persisted first.",
                !bsonObj.isEmpty());

        const auto collCSDoc = UserWriteBlockingCriticalSectionDocument::parse(
            IDLParserContext("PromoteUserWritesCS"), bsonObj);

        uassert(ErrorCodes::IllegalOperation,
                "Cannot promote user writes critical section to block user writes if sharded DDL "
                "operations have not been blocked first.",
                collCSDoc.getBlockNewUserShardedDDL());

        // If we are already blocking user writes, then we are done.
        if (collCSDoc.getBlockUserWrites()) {
            LOGV2_DEBUG(6351903,
                        3,
                        "The user writes recoverable critical section was already promoted to also "
                        "block user "
                        "writes, do nothing",
                        "namespace"_attr = nss);
            return;
        }

        // Promote the critical section to block also user writes by updating the critical section
        // document. The OpObserver will promote the in-memory CS when reacting to the update event.
        setBlockUserWritesDocumentField(opCtx, nss, true /* blockUserWrites */);
    }

    LOGV2_DEBUG(6351904,
                2,
                "Promoted user writes recoverable critical section to also block user writes",
                "namespace"_attr = nss);
}

void UserWritesRecoverableCriticalSectionService::
    demoteRecoverableCriticalSectionToNoLongerBlockUserWrites(OperationContext* opCtx,
                                                              const NamespaceString& nss) {
    invariant(serverGlobalParams.clusterRole != ClusterRole::None,
              "Demoting the user writes recoverable critical section to also block user writes is "
              "only allowed on sharded clusters");

    LOGV2_DEBUG(6351905,
                3,
                "Demoting user writes recoverable critical section to no longer block user writes",
                "namespace"_attr = nss);

    invariant(nss == UserWritesRecoverableCriticalSectionService::kGlobalUserWritesNamespace);
    invariant(!opCtx->lockState()->isLocked());

    {
        Lock::GlobalLock globalLock(opCtx, MODE_IX);

        const auto bsonObj = findRecoverableCriticalSectionDoc(opCtx, nss);
        // If the critical section is not taken, then we are done.
        if (bsonObj.isEmpty()) {
            LOGV2_DEBUG(
                6351906,
                3,
                "The user writes recoverable critical section was not currently taken, do nothing",
                "namespace"_attr = nss);
            return;
        }

        const auto collCSDoc = UserWriteBlockingCriticalSectionDocument::parse(
            IDLParserContext("DemoteUserWritesCS"), bsonObj);

        // If we are not currently blocking user writes, then we are done.
        if (!collCSDoc.getBlockUserWrites()) {
            LOGV2_DEBUG(6351907,
                        3,
                        "The user writes recoverable critical section was already not blocking "
                        "user writes, do nothing",
                        "namespace"_attr = nss);
            return;
        }

        // Demote the critical section to block also user writes by updating the critical section
        // document. The OpObserver will demote the in-memory CS when reacting to the update event.
        setBlockUserWritesDocumentField(opCtx, nss, false /* blockUserWrites */);
    }

    LOGV2_DEBUG(6351908,
                2,
                "Demoted user writes recoverable critical section to no longer block user writes",
                "namespace"_attr = nss);
}


void UserWritesRecoverableCriticalSectionService::releaseRecoverableCriticalSection(
    OperationContext* opCtx, const NamespaceString& nss) {
    LOGV2_DEBUG(
        6351909, 3, "Releasing user writes recoverable critical section", "namespace"_attr = nss);

    invariant(nss == UserWritesRecoverableCriticalSectionService::kGlobalUserWritesNamespace);
    invariant(!opCtx->lockState()->isLocked());

    {
        Lock::GlobalLock globalLock(opCtx, MODE_IX);

        const auto bsonObj = findRecoverableCriticalSectionDoc(opCtx, nss);

        // If there is no persisted document, then we are done.
        if (bsonObj.isEmpty()) {
            LOGV2_DEBUG(
                6351910,
                3,
                "The user writes recoverable critical section was already released, do nothing",
                "namespace"_attr = nss);
            return;
        }

        const auto collCSDoc = UserWriteBlockingCriticalSectionDocument::parse(
            IDLParserContext("ReleaseUserWritesCS"), bsonObj);

        // Release the critical section by deleting the critical section document. The OpObserver
        // will release the in-memory CS when reacting to the delete event.
        DBDirectClient dbClient(opCtx);
        const auto cmdResponse = dbClient.runCommand([&] {
            write_ops::DeleteCommandRequest deleteOp(
                NamespaceString::kUserWritesCriticalSectionsNamespace);

            deleteOp.setDeletes({[&] {
                write_ops::DeleteOpEntry entry;
                entry.setQ(BSON(UserWriteBlockingCriticalSectionDocument::kNssFieldName
                                << nss.toString()));
                // At most one doc can possibly match the above query.
                entry.setMulti(false);
                return entry;
            }()});

            return deleteOp.serialize({});
        }());

        const auto commandReply = cmdResponse->getCommandReply();
        uassertStatusOK(getStatusFromWriteCommandReply(commandReply));
    }

    LOGV2_DEBUG(
        6351911, 2, "Released user writes recoverable critical section", "namespace"_attr = nss);
}

void UserWritesRecoverableCriticalSectionService::recoverRecoverableCriticalSections(
    OperationContext* opCtx) {
    if (MONGO_unlikely(skipRecoverUserWriteCriticalSections.shouldFail())) {
        return;
    }

    LOGV2_DEBUG(6351912, 2, "Recovering all user writes recoverable critical sections");

    GlobalUserWriteBlockState::get(opCtx)->disableUserShardedDDLBlocking(opCtx);
    GlobalUserWriteBlockState::get(opCtx)->disableUserWriteBlocking(opCtx);

    // Read the persisted critical section documents and restore the state into memory.
    PersistentTaskStore<UserWriteBlockingCriticalSectionDocument> store(
        NamespaceString::kUserWritesCriticalSectionsNamespace);
    store.forEach(opCtx, BSONObj{}, [&opCtx](const UserWriteBlockingCriticalSectionDocument& doc) {
        invariant(doc.getNss().isEmpty());

        if (doc.getBlockNewUserShardedDDL()) {
            GlobalUserWriteBlockState::get(opCtx)->enableUserShardedDDLBlocking(opCtx);
        }

        if (doc.getBlockUserWrites()) {
            GlobalUserWriteBlockState::get(opCtx)->enableUserWriteBlocking(opCtx);
        }

        return true;
    });

    LOGV2_DEBUG(6351913, 2, "Recovered all user writes recoverable critical sections");
}

}  // namespace mongo
