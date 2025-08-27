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


#include "mongo/db/global_catalog/ddl/sharding_recovery_service.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_critical_section_document_gen.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_runtime.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_state.h"
#include "mongo/db/local_catalog/shard_role_catalog/database_sharding_runtime.h"
#include "mongo/db/local_catalog/shard_role_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/write_concern.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/namespace_string_util.h"

#include <algorithm>
#include <array>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

namespace {
const auto serviceDecorator = ServiceContext::declareDecoration<ShardingRecoveryService>();

}  // namespace

ShardingRecoveryService::FilteringMetadataClearer::FilteringMetadataClearer(
    bool includeStepsForNamespaceDropped)
    : _includeStepsForNamespaceDropped(includeStepsForNamespaceDropped) {}

void ShardingRecoveryService::FilteringMetadataClearer::operator()(
    OperationContext* opCtx, const NamespaceString& nssBeingReleased) const {
    if (nssBeingReleased.isNamespaceAlwaysUntracked()) {
        return;
    }

    Lock::DBLock dbLock(opCtx, nssBeingReleased.dbName(), MODE_IX);

    if (nssBeingReleased.isDbOnly()) {
        auto scopedDsr = DatabaseShardingRuntime::assertDbLockedAndAcquireExclusive(
            opCtx, nssBeingReleased.dbName());
        scopedDsr->clearDbInfo_DEPRECATED(opCtx);
        return;
    }

    Lock::CollectionLock collLock{opCtx, nssBeingReleased, MODE_IX};

    auto scopedCsr = CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(
        opCtx, nssBeingReleased);
    if (_includeStepsForNamespaceDropped) {
        scopedCsr->clearFilteringMetadataForDroppedCollection(opCtx);
    } else {
        scopedCsr->clearFilteringMetadata(opCtx);
    }
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
    bool clearDbMetadata) {
    LOGV2_DEBUG(5656600,
                3,
                "Acquiring recoverable critical section blocking writes",
                logAttrs(nss),
                "reason"_attr = reason,
                "writeConcern"_attr = writeConcern);

    tassert(7032360,
            fmt::format("Can't acquire recoverable critical section for collection '{}' with "
                        "reason '{}' while holding locks",
                        nss.toStringForErrorMsg(),
                        reason.toString()),
            !shard_role_details::getLocker(opCtx)->isLocked());

    {
        Lock::GlobalLock lk(opCtx, MODE_IX);
        boost::optional<Lock::DBLock> dbLock;
        boost::optional<Lock::CollectionLock> collLock;
        if (nss.isDbOnly()) {
            tassert(8096300,
                    "Cannot acquire critical section on the config database",
                    !nss.isConfigDB());
            dbLock.emplace(opCtx, nss.dbName(), MODE_S);
        } else {
            // Take the 'config' database lock in mode IX to prevent lock upgrade when we later
            // write to kCollectionCriticalSectionsNamespace.
            dbLock.emplace(opCtx, nss.dbName(), nss.isConfigDB() ? MODE_IX : MODE_IS);

            collLock.emplace(opCtx, nss, MODE_S);
        }

        DBDirectClient dbClient(opCtx);
        FindCommandRequest findRequest{NamespaceString::kCollectionCriticalSectionsNamespace};
        findRequest.setFilter(
            BSON(CollectionCriticalSectionDocument::kNssFieldName
                 << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())));
        auto cursor = dbClient.find(std::move(findRequest));

        // if there is a doc with the same nss -> in order to not fail it must have the same
        // reason
        if (cursor->more()) {
            const auto bsonObj = cursor->next();
            const auto collCSDoc = CollectionCriticalSectionDocument::parse(
                bsonObj, IDLParserContext("AcquireRecoverableCSBW"));

            tassert(7032368,
                    fmt::format("Trying to acquire a  critical section blocking writes for "
                                "namespace '{}' and reason '{}' but it is already taken by another "
                                "operation with different reason '{}'",
                                nss.toStringForErrorMsg(),
                                reason.toString(),
                                collCSDoc.getReason().toString()),
                    collCSDoc.getReason().woCompare(reason) == 0);

            LOGV2_DEBUG(5656601,
                        3,
                        "The recoverable critical section was already acquired to block "
                        "writes, do nothing",
                        logAttrs(nss),
                        "reason"_attr = reason,
                        "writeConcern"_attr = writeConcern);

            return;
        }

        // TODO(SERVER-100328): remove after 9.0 is branched.
        // If the storage snapshot is reused between the 'find' above and the 'insert' below, the
        // operation may unnecessarily fail if a collMod is run on the
        // kCollectionCriticalSectionsNamespace during FCV upgrade.
        shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();

        // The collection critical section is not taken, try to acquire it.

        // The following code will try to add a doc to config.criticalCollectionSections:
        // - If everything goes well, the shard server op observer will acquire the in-memory
        // CS.
        // - Otherwise this call will fail and the CS won't be taken (neither persisted nor
        // in-mem)
        CollectionCriticalSectionDocument newDoc(nss, reason, false /* blockReads */);
        newDoc.setClearDbInfo(clearDbMetadata);

        const auto commandResponse = dbClient.runCommand([&] {
            write_ops::InsertCommandRequest insertOp(
                NamespaceString::kCollectionCriticalSectionsNamespace);
            insertOp.setDocuments({newDoc.toBSON()});
            return insertOp.serialize();
        }());

        const auto commandReply = commandResponse->getCommandReply();
        uassertStatusOK(getStatusFromWriteCommandReply(commandReply));

        BatchedCommandResponse batchedResponse;
        std::string unusedErrmsg;
        batchedResponse.parseBSON(commandReply, &unusedErrmsg);
        tassert(7032369,
                fmt::format(
                    "Insert did not add any doc to collection '{}' for namespace '{}' "
                    "and reason '{}'",
                    nss.toStringForErrorMsg(),
                    reason.toString(),
                    NamespaceString::kCollectionCriticalSectionsNamespace.toStringForErrorMsg()),
                batchedResponse.getN() > 0);
    }

    WriteConcernResult ignoreResult;
    const auto latestOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    uassertStatusOK(waitForWriteConcern(opCtx, latestOpTime, writeConcern, &ignoreResult));

    LOGV2_DEBUG(5656602,
                2,
                "Acquired recoverable critical section blocking writes",
                logAttrs(nss),
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
                logAttrs(nss),
                "reason"_attr = reason,
                "writeConcern"_attr = writeConcern);

    tassert(7032364,
            fmt::format("Can't promote recoverable critical section for collection '{}' with "
                        "reason '{}' while holding locks",
                        nss.toStringForErrorMsg(),
                        reason.toString()),
            !shard_role_details::getLocker(opCtx)->isLocked());

    {
        boost::optional<Lock::DBLock> dbLock;
        boost::optional<Lock::CollectionLock> collLock;
        if (nss.isDbOnly()) {
            dbLock.emplace(opCtx, nss.dbName(), MODE_X);
        } else {
            dbLock.emplace(opCtx, nss.dbName(), MODE_IX);
            collLock.emplace(opCtx, nss, MODE_X);
        }

        DBDirectClient dbClient(opCtx);
        FindCommandRequest findRequest{NamespaceString::kCollectionCriticalSectionsNamespace};
        findRequest.setFilter(
            BSON(CollectionCriticalSectionDocument::kNssFieldName
                 << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())));
        auto cursor = dbClient.find(std::move(findRequest));

        tassert(7032361,
                fmt::format(
                    "Trying to acquire a critical section blocking reads for namespace '{}' and "
                    "reason '{}' but the critical section wasn't acquired first blocking writers.",
                    nss.toStringForErrorMsg(),
                    reason.toString()),
                cursor->more());
        BSONObj bsonObj = cursor->next();
        const auto collCSDoc = CollectionCriticalSectionDocument::parse(
            bsonObj, IDLParserContext("AcquireRecoverableCSBR"));

        tassert(7032362,
                fmt::format(
                    "Trying to acquire a critical section blocking reads for namespace '{}' and "
                    "reason "
                    "'{}' but it is already taken by another operation with different reason '{}'",
                    nss.toStringForErrorMsg(),
                    reason.toString(),
                    collCSDoc.getReason().toString()),
                collCSDoc.getReason().woCompare(reason) == 0);

        // if there is a document with the same nss, reason and blocking reads -> do nothing,
        // the CS is already taken!
        if (collCSDoc.getBlockReads()) {
            LOGV2_DEBUG(5656604,
                        3,
                        "The recoverable critical section was already promoted to also block "
                        "reads, do nothing",
                        logAttrs(nss),
                        "reason"_attr = reason,
                        "writeConcern"_attr = writeConcern);
            return;
        }

        // TODO(SERVER-100328): remove after 9.0 is branched.
        // If the storage snapshot is reused between the 'find' above and the 'update' below, the
        // operation may unnecessarily fail when a collMod is run on the
        // kCollectionCriticalSectionsNamespace during FCV upgrade.
        shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();

        // The CS is in the catch-up phase, try to advance it to the commit phase.

        // The following code will try to update a doc from config.criticalCollectionSections:
        // - If everything goes well, the shard server op observer will advance the in-memory CS
        // to the
        //   commit phase (blocking readers).
        // - Otherwise this call will fail and the CS won't be advanced (neither persisted nor
        // in-mem)
        auto commandResponse = dbClient.runCommand([&] {
            const auto query =
                BSON(CollectionCriticalSectionDocument::kNssFieldName
                     << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())
                     << CollectionCriticalSectionDocument::kReasonFieldName << reason);
            const auto update = BSON(
                "$set" << BSON(CollectionCriticalSectionDocument::kBlockReadsFieldName << true));

            write_ops::UpdateCommandRequest updateOp(
                NamespaceString::kCollectionCriticalSectionsNamespace);
            auto updateModification = write_ops::UpdateModification::parseFromClassicUpdate(update);
            write_ops::UpdateOpEntry updateEntry(query, updateModification);
            updateOp.setUpdates({updateEntry});

            return updateOp.serialize();
        }());

        const auto commandReply = commandResponse->getCommandReply();
        uassertStatusOK(getStatusFromWriteCommandReply(commandReply));

        BatchedCommandResponse batchedResponse;
        std::string unusedErrmsg;
        batchedResponse.parseBSON(commandReply, &unusedErrmsg);
        tassert(
            7032363,
            fmt::format("Update did not modify any doc from collection '{}' for namespace '{}' "
                        "and reason '{}'",
                        NamespaceString::kCollectionCriticalSectionsNamespace.toStringForErrorMsg(),
                        nss.toStringForErrorMsg(),
                        reason.toString()),
            batchedResponse.getNModified() > 0);
    }

    WriteConcernResult ignoreResult;
    const auto latestOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    uassertStatusOK(waitForWriteConcern(opCtx, latestOpTime, writeConcern, &ignoreResult));

    LOGV2_DEBUG(5656605,
                2,
                "Promoted recoverable critical section to also block reads",
                logAttrs(nss),
                "reason"_attr = reason,
                "writeConcern"_attr = writeConcern);
}

