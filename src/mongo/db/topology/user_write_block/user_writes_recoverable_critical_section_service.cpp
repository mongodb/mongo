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


#include "mongo/db/topology/user_write_block/user_writes_recoverable_critical_section_service.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/user_write_block/global_user_write_block_state.h"
#include "mongo/db/topology/user_write_block/replica_set_writes_critical_section_document_gen.h"
#include "mongo/db/topology/user_write_block/user_writes_critical_section_document_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#include <utility>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

namespace {
MONGO_FAIL_POINT_DEFINE(skipRecoverUserWriteCriticalSections);

const auto serviceDecorator =
    ServiceContext::declareDecoration<UserWritesRecoverableCriticalSectionService>();

inline StringData reasonText(const boost::optional<UserWritesBlockReasonEnum>& reason) {
    return idl::serialize(reason.value_or(UserWritesBlockReasonEnum::kUnspecified));
}

inline StringData reasonText(const ReplicaSetWritesBlockReasonEnum& reason) {
    return idl::serialize(reason);
}

BSONObj findRecoverableCriticalSectionDoc(OperationContext* opCtx,
                                          const NamespaceString& collectionNss,
                                          const NamespaceString& nss) {
    DBDirectClient dbClient(opCtx);

    const auto queryNss =
        BSON("_id" << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()));
    FindCommandRequest findRequest{collectionNss};
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
        BSON(UserWriteBlockingCriticalSectionDocument::kNssFieldName
             << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())),
        BSON("$set" << BSON(UserWriteBlockingCriticalSectionDocument::kBlockUserWritesFieldName
                            << blockUserWrites)),
        ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());
}

template <typename DocType, typename CheckExistingFn, typename BuildNewDocFn>
void acquireCriticalSection(OperationContext* opCtx,
                            const NamespaceString& collectionNss,
                            const NamespaceString& nss,
                            LockMode lockMode,
                            StringData logContext,
                            CheckExistingFn&& checkExistingDoc,
                            BuildNewDocFn&& buildNewDoc) {
    invariant(!shard_role_details::getLocker(opCtx)->isLocked());

    {
        Lock::GlobalLock globalLock(opCtx, lockMode);

        const auto bsonObj = findRecoverableCriticalSectionDoc(opCtx, collectionNss, nss);
        if (!bsonObj.isEmpty()) {
            const auto existingDoc = DocType::parse(bsonObj, IDLParserContext(logContext));
            checkExistingDoc(existingDoc);

            repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
            return;
        }

        auto newDoc = buildNewDoc();
        PersistentTaskStore<DocType> store(collectionNss);
        store.add(opCtx, newDoc, ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());
    }
}

template <typename DocType, typename CheckExistingFn>
void releaseCriticalSection(OperationContext* opCtx,
                            const NamespaceString& collectionNss,
                            const NamespaceString& nss,
                            StringData logContext,
                            CheckExistingFn&& checkExistingDoc) {
    invariant(!shard_role_details::getLocker(opCtx)->isLocked());

    {
        Lock::GlobalLock globalLock(opCtx, MODE_IX);

        const auto bsonObj = findRecoverableCriticalSectionDoc(opCtx, collectionNss, nss);
        if (bsonObj.isEmpty()) {
            LOGV2_DEBUG(
                6351910,
                3,
                "The user writes recoverable critical section was already released, do nothing",
                logAttrs(nss));
            repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
            return;
        }

        const auto existingDoc = DocType::parse(bsonObj, IDLParserContext(logContext));
        checkExistingDoc(existingDoc);

        DBDirectClient dbClient(opCtx);
        const auto cmdResponse = dbClient.runCommand([&] {
            write_ops::DeleteCommandRequest deleteOp(collectionNss);
            deleteOp.setDeletes({[&] {
                write_ops::DeleteOpEntry entry;
                entry.setQ(BSON("_id" << NamespaceStringUtil::serialize(
                                    nss, SerializationContext::stateDefault())));
                entry.setMulti(false);
                return entry;
            }()});
            return deleteOp.serialize();
        }());

        const auto commandReply = cmdResponse->getCommandReply();
        uassertStatusOK(getStatusFromWriteCommandReply(commandReply));
    }
}
}  // namespace

const NamespaceString UserWritesRecoverableCriticalSectionService::kGlobalUserWritesNamespace =
    NamespaceString::kEmpty;

const NamespaceString UserWritesRecoverableCriticalSectionService::kBlockReplicaSetWritesNamespace =
    NamespaceString::kEmpty;

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
        "UserWritesRecoverableCriticalSectionService", {"ShardingInitializationMongoDRegistry"});

bool UserWritesRecoverableCriticalSectionService::shouldRegisterReplicaSetAwareService() const {
    return serverGlobalParams.clusterRole.has(ClusterRole::None) ||
        serverGlobalParams.clusterRole.has(ClusterRole::ShardServer);
}