void ShardingRecoveryService::releaseRecoverableCriticalSection(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& reason,
    const WriteConcernOptions& writeConcern,
    const BeforeReleasingCustomAction& beforeReleasingAction,
    bool throwIfReasonDiffers) {
    LOGV2_DEBUG(5656606,
                3,
                "Releasing recoverable critical section",
                logAttrs(nss),
                "reason"_attr = reason,
                "writeConcern"_attr = writeConcern);

    tassert(7032365,
            fmt::format("Can't release recoverable critical section for collection '{}' with "
                        "reason '{}' while holding locks",
                        nss.toStringForErrorMsg(),
                        reason.toString()),
            !shard_role_details::getLocker(opCtx)->isLocked());

    {
        Lock::DBLock dbLock{opCtx, nss.dbName(), MODE_IX};

        boost::optional<Lock::CollectionLock> collLock;
        if (!nss.isDbOnly()) {
            collLock.emplace(opCtx, nss, MODE_IX);
        }

        DBDirectClient dbClient(opCtx);

        const auto queryNss =
            BSON(CollectionCriticalSectionDocument::kNssFieldName
                 << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()));
        FindCommandRequest findRequest{NamespaceString::kCollectionCriticalSectionsNamespace};
        findRequest.setFilter(queryNss);
        auto cursor = dbClient.find(std::move(findRequest));

        // if there is no document with the same nss -> do nothing!
        if (!cursor->more()) {
            LOGV2_DEBUG(5656607,
                        3,
                        "The recoverable critical section was already released, do nothing",
                        logAttrs(nss),
                        "reason"_attr = reason,
                        "writeConcern"_attr = writeConcern);
            return;
        }

        BSONObj bsonObj = cursor->next();
        const auto collCSDoc = CollectionCriticalSectionDocument::parse(
            bsonObj, IDLParserContext("ReleaseRecoverableCS"));

        const bool isDifferentReason = collCSDoc.getReason().woCompare(reason) != 0;
        if (MONGO_unlikely(!throwIfReasonDiffers && isDifferentReason)) {
            LOGV2_DEBUG(7019701,
                        2,
                        "Impossible to release recoverable critical section since it was taken by "
                        "another operation with different reason",
                        logAttrs(nss),
                        "callerReason"_attr = reason,
                        "storedReason"_attr = collCSDoc.getReason(),
                        "writeConcern"_attr = writeConcern);
            return;
        }

        tassert(7032366,
                fmt::format("Trying to release a critical for namespace '{}' and reason '{}' but "
                            "it is already taken by another operation with different reason '{}'",
                            nss.toStringForErrorMsg(),
                            reason.toString(),
                            collCSDoc.getReason().toString()),
                !isDifferentReason);

        // The collection critical section is taken (in any phase), perform the custom action then
        // try to release it.

        beforeReleasingAction(opCtx, nss);

        // TODO(SERVER-100328): remove after 9.0 is branched.
        // If the storage snapshot is reused between the 'find' above and the 'delete' below, the
        // operation may unnecessarily fail if a collMod is run on the
        // kCollectionCriticalSectionsNamespace during FCV upgrade.
        shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();

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

            return deleteOp.serialize();
        }());

        const auto commandReply = commandResponse->getCommandReply();
        uassertStatusOK(getStatusFromWriteCommandReply(commandReply));

        BatchedCommandResponse batchedResponse;
        std::string unusedErrmsg;
        batchedResponse.parseBSON(commandReply, &unusedErrmsg);
        tassert(
            7032367,
            fmt::format("Delete did not remove any doc from collection '{}' for namespace '{}' "
                        "and reason '{}'",
                        NamespaceString::kCollectionCriticalSectionsNamespace.toStringForErrorMsg(),
                        nss.toStringForErrorMsg(),
                        reason.toString()),
            batchedResponse.getN() > 0);
    }

    WriteConcernResult ignoreResult;
    const auto latestOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    uassertStatusOK(waitForWriteConcern(opCtx, latestOpTime, writeConcern, &ignoreResult));

    LOGV2_DEBUG(5656608,
                2,
                "Released recoverable critical section",
                logAttrs(nss),
                "reason"_attr = reason,
                "writeConcern"_attr = writeConcern);
}