void UserWritesRecoverableCriticalSectionService::
    acquireRecoverableCriticalSectionBlockingUserWrites(
        OperationContext* opCtx,
        const NamespaceString& nss,
        boost::optional<UserWritesBlockReasonEnum> reason) {

    const auto blockShardedDDL = false;
    const auto blockUserWrites = true;

    LOGV2_DEBUG(12096700,
                3,
                "Acquiring user writes recoverable critical section",
                logAttrs(nss),
                "blockShardedDDL"_attr = blockShardedDDL,
                "blockUserWrites"_attr = blockUserWrites,
                "reason"_attr = reasonText(reason));

    invariant(nss == kGlobalUserWritesNamespace);

    acquireCriticalSection<UserWriteBlockingCriticalSectionDocument>(
        opCtx,
        NamespaceString::kUserWritesCriticalSectionsNamespace,
        nss,
        MODE_X,
        "AcquireUserWritesCS"_sd,
        [&](const UserWriteBlockingCriticalSectionDocument& collCSDoc) {
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
                                  << ", current: " << collCSDoc.getBlockUserWrites(),
                    !blockUserWrites || collCSDoc.getBlockUserWrites() == blockUserWrites);

            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << "Cannot acquire user writes critical section with different "
                                     "options than the already existing one. reason: "
                                  << reasonText(reason) << ", current: "
                                  << reasonText(collCSDoc.getBlockUserWritesReason()),
                    collCSDoc.getBlockUserWritesReason() == reason);
        },
        [&]() {
            UserWriteBlockingCriticalSectionDocument newDoc(nss);
            newDoc.setBlockNewUserShardedDDL(blockShardedDDL);
            newDoc.setBlockUserWrites(blockUserWrites);
            newDoc.setBlockUserWritesReason(reason);
            return newDoc;
        });

    LOGV2_DEBUG(12096701,
                2,
                "Acquired user writes recoverable critical section",
                logAttrs(nss),
                "blockShardedDDL"_attr = "false",
                "blockUserWrites"_attr = "true");
}

void UserWritesRecoverableCriticalSectionService::
    acquireRecoverableCriticalSectionBlockNewShardedDDL(OperationContext* opCtx,
                                                        const NamespaceString& nss) {

    const auto blockShardedDDL = true;
    const auto blockUserWrites = false;

    invariant(!serverGlobalParams.clusterRole.has(ClusterRole::None),
              "Acquiring the user writes recoverable critical section blocking only sharded DDL is "
              "only allowed on sharded clusters");

    invariant(nss == kGlobalUserWritesNamespace);

    LOGV2_DEBUG(12096702,
                3,
                "Acquiring user writes recoverable critical section",
                logAttrs(nss),
                "blockShardedDDL"_attr = blockShardedDDL,
                "blockUserWrites"_attr = blockUserWrites);

    acquireCriticalSection<UserWriteBlockingCriticalSectionDocument>(
        opCtx,
        NamespaceString::kUserWritesCriticalSectionsNamespace,
        nss,
        MODE_IX,
        "AcquireUserWritesCS"_sd,
        [&](const UserWriteBlockingCriticalSectionDocument& collCSDoc) {
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
                                  << ", current: " << collCSDoc.getBlockUserWrites(),
                    !blockUserWrites || collCSDoc.getBlockUserWrites() == blockUserWrites);
            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << "Cannot acquire user writes critical section with different "
                                     "options than the already existing one. reason: "
                                  << reasonText(boost::none) << ", current: "
                                  << reasonText(collCSDoc.getBlockUserWritesReason()),
                    collCSDoc.getBlockUserWritesReason() == boost::none);
        },
        [&]() {
            UserWriteBlockingCriticalSectionDocument newDoc(nss);
            newDoc.setBlockNewUserShardedDDL(blockShardedDDL);
            newDoc.setBlockUserWrites(blockUserWrites);
            return newDoc;
        });
    LOGV2_DEBUG(12096703,
                2,
                "Acquired user writes recoverable critical section",
                logAttrs(nss),
                "blockShardedDDL"_attr = blockShardedDDL,
                "blockUserWrites"_attr = blockUserWrites);
}

void UserWritesRecoverableCriticalSectionService::
    promoteRecoverableCriticalSectionToBlockUserWrites(OperationContext* opCtx,
                                                       const NamespaceString& nss) {
    invariant(!serverGlobalParams.clusterRole.has(ClusterRole::None),
              "Promoting the user writes recoverable critical section to also block user writes is "
              "only allowed on sharded clusters");

    LOGV2_DEBUG(6351902,
                3,
                "Promoting user writes recoverable critical section to also block reads",
                logAttrs(nss));

    invariant(nss == UserWritesRecoverableCriticalSectionService::kGlobalUserWritesNamespace);
    invariant(!shard_role_details::getLocker(opCtx)->isLocked());

    {
        // Take the GlobalLock in MODE_X in order to ensure that any ongoing writes have completed
        // before starting to block new writes.
        Lock::GlobalLock globalLock(opCtx, MODE_X);

        const auto bsonObj = findRecoverableCriticalSectionDoc(
            opCtx, NamespaceString::kUserWritesCriticalSectionsNamespace, nss);
        uassert(ErrorCodes::IllegalOperation,
                "Cannot promote user writes critical section to block user writes if critical "
                "section document not persisted first.",
                !bsonObj.isEmpty());

        const auto collCSDoc = UserWriteBlockingCriticalSectionDocument::parse(
            bsonObj, IDLParserContext("PromoteUserWritesCS"));

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
                        logAttrs(nss));

            repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);

            return;
        }

        // Promote the critical section to block also user writes by updating the critical section
        // document. The OpObserver will promote the in-memory CS when reacting to the update event.
        setBlockUserWritesDocumentField(opCtx, nss, true /* blockUserWrites */);
    }

    LOGV2_DEBUG(6351904,
                2,
                "Promoted user writes recoverable critical section to also block user writes",
                logAttrs(nss));
}

void UserWritesRecoverableCriticalSectionService::
    demoteRecoverableCriticalSectionToNoLongerBlockUserWrites(OperationContext* opCtx,
                                                              const NamespaceString& nss) {
    invariant(!serverGlobalParams.clusterRole.has(ClusterRole::None),
              "Demoting the user writes recoverable critical section to also block user writes is "
              "only allowed on sharded clusters");

    LOGV2_DEBUG(6351905,
                3,
                "Demoting user writes recoverable critical section to no longer block user writes",
                logAttrs(nss));

    invariant(nss == UserWritesRecoverableCriticalSectionService::kGlobalUserWritesNamespace);
    invariant(!shard_role_details::getLocker(opCtx)->isLocked());

    {
        Lock::GlobalLock globalLock(opCtx, MODE_IX);

        const auto bsonObj = findRecoverableCriticalSectionDoc(
            opCtx, NamespaceString::kUserWritesCriticalSectionsNamespace, nss);
        // If the critical section is not taken, then we are done.
        if (bsonObj.isEmpty()) {
            LOGV2_DEBUG(
                6351906,
                3,
                "The user writes recoverable critical section was not currently taken, do nothing",
                logAttrs(nss));

            repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);

            return;
        }

        const auto collCSDoc = UserWriteBlockingCriticalSectionDocument::parse(
            bsonObj, IDLParserContext("DemoteUserWritesCS"));

        // If we are not currently blocking user writes, then we are done.
        if (!collCSDoc.getBlockUserWrites()) {
            LOGV2_DEBUG(6351907,
                        3,
                        "The user writes recoverable critical section was already not blocking "
                        "user writes, do nothing",
                        logAttrs(nss));

            repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);

            return;
        }

        // Demote the critical section to block also user writes by updating the critical section
        // document. The OpObserver will demote the in-memory CS when reacting to the update event.
        setBlockUserWritesDocumentField(opCtx, nss, false /* blockUserWrites */);
    }

    LOGV2_DEBUG(6351908,
                2,
                "Demoted user writes recoverable critical section to no longer block user writes",
                logAttrs(nss));
}


void UserWritesRecoverableCriticalSectionService::
    releaseRecoverableCriticalSectionBlockingUserWrites(
        OperationContext* opCtx,
        const NamespaceString& nss,
        boost::optional<UserWritesBlockReasonEnum> reason) {
    LOGV2_DEBUG(6351909,
                3,
                "Releasing user writes recoverable critical section",
                logAttrs(nss),
                "reason"_attr = reasonText(reason));

    invariant(nss == UserWritesRecoverableCriticalSectionService::kGlobalUserWritesNamespace ||
              nss == UserWritesRecoverableCriticalSectionService::kBlockReplicaSetWritesNamespace);
    invariant(!shard_role_details::getLocker(opCtx)->isLocked());

    releaseCriticalSection<UserWriteBlockingCriticalSectionDocument>(
        opCtx,
        NamespaceString::kUserWritesCriticalSectionsNamespace,
        nss,
        "ReleaseUserWritesCS"_sd,
        [&](const UserWriteBlockingCriticalSectionDocument& collCSDoc) {
            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << "Cannot release user writes critical section with different "
                                     "reason than the already existing one. reason: "
                                  << reasonText(reason) << ", current: "
                                  << reasonText(collCSDoc.getBlockUserWritesReason()),
                    collCSDoc.getBlockUserWritesReason() == reason);
        });

    LOGV2_DEBUG(6351911, 2, "Released user writes recoverable critical section", logAttrs(nss));
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
            GlobalUserWriteBlockState::get(opCtx)->enableUserWriteBlocking(
                opCtx,
                doc.getBlockUserWritesReason().value_or(UserWritesBlockReasonEnum::kUnspecified));
        }

        return true;
    });

    // Recover the persisted replica set writes critical section documents and restore the
    // state into memory
    PersistentTaskStore<ReplicaSetWriteBlockingCriticalSectionDocument> replicaSetWritesStore(
        NamespaceString::kReplicaSetWritesCriticalSectionsNamespace);
    replicaSetWritesStore.forEach(
        opCtx, BSONObj{}, [&opCtx](const ReplicaSetWriteBlockingCriticalSectionDocument& doc) {
            invariant(doc.getNss().isEmpty());
            // TODO(SERVER-120970): restore the state into memory
            return true;
        });

    LOGV2_DEBUG(6351913,
                2,
                "Recovered both user writes and replica set writes recoverable critical sections");
}