void ShardingRecoveryService::onReplicationRollback(
    OperationContext* opCtx, const std::set<NamespaceString>& rollbackNamespaces) {
    // TODO (SERVER-91926): Move these recovery services to onConsistentDataAvailable interface once
    // it offers a way to know which nss were impacted by the rollback event.

    static const std::array kShardingRecoveryTriggerNamespaces{
        NamespaceString::kConfigShardCatalogDatabasesNamespace,
        NamespaceString::kCollectionCriticalSectionsNamespace,
    };
    if (std::ranges::any_of(kShardingRecoveryTriggerNamespaces, [&](const NamespaceString& nss) {
            return rollbackNamespaces.contains(nss);
        })) {
        _resetInMemoryStates(opCtx);
        _recoverDatabaseShardingState(opCtx);
        _recoverRecoverableCriticalSections(opCtx);
    }

    // If writes to config.cache.* have been rolled back, interrupt the SSCCL to ensure secondary
    // waits for replication do not use incorrect opTimes.
    if (std::ranges::any_of(rollbackNamespaces, [](const NamespaceString& nss) {
            return nss.isConfigDotCacheDotChunks();
        })) {
        FilteringMetadataCache::get(opCtx)->onReplicationRollback();
    }
}

void ShardingRecoveryService::onConsistentDataAvailable(OperationContext* opCtx,
                                                        bool isMajority,
                                                        bool isRollback) {
    // TODO (SERVER-91505): Determine if we should reload in-memory states on rollback.
    if (isRollback) {
        return;
    }

    _resetInMemoryStates(opCtx);
    _recoverDatabaseShardingState(opCtx);
    _recoverRecoverableCriticalSections(opCtx);
}

void ShardingRecoveryService::_recoverRecoverableCriticalSections(OperationContext* opCtx) {
    LOGV2_DEBUG(5604000, 2, "Recovering all recoverable critical sections");

    Lock::DBLockSkipOptions dbLockOptions{.explicitIntent =
                                              rss::consensus::IntentRegistry::Intent::Read};

    // Map the critical sections that are on disk to memory
    PersistentTaskStore<CollectionCriticalSectionDocument> store(
        NamespaceString::kCollectionCriticalSectionsNamespace);
    store.forEach(
        opCtx, BSONObj{}, [&opCtx, &dbLockOptions](const CollectionCriticalSectionDocument& doc) {
            const auto& nss = doc.getNss();
            if (nss.isDbOnly()) {
                Lock::DBLock dbLock{opCtx, nss.dbName(), MODE_X, Date_t::max(), dbLockOptions};
                auto scopedDsr =
                    DatabaseShardingRuntime::assertDbLockedAndAcquireExclusive(opCtx, nss.dbName());
                scopedDsr->enterCriticalSectionCatchUpPhase(doc.getReason());
                if (doc.getBlockReads()) {
                    scopedDsr->enterCriticalSectionCommitPhase(doc.getReason());
                }
            } else {
                Lock::DBLock dbLock{opCtx, nss.dbName(), MODE_IX, Date_t::max(), dbLockOptions};
                Lock::CollectionLock collLock{opCtx, nss, MODE_X};
                auto scopedCsr =
                    CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx,
                                                                                         nss);
                scopedCsr->enterCriticalSectionCatchUpPhase(doc.getReason());
                if (doc.getBlockReads()) {
                    scopedCsr->enterCriticalSectionCommitPhase(doc.getReason());
                }
            }

            return true;
        });

    LOGV2_DEBUG(5604001, 2, "Recovered all recoverable critical sections");
}