void UserWritesRecoverableCriticalSectionService::
    acquireRecoverableCriticalSectionBlockingReplicaSetWrites(
        OperationContext* opCtx,
        const NamespaceString& nss,
        bool allowDeletions,
        ReplicaSetWritesBlockReasonEnum reason) {

    LOGV2_DEBUG(12096704,
                3,
                "Acquiring replica set writes recoverable critical section",
                logAttrs(nss),
                "enabled"_attr = "true",
                "allowDeletions"_attr = "false",
                "reason"_attr = reasonText(reason));

    invariant(nss == kBlockReplicaSetWritesNamespace);

    acquireCriticalSection<ReplicaSetWriteBlockingCriticalSectionDocument>(
        opCtx,
        NamespaceString::kReplicaSetWritesCriticalSectionsNamespace,
        nss,
        MODE_X,
        "AcquireReplicaSetWritesCS"_sd,
        [&](const ReplicaSetWriteBlockingCriticalSectionDocument& collCSDoc) {
            uassert(ErrorCodes::IllegalOperation,
                    str::stream()
                        << "Cannot acquire replica set writes critical section with different "
                           "options than the already existing one. enabled: true, current: "
                        << collCSDoc.getEnabled(),
                    collCSDoc.getEnabled());

            uassert(ErrorCodes::IllegalOperation,
                    str::stream()
                        << "Cannot acquire replica set writes critical section with different "
                           "options than the already existing one. allowDeletions: "
                        << allowDeletions << ", current: " << collCSDoc.getAllowDeletions(),
                    collCSDoc.getAllowDeletions() == allowDeletions);

            uassert(ErrorCodes::IllegalOperation,
                    str::stream()
                        << "Cannot acquire replica set writes critical section with different "
                           "options than the already existing one. reason: "
                        << reasonText(reason)
                        << ", current: " << reasonText(collCSDoc.getReplicaSetWritesBlockReason()),
                    collCSDoc.getReplicaSetWritesBlockReason() == reason);
        },
        [&]() {
            ReplicaSetWriteBlockingCriticalSectionDocument newDoc(nss);
            newDoc.setEnabled(true);
            newDoc.setAllowDeletions(allowDeletions);
            newDoc.setReplicaSetWritesBlockReason(reason);
            return newDoc;
        });

    LOGV2_DEBUG(12096705,
                2,
                "Acquired replica set writes recoverable critical section",
                logAttrs(nss),
                "enabled"_attr = "true",
                "allowDeletions"_attr = allowDeletions,
                "reason"_attr = reasonText(reason));
}

void UserWritesRecoverableCriticalSectionService::
    releaseRecoverableCriticalSectionBlockingReplicaSetWrites(
        OperationContext* opCtx,
        const NamespaceString& nss,
        ReplicaSetWritesBlockReasonEnum reason) {

    LOGV2_DEBUG(
        12096406, 3, "Releasing replica set writes recoverable critical section", logAttrs(nss));

    invariant(nss == kBlockReplicaSetWritesNamespace);

    releaseCriticalSection<ReplicaSetWriteBlockingCriticalSectionDocument>(
        opCtx,
        NamespaceString::kReplicaSetWritesCriticalSectionsNamespace,
        nss,
        "ReleaseReplicaSetWritesCS"_sd,
        [&](const ReplicaSetWriteBlockingCriticalSectionDocument& doc) {
            uassert(ErrorCodes::IllegalOperation,
                    "Cannot release with different reason",
                    doc.getReplicaSetWritesBlockReason() == reason);
        });

    LOGV2_DEBUG(
        12096407, 2, "Released replica set writes recoverable critical section", logAttrs(nss));
}
}  // namespace mongo