void ShardingRecoveryService::_recoverDatabaseShardingState(OperationContext* opCtx) {
    if (!feature_flags::gShardAuthoritativeDbMetadataDDL.isEnabled(
            VersionContext::getDecoration(opCtx),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        return;
    }

    LOGV2_DEBUG(9813601, 2, "Recovering DatabaseShardingState from the shard catalog");

    Lock::DBLockSkipOptions dbLockOptions{.explicitIntent =
                                              rss::consensus::IntentRegistry::Intent::Read};

    PersistentTaskStore<DatabaseType> store(NamespaceString::kConfigShardCatalogDatabasesNamespace);
    store.forEach(opCtx, BSONObj{}, [&opCtx, &dbLockOptions](const DatabaseType& dbMetadata) {
        const auto dbName = dbMetadata.getDbName();
        Lock::DBLock dbLock{opCtx, dbName, MODE_X, Date_t::max(), dbLockOptions};
        auto scopedDsr = DatabaseShardingRuntime::assertDbLockedAndAcquireExclusive(opCtx, dbName);

        auto reason = BSON(
            "shardingRecoveryService"
            << "setDbMetadata"
            << "dbName"
            << DatabaseNameUtil::serialize(dbName, SerializationContext::stateCommandRequest()));

        scopedDsr->enterCriticalSectionCatchUpPhase(reason);
        scopedDsr->enterCriticalSectionCommitPhase(reason);

        scopedDsr->setDbMetadata(opCtx, dbMetadata);

        scopedDsr->exitCriticalSection(reason);

        return true;
    });

    LOGV2_DEBUG(9813602, 2, "Recovered the DatabaseShardingState from the shard catalog");
}

void ShardingRecoveryService::_resetInMemoryStates(OperationContext* opCtx) {
    LOGV2_DEBUG(10371108, 2, "Resetting all in-memory sharding states");

    Lock::DBLockSkipOptions dbLockOptions{.explicitIntent =
                                              rss::consensus::IntentRegistry::Intent::Read};

    // Release all in-memory critical sections
    for (const auto& nss : CollectionShardingState::getCollectionNames(opCtx)) {
        Lock::DBLock dbLock{opCtx, nss.dbName(), MODE_IX, Date_t::max(), dbLockOptions};
        Lock::CollectionLock collLock{opCtx, nss, MODE_IX};
        auto scopedCsr =
            CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx, nss);
        scopedCsr->exitCriticalSectionNoChecks();
    }

    for (const auto& dbName : DatabaseShardingState::getDatabaseNames(opCtx)) {
        Lock::DBLock dbLock{opCtx, dbName, MODE_X, Date_t::max(), dbLockOptions};

        auto scopedDsr = DatabaseShardingRuntime::assertDbLockedAndAcquireExclusive(opCtx, dbName);
        scopedDsr->exitCriticalSectionNoChecks();

        auto reason = BSON(
            "shardingRecoveryService"
            << "clearDbMetadata"
            << "dbName"
            << DatabaseNameUtil::serialize(dbName, SerializationContext::stateCommandRequest()));

        scopedDsr->enterCriticalSectionCatchUpPhase(reason);
        scopedDsr->enterCriticalSectionCommitPhase(reason);

        scopedDsr->clearDbMetadata();

        scopedDsr->exitCriticalSection(reason);
    }

    LOGV2_DEBUG(10371109, 2, "Reset all in-memory sharding states");
}

}  // namespace mongo
