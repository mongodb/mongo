/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/sharding_environment/shard_server_op_observer.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/cannot_implicitly_create_collection_info.h"
#include "mongo/db/global_catalog/ddl/sharding_migration_critical_section.h"
#include "mongo/db/global_catalog/ddl/sharding_recovery_service.h"
#include "mongo/db/global_catalog/type_shard_collection.h"
#include "mongo/db/global_catalog/type_shard_identity.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_critical_section_document_gen.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_metadata.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_runtime.h"
#include "mongo/db/local_catalog/shard_role_catalog/database_sharding_runtime.h"
#include "mongo/db/local_catalog/shard_role_catalog/operation_sharding_state.h"
#include "mongo/db/local_catalog/shard_role_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/local_catalog/shard_role_catalog/type_oplog_catalog_metadata_gen.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/replica_set_endpoint_sharding_state.h"
#include "mongo/db/s/balancer_stats_registry.h"
#include "mongo/db/s/migration_source_manager.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/db/sharding_environment/sharding_initialization_mongod.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/topology/shard_identity_rollback_notifier.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/namespace_string_util.h"

#include <memory>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

/**
 * Used to notify the catalog cache loader of a new placement version and invalidate the in-memory
 * routing table cache once the oplog updates are committed and become visible.
 */
class CollectionPlacementVersionLogOpHandler final : public RecoveryUnit::Change {
public:
    CollectionPlacementVersionLogOpHandler(const NamespaceString& nss, bool droppingCollection)
        : _nss(nss), _droppingCollection(droppingCollection) {}

    void commit(OperationContext* opCtx, boost::optional<Timestamp> commitTime) noexcept override {
        invariant(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(_nss, MODE_IX));
        invariant(commitTime, "Invalid commit time");

        FilteringMetadataCache::get(opCtx)->notifyOfCollectionRefreshEndMarkerSeen(_nss,
                                                                                   *commitTime);

        // Force subsequent uses of the namespace to refresh the filtering metadata so they can
        // synchronize with any work happening on the primary (e.g., migration critical section).
        auto scopedCss =
            CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx, _nss);
        if (_droppingCollection)
            scopedCss->clearFilteringMetadataForDroppedCollection(opCtx);
        else
            scopedCss->clearFilteringMetadata(opCtx);
    }

    void rollback(OperationContext* opCtx) noexcept override {}

private:
    const NamespaceString _nss;
    const bool _droppingCollection;
};

/**
 * Invalidates the in-memory routing table cache when a collection is dropped, so the next caller
 * with routing information will provoke a routing table refresh and see the drop.
 *
 * The query parameter must contain an _id field that identifies which collections entry is being
 * updated.
 *
 * This only runs on secondaries.
 * The global exclusive lock is expected to be held by the caller.
 */
void onConfigDeleteInvalidateCachedCollectionMetadataAndNotify(OperationContext* opCtx,
                                                               const BSONObj& query) {
    // Notification of routing table changes is only needed on secondaries that are applying
    // oplog entries.
    if (opCtx->isEnforcingConstraints()) {
        return;
    }

    // Extract which collection entry is being deleted from the _id field.
    std::string deletedCollection;
    fassert(40479,
            bsonExtractStringField(query, ShardCollectionType::kNssFieldName, &deletedCollection));
    const NamespaceString deletedNss = NamespaceStringUtil::deserialize(
        boost::none, deletedCollection, SerializationContext::stateDefault());

    // Need the WUOW to retain the lock for CollectionPlacementVersionLogOpHandler::commit().
    // TODO SERVER-58223: evaluate whether this is safe or whether acquiring the lock can block.
    AllowLockAcquisitionOnTimestampedUnitOfWork allowLockAcquisition(
        shard_role_details::getLocker(opCtx));
    // TODO SERVER-99703: We have to disable the checks here as we're
    // violating the ordering of locks for databases. This can happen
    // because a write to a collection in the config database like the
    // critical section will make us take a lock on a different database.
    // That is, we have an operation that has taken a lock on config,
    // followed by a lock on a user database. Other op observers like the
    // preimages one will take the inverse order, that is they have a lock
    // on a user database and then take a lock on the config database.
    DisableLockerRuntimeOrderingChecks disableChecks{opCtx};
    AutoGetCollection autoColl(
        opCtx,
        deletedNss,
        MODE_IX,
        AutoGetCollection::Options{}.viewMode(auto_get_collection::ViewMode::kViewsPermitted));

    tassert(7751400,
            str::stream() << "Untimestamped writes to "
                          << NamespaceString::kShardConfigCollectionsNamespace.toStringForErrorMsg()
                          << " are not allowed",
            shard_role_details::getRecoveryUnit(opCtx)->isTimestamped());
    shard_role_details::getRecoveryUnit(opCtx)->registerChange(
        std::make_unique<CollectionPlacementVersionLogOpHandler>(deletedNss,
                                                                 /* droppingCollection */ true));
}

/**
 * Aborts any ongoing migration for the given namespace. Should only be called when observing
 * index operations.
 */
void abortOngoingMigrationIfNeeded(OperationContext* opCtx, const NamespaceString& nss) {
    const auto scopedCsr =
        CollectionShardingRuntime::assertCollectionLockedAndAcquireShared(opCtx, nss);
    if (auto msm = MigrationSourceManager::get(*scopedCsr)) {
        // Only interrupt the migration, but don't actually join
        (void)msm->abort();
    }
}

}  // namespace

ShardServerOpObserver::ShardServerOpObserver() = default;

ShardServerOpObserver::~ShardServerOpObserver() = default;

void ShardServerOpObserver::onInserts(OperationContext* opCtx,
                                      const CollectionPtr& coll,
                                      std::vector<InsertStatement>::const_iterator begin,
                                      std::vector<InsertStatement>::const_iterator end,
                                      const std::vector<RecordId>& recordIds,
                                      std::vector<bool> fromMigrate,
                                      bool defaultFromMigrate,
                                      OpStateAccumulator* opAccumulator) {
    // TODO (SERVER-91505): Determine if we should change this to check isDataConsistent.
    if (repl::ReplicationCoordinator::get(opCtx)->isInInitialSyncOrRollback()) {
        return;
    }

    const auto& nss = coll->ns();

    for (auto it = begin; it != end; ++it) {
        const auto& insertedDoc = it->doc;

        if (nss == NamespaceString::kServerConfigurationNamespace) {
            if (auto idElem = insertedDoc["_id"]) {
                if (idElem.str() == ShardIdentityType::IdName) {
                    auto shardIdentityDoc =
                        uassertStatusOK(ShardIdentityType::fromShardIdentityDocument(insertedDoc));
                    uassertStatusOK(shardIdentityDoc.validate(
                        true /* fassert cluster role matches shard identity document */));
                    /**
                     * Perform shard identity initialization once we are certain that the document
                     * is committed.
                     */
                    shard_role_details::getRecoveryUnit(opCtx)->onCommit(
                        [shardIdentity = std::move(shardIdentityDoc)](OperationContext* opCtx,
                                                                      boost::optional<Timestamp>) {
                            try {
                                ShardingInitializationMongoD::get(opCtx)
                                    ->initializeFromShardIdentity(opCtx, shardIdentity);
                            } catch (const AssertionException& ex) {
                                fassertFailedWithStatus(40071, ex.toStatus());
                            }
                        });
                }
            }
        }

        if (replica_set_endpoint::isFeatureFlagEnabledIgnoreFCV() &&
            serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer) &&
            nss == NamespaceString::kConfigsvrShardsNamespace) {
            // The feature flag check here needs to ignore the FCV since the
            // ReplicaSetEndpointShardingState needs to be maintained even before the FCV is fully
            // upgraded.
            if (auto shardId = insertedDoc["_id"].str(); shardId == ShardId::kConfigServerId) {
                shard_role_details::getRecoveryUnit(opCtx)->onCommit(
                    [](OperationContext* opCtx, boost::optional<Timestamp>) {
                        replica_set_endpoint::ReplicaSetEndpointShardingState::get(opCtx)
                            ->setIsConfigShard(true);
                    });
            }
        }

        if (nss == NamespaceString::kRangeDeletionNamespace) {
            // Return early on secondaries that are in oplog application.
            if (!opCtx->isEnforcingConstraints()) {
                return;
            }

            auto deletionTask =
                RangeDeletionTask::parse(insertedDoc, IDLParserContext("ShardServerOpObserver"));

            const auto numOrphanDocs = deletionTask.getNumOrphanDocs();
            BalancerStatsRegistry::get(opCtx)->onRangeDeletionTaskInsertion(
                deletionTask.getCollectionUuid(), numOrphanDocs);
        }

        if (nss == NamespaceString::kCollectionCriticalSectionsNamespace) {
            const auto collCSDoc = CollectionCriticalSectionDocument::parse(
                insertedDoc, IDLParserContext("ShardServerOpObserver"));
            invariant(!collCSDoc.getBlockReads());
            shard_role_details::getRecoveryUnit(opCtx)->onCommit(
                [insertedNss = collCSDoc.getNss(), reason = collCSDoc.getReason().getOwned()](
                    OperationContext* opCtx, boost::optional<Timestamp>) {
                    if (insertedNss.isDbOnly()) {
                        // Primaries take locks when writing to certain internal namespaces. It must
                        // be ensured that those locks are also taken on secondaries, when
                        // applying the related oplog entries.
                        boost::optional<AutoGetDb> lockDbIfNotPrimary;
                        if (!opCtx->isEnforcingConstraints()) {
                            // TODO SERVER-99703: We have to disable the checks here as we're
                            // violating the ordering of locks for databases. This can happen
                            // because a write to a collection in the config database like the
                            // critical section will make us take a lock on a different database.
                            // That is, we have an operation that has taken a lock on config,
                            // followed by a lock on a user database. Other op observers like the
                            // preimages one will take the inverse order, that is they have a lock
                            // on a user database and then take a lock on the config database.
                            DisableLockerRuntimeOrderingChecks disableChecks{opCtx};
                            lockDbIfNotPrimary.emplace(opCtx, insertedNss.dbName(), MODE_IX);
                        }
                        auto scopedDsr = DatabaseShardingRuntime::assertDbLockedAndAcquireExclusive(
                            opCtx, insertedNss.dbName());
                        scopedDsr->enterCriticalSectionCatchUpPhase(reason);
                    } else {
                        // Primaries take locks when writing to certain internal namespaces. It must
                        // be ensured that those locks are also taken on secondaries, when
                        // applying the related oplog entries.
                        boost::optional<AutoGetCollection> lockCollectionIfNotPrimary;
                        if (!opCtx->isEnforcingConstraints()) {
                            // TODO SERVER-99703: We have to disable the checks here as we're
                            // violating the ordering of locks for databases. This can happen
                            // because a write to a collection in the config database like the
                            // critical section will make us take a lock on a different database.
                            // That is, we have an operation that has taken a lock on config,
                            // followed by a lock on a user database. Other op observers like the
                            // preimages one will take the inverse order, that is they have a lock
                            // on a user database and then take a lock on the config database.
                            DisableLockerRuntimeOrderingChecks disableChecks{opCtx};
                            lockCollectionIfNotPrimary.emplace(
                                opCtx,
                                insertedNss,
                                fixLockModeForSystemDotViewsChanges(insertedNss, MODE_IX),
                                AutoGetCollection::Options{}.viewMode(
                                    auto_get_collection::ViewMode::kViewsPermitted));
                        }

                        auto scopedCsr =
                            CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(
                                opCtx, insertedNss);
                        scopedCsr->enterCriticalSectionCatchUpPhase(reason);
                    }
                });
        }
    }
}

void ShardServerOpObserver::onUpdate(OperationContext* opCtx,
                                     const OplogUpdateEntryArgs& args,
                                     OpStateAccumulator* opAccumulator) {
    // TODO (SERVER-91505): Determine if we should change this to check isDataConsistent.
    if (repl::ReplicationCoordinator::get(opCtx)->isInInitialSyncOrRollback()) {
        return;
    }

    const auto& updateDoc = args.updateArgs->update;
    // Most of these handlers do not need to run when the update is a full document replacement.
    // An empty updateDoc implies a no-op update and is not a valid oplog entry.
    const bool needsSpecialHandling = !updateDoc.isEmpty() &&
        (update_oplog_entry::extractUpdateType(updateDoc) !=
         update_oplog_entry::UpdateType::kReplacement);
    if (needsSpecialHandling &&
        args.coll->ns() == NamespaceString::kShardConfigCollectionsNamespace) {
        // Notification of routing table changes is only needed on secondaries that are applying
        // oplog entries.
        if (opCtx->isEnforcingConstraints()) {
            return;
        }

        // This logic runs on updates to the shard's persisted cache of the config server's
        // config.collections collection.
        //
        // If an update occurs to the 'lastRefreshedCollectionPlacementVersion' field it notifies
        // the catalog cache loader of a new placement version and clears the routing table so the
        // next caller with routing information will provoke a routing table refresh.
        //
        // When 'lastRefreshedCollectionPlacementVersion' is in 'update', it means that a chunk
        // metadata refresh has finished being applied to the collection's locally persisted
        // metadata store.
        //
        // If an update occurs to the 'enterCriticalSectionSignal' field, simply clear the routing
        // table immediately. This will provoke the next secondary caller to refresh through the
        // primary, blocking behind the critical section.

        // Extract which user collection was updated
        const auto updatedNss([&] {
            std::string coll;
            fassert(40477,
                    bsonExtractStringField(
                        args.updateArgs->criteria, ShardCollectionType::kNssFieldName, &coll));
            return NamespaceStringUtil::deserialize(
                boost::none, coll, SerializationContext::stateDefault());
        }());

        auto enterCriticalSectionFieldNewVal = update_oplog_entry::extractNewValueForField(
            updateDoc, ShardCollectionType::kEnterCriticalSectionCounterFieldName);
        auto refreshingFieldNewVal = update_oplog_entry::extractNewValueForField(
            updateDoc, ShardCollectionType::kRefreshingFieldName);

        // Need the WUOW to retain the lock for CollectionPlacementVersionLogOpHandler::commit().
        // TODO SERVER-58223: evaluate whether this is safe or whether acquiring the lock can block.
        AllowLockAcquisitionOnTimestampedUnitOfWork allowLockAcquisition(
            shard_role_details::getLocker(opCtx));

        // TODO SERVER-99703: We have to disable the checks here as we're
        // violating the ordering of locks for databases. This can happen
        // because a write to a collection in the config database like the
        // critical section will make us take a lock on a different database.
        // That is, we have an operation that has taken a lock on config,
        // followed by a lock on a user database. Other op observers like the
        // preimages one will take the inverse order, that is they have a lock
        // on a user database and then take a lock on the config database.
        DisableLockerRuntimeOrderingChecks disableChecks{opCtx};
        AutoGetCollection autoColl(
            opCtx,
            updatedNss,
            MODE_IX,
            AutoGetCollection::Options{}.viewMode(auto_get_collection::ViewMode::kViewsPermitted));
        if (refreshingFieldNewVal.isBoolean() && !refreshingFieldNewVal.boolean()) {
            tassert(7751401,
                    str::stream()
                        << "Untimestamped writes to "
                        << NamespaceString::kShardConfigCollectionsNamespace.toStringForErrorMsg()
                        << " are not allowed",
                    shard_role_details::getRecoveryUnit(opCtx)->isTimestamped());
            shard_role_details::getRecoveryUnit(opCtx)->registerChange(
                std::make_unique<CollectionPlacementVersionLogOpHandler>(
                    updatedNss, /* droppingCollection */ false));
        }

        if (enterCriticalSectionFieldNewVal.ok()) {
            // Force subsequent uses of the namespace to refresh the filtering metadata so they
            // can synchronize with any work happening on the primary (e.g., migration critical
            // section).
            CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx, updatedNss)
                ->clearFilteringMetadata(opCtx);
        }
    }

    if (args.coll->ns() == NamespaceString::kCollectionCriticalSectionsNamespace) {
        const auto collCSDoc = CollectionCriticalSectionDocument::parse(
            args.updateArgs->updatedDoc, IDLParserContext("ShardServerOpObserver"));
        invariant(collCSDoc.getBlockReads());

        shard_role_details::getRecoveryUnit(opCtx)->onCommit(
            [updatedNss = collCSDoc.getNss(), reason = collCSDoc.getReason().getOwned()](
                OperationContext* opCtx, boost::optional<Timestamp>) {
                if (updatedNss.isDbOnly()) {
                    // Primaries take locks when writing to certain internal namespaces. It must
                    // be ensured that those locks are also taken on secondaries, when
                    // applying the related oplog entries.
                    boost::optional<AutoGetDb> lockDbIfNotPrimary;
                    if (!opCtx->isEnforcingConstraints()) {
                        // TODO SERVER-99703: We have to disable the checks here as we're
                        // violating the ordering of locks for databases. This can happen
                        // because a write to a collection in the config database like the
                        // critical section will make us take a lock on a different database.
                        // That is, we have an operation that has taken a lock on config,
                        // followed by a lock on a user database. Other op observers like the
                        // preimages one will take the inverse order, that is they have a lock
                        // on a user database and then take a lock on the config database.
                        DisableLockerRuntimeOrderingChecks disableChecks{opCtx};
                        lockDbIfNotPrimary.emplace(opCtx, updatedNss.dbName(), MODE_IX);
                    }

                    auto scopedDsr = DatabaseShardingRuntime::assertDbLockedAndAcquireExclusive(
                        opCtx, updatedNss.dbName());
                    scopedDsr->enterCriticalSectionCommitPhase(reason);
                } else {
                    // Primaries take locks when writing to certain internal namespaces. It must
                    // be ensured that those locks are also taken on secondaries, when
                    // applying the related oplog entries.
                    boost::optional<AutoGetCollection> lockCollectionIfNotPrimary;
                    if (!opCtx->isEnforcingConstraints()) {
                        // TODO SERVER-99703: We have to disable the checks here as we're
                        // violating the ordering of locks for databases. This can happen
                        // because a write to a collection in the config database like the
                        // critical section will make us take a lock on a different database.
                        // That is, we have an operation that has taken a lock on config,
                        // followed by a lock on a user database. Other op observers like the
                        // preimages one will take the inverse order, that is they have a lock
                        // on a user database and then take a lock on the config database.
                        DisableLockerRuntimeOrderingChecks disableChecks{opCtx};
                        lockCollectionIfNotPrimary.emplace(
                            opCtx,
                            updatedNss,
                            fixLockModeForSystemDotViewsChanges(updatedNss, MODE_IX),
                            AutoGetCollection::Options{}.viewMode(
                                auto_get_collection::ViewMode::kViewsPermitted));
                    }

                    auto scopedCsr =
                        CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(
                            opCtx, updatedNss);
                    scopedCsr->enterCriticalSectionCommitPhase(reason);
                }
            });
    }

    const auto& nss = args.coll->ns();
    if (nss == NamespaceString::kServerConfigurationNamespace) {
        [&]() {
            const auto idElem = args.updateArgs->criteria["_id"];
            if (!idElem) {
                return;
            }
            if (idElem.str() != ShardIdentityType::IdName) {
                return;
            }
            const auto updatedShardIdentityDoc = args.updateArgs->updatedDoc;
            const auto oldShardIdentityDoc = uassertStatusOK(
                ShardIdentityType::fromShardIdentityDocument(args.updateArgs->preImageDoc));
            const auto shardIdentityDoc = uassertStatusOK(
                ShardIdentityType::fromShardIdentityDocument(updatedShardIdentityDoc));
            uassertStatusOK(shardIdentityDoc.validate());

            const auto deferShardingInitialization =
                shardIdentityDoc.getDeferShardingInitialization().value_or(false);

            // If there was a change in the defer sharding initialization field, then try
            // to initialize
            if (deferShardingInitialization !=
                oldShardIdentityDoc.getDeferShardingInitialization().value_or(false)) {
                if (deferShardingInitialization) {
                    LOGV2_WARNING(10892401,
                                  "ShardIdentity's deferShardingInitialization flag went from "
                                  "false to true, which is suspicious");
                }
                try {
                    ShardingInitializationMongoD::get(opCtx)->initializeFromShardIdentity(
                        opCtx, shardIdentityDoc);
                } catch (const AssertionException& ex) {
                    fassertFailedWithStatus(10892400, ex.toStatus());
                }
            }
        }();
    }
}

void ShardServerOpObserver::onDelete(OperationContext* opCtx,
                                     const CollectionPtr& coll,
                                     StmtId stmtId,
                                     const BSONObj& doc,
                                     const DocumentKey& documentKey,
                                     const OplogDeleteEntryArgs& args,
                                     OpStateAccumulator* opAccumulator) {
    // TODO (SERVER-91505): Determine if we should change this to check isDataConsistent.
    if (repl::ReplicationCoordinator::get(opCtx)->isInInitialSyncOrRollback()) {
        return;
    }

    const auto& nss = coll->ns();
    BSONObj documentId;
    if (nss == NamespaceString::kCollectionCriticalSectionsNamespace ||
        nss == NamespaceString::kRangeDeletionNamespace) {
        documentId = doc;
    } else {
        // Extract the _id field from the document. If it does not have an _id, use the
        // document itself as the _id.
        documentId = doc["_id"] ? doc["_id"].wrap() : doc;
    }
    invariant(!documentId.isEmpty());

    if (nss == NamespaceString::kShardConfigCollectionsNamespace) {
        onConfigDeleteInvalidateCachedCollectionMetadataAndNotify(opCtx, documentId);
    }

    if (nss == NamespaceString::kServerConfigurationNamespace) {
        if (auto idElem = documentId.firstElement()) {
            auto idStr = idElem.str();
            if (idStr == ShardIdentityType::IdName) {
                if (!repl::ReplicationCoordinator::get(opCtx)->getMemberState().rollback()) {
                    uasserted(40070,
                              "cannot delete shardIdentity document while in --shardsvr mode");
                } else {
                    LOGV2_WARNING(23779,
                                  "Shard identity document rolled back.  Will shut down after "
                                  "finishing rollback.");
                    ShardIdentityRollbackNotifier::get(opCtx)->recordThatRollbackHappened();
                }
            }
        }
    }

    if (replica_set_endpoint::isFeatureFlagEnabledIgnoreFCV() &&
        serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer) &&
        nss == NamespaceString::kConfigsvrShardsNamespace) {
        // The feature flag check here needs to ignore the FCV since the
        // ReplicaSetEndpointShardingState needs to be maintained even before the FCV is fully
        // upgraded.
        if (auto shardId = documentId["_id"].str(); shardId == ShardId::kConfigServerId) {
            shard_role_details::getRecoveryUnit(opCtx)->onCommit([](OperationContext* opCtx,
                                                                    boost::optional<Timestamp>) {
                replica_set_endpoint::ReplicaSetEndpointShardingState::get(opCtx)->setIsConfigShard(
                    false);
            });
        }
    }

    if (nss == NamespaceString::kCollectionCriticalSectionsNamespace) {
        const auto& deletedDoc = documentId;
        const auto collCSDoc = CollectionCriticalSectionDocument::parse(
            deletedDoc, IDLParserContext("ShardServerOpObserver"));

        shard_role_details::getRecoveryUnit(opCtx)->onCommit(
            [deletedNss = collCSDoc.getNss(),
             reason = collCSDoc.getReason().getOwned(),
             clearDbMetadata = collCSDoc.getClearDbInfo()](OperationContext* opCtx,
                                                           boost::optional<Timestamp>) {
                if (deletedNss.isDbOnly()) {
                    // Primaries take locks when writing to certain internal namespaces. It must
                    // be ensured that those locks are also taken on secondaries, when
                    // applying the related oplog entries.
                    boost::optional<AutoGetDb> lockDbIfNotPrimary;
                    if (!opCtx->isEnforcingConstraints()) {
                        // TODO SERVER-99703: We have to disable the checks here as we're
                        // violating the ordering of locks for databases. This can happen
                        // because a write to a collection in the config database like the
                        // critical section will make us take a lock on a different database.
                        // That is, we have an operation that has taken a lock on config,
                        // followed by a lock on a user database. Other op observers like the
                        // preimages one will take the inverse order, that is they have a lock
                        // on a user database and then take a lock on the config database.
                        DisableLockerRuntimeOrderingChecks disableChecks{opCtx};
                        lockDbIfNotPrimary.emplace(opCtx, deletedNss.dbName(), MODE_IX);
                    }

                    auto scopedDsr = DatabaseShardingRuntime::assertDbLockedAndAcquireExclusive(
                        opCtx, deletedNss.dbName());

                    // Secondaries that are in oplog application must clear the database metadata
                    // before releasing the in-memory critical section.
                    if (!opCtx->isEnforcingConstraints() && clearDbMetadata) {
                        scopedDsr->clearDbInfo_DEPRECATED(opCtx);
                    }

                    scopedDsr->exitCriticalSection(reason);
                } else {
                    // Primaries take locks when writing to certain internal namespaces. It must
                    // be ensured that those locks are also taken on secondaries, when
                    // applying the related oplog entries.
                    boost::optional<AutoGetCollection> lockCollectionIfNotPrimary;
                    if (!opCtx->isEnforcingConstraints()) {
                        // TODO SERVER-99703: We have to disable the checks here as we're
                        // violating the ordering of locks for databases. This can happen
                        // because a write to a collection in the config database like the
                        // critical section will make us take a lock on a different database.
                        // That is, we have an operation that has taken a lock on config,
                        // followed by a lock on a user database. Other op observers like the
                        // preimages one will take the inverse order, that is they have a lock
                        // on a user database and then take a lock on the config database.
                        DisableLockerRuntimeOrderingChecks disableChecks{opCtx};
                        lockCollectionIfNotPrimary.emplace(
                            opCtx,
                            deletedNss,
                            fixLockModeForSystemDotViewsChanges(deletedNss, MODE_IX),
                            AutoGetCollection::Options{}.viewMode(
                                auto_get_collection::ViewMode::kViewsPermitted));
                    }

                    auto scopedCsr =
                        CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(
                            opCtx, deletedNss);

                    // Secondaries that are in oplog application must clear the collection
                    // filtering metadata before releasing the in-memory critical section.
                    if (!opCtx->isEnforcingConstraints()) {
                        scopedCsr->clearFilteringMetadata(opCtx);
                    }

                    scopedCsr->exitCriticalSection(reason);
                }
            });
    }

    if (nss == NamespaceString::kRangeDeletionNamespace) {
        const auto& deletedDoc = documentId;

        const auto numOrphanDocs = [&] {
            auto numOrphanDocsElem = update_oplog_entry::extractNewValueForField(
                deletedDoc, RangeDeletionTask::kNumOrphanDocsFieldName);
            return numOrphanDocsElem.exactNumberLong();
        }();

        auto collUuid = [&] {
            BSONElement collUuidElem;
            uassertStatusOK(bsonExtractField(
                documentId, RangeDeletionTask::kCollectionUuidFieldName, &collUuidElem));
            return uassertStatusOK(UUID::parse(std::move(collUuidElem)));
        }();

        shard_role_details::getRecoveryUnit(opCtx)->onCommit(
            [collUuid = std::move(collUuid), numOrphanDocs](OperationContext* opCtx,
                                                            boost::optional<Timestamp>) {
                BalancerStatsRegistry::get(opCtx)->onRangeDeletionTaskDeletion(collUuid,
                                                                               numOrphanDocs);
            });
    }
}

void ShardServerOpObserver::onCreateCollection(
    OperationContext* opCtx,
    const NamespaceString& collectionName,
    const CollectionOptions& options,
    const BSONObj& idIndex,
    const OplogSlot& createOpTime,
    const boost::optional<CreateCollCatalogIdentifier>& createCollCatalogIdentifier,
    bool fromMigrate,
    bool isTimeseries) {
    // TODO (SERVER-91505): Determine if we should change this to check isDataConsistent.
    if (repl::ReplicationCoordinator::get(opCtx)->isInInitialSyncOrRollback()) {
        return;
    }

    // Only the shard primary nodes control the collection creation.
    if (!opCtx->writesAreReplicated()) {
        // On secondaries node of sharded cluster we force the cleanup of the filtering metadata in
        // order to remove anything that was left from any previous collection instance. This could
        // happen by first having an UNSHARDED version for a collection that didn't exist followed
        // by a movePrimary to the current shard.
        if (ShardingState::get(opCtx)->enabled()) {
            auto scopedCsr = CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(
                opCtx, collectionName);
            scopedCsr->clearFilteringMetadata(opCtx);
        }

        return;
    }

    // Collections which are always UNSHARDED have a fixed CSS, which never changes, so we don't
    // need to do anything
    if (collectionName.isNamespaceAlwaysUntracked()) {
        return;
    }

    // Temp collections are always UNSHARDED
    if (options.temp) {
        CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx, collectionName)
            ->setFilteringMetadata(opCtx, CollectionMetadata::UNTRACKED());
        return;
    }

    const auto& oss = OperationShardingState::get(opCtx);
    uassert(CannotImplicitlyCreateCollectionInfo(collectionName),
            "Implicit collection creation on a sharded cluster must go through the "
            "CreateCollectionCoordinator",
            oss._implicitCreationInfo._creationNss &&
                (oss._implicitCreationInfo._creationNss == collectionName ||
                 collectionName ==
                     oss._implicitCreationInfo._creationNss->makeTimeseriesBucketsNamespace()));

    // If the check above passes, this means the collection doesn't exist and is being created and
    // that the caller will be responsible to eventually set the proper placement version.
    auto scopedCsr =
        CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx, collectionName);
    if (oss._implicitCreationInfo._forceCSRAsUnknownAfterCollectionCreation) {
        scopedCsr->clearFilteringMetadata(opCtx);
    } else if (!scopedCsr->getCurrentMetadataIfKnown()) {
        scopedCsr->setFilteringMetadata(opCtx, CollectionMetadata::UNTRACKED());
    }
}

repl::OpTime ShardServerOpObserver::onDropCollection(OperationContext* opCtx,
                                                     const NamespaceString& collectionName,
                                                     const UUID& uuid,
                                                     std::uint64_t numRecords,
                                                     bool markFromMigrate,
                                                     bool isTimeseries) {
    // TODO (SERVER-91505): Determine if we should change this to check isDataConsistent.
    if (repl::ReplicationCoordinator::get(opCtx)->isInInitialSyncOrRollback()) {
        return {};
    }

    if (collectionName == NamespaceString::kServerConfigurationNamespace) {
        // Dropping system collections is not allowed for end users
        invariant(!opCtx->writesAreReplicated());
        invariant(repl::ReplicationCoordinator::get(opCtx)->getMemberState().rollback());

        // Can't confirm whether there was a ShardIdentity document or not yet, so assume there was
        // one and shut down the process to clear the in-memory sharding state
        LOGV2_WARNING(23780,
                      "admin.system.version collection rolled back. Will shut down after finishing "
                      "rollback");

        ShardIdentityRollbackNotifier::get(opCtx)->recordThatRollbackHappened();
    }

    return {};
}

void ShardServerOpObserver::onCreateIndex(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const UUID& uuid,
                                          const IndexBuildInfo& indexBuildInfo,
                                          bool fromMigrate,
                                          bool isTimeseries) {
    // TODO (SERVER-91505): Determine if we should change this to check isDataConsistent.
    if (repl::ReplicationCoordinator::get(opCtx)->isInInitialSyncOrRollback()) {
        return;
    }

    abortOngoingMigrationIfNeeded(opCtx, nss);
}

void ShardServerOpObserver::onStartIndexBuild(OperationContext* opCtx,
                                              const NamespaceString& nss,
                                              const UUID& collUUID,
                                              const UUID& indexBuildUUID,
                                              const std::vector<IndexBuildInfo>& indexes,
                                              bool fromMigrate,
                                              bool isTimeseries) {
    // TODO (SERVER-91505): Determine if we should change this to check isDataConsistent.
    if (repl::ReplicationCoordinator::get(opCtx)->isInInitialSyncOrRollback()) {
        return;
    }

    abortOngoingMigrationIfNeeded(opCtx, nss);
}

void ShardServerOpObserver::onStartIndexBuildSinglePhase(OperationContext* opCtx,
                                                         const NamespaceString& nss) {
    // TODO (SERVER-91505): Determine if we should change this to check isDataConsistent.
    if (repl::ReplicationCoordinator::get(opCtx)->isInInitialSyncOrRollback()) {
        return;
    }

    abortOngoingMigrationIfNeeded(opCtx, nss);
}

void ShardServerOpObserver::onDropIndex(OperationContext* opCtx,
                                        const NamespaceString& nss,
                                        const UUID& uuid,
                                        const std::string& indexName,
                                        const BSONObj& indexInfo,
                                        bool isTimeseries) {
    // TODO (SERVER-91505): Determine if we should change this to check isDataConsistent.
    if (repl::ReplicationCoordinator::get(opCtx)->isInInitialSyncOrRollback()) {
        return;
    }

    abortOngoingMigrationIfNeeded(opCtx, nss);
};

void ShardServerOpObserver::onCollMod(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      const UUID& uuid,
                                      const BSONObj& collModCmd,
                                      const CollectionOptions& oldCollOptions,
                                      boost::optional<IndexCollModInfo> indexInfo,
                                      bool isTimeseries) {
    // TODO (SERVER-91505): Determine if we should change this to check isDataConsistent.
    if (repl::ReplicationCoordinator::get(opCtx)->isInInitialSyncOrRollback()) {
        return;
    }
    // A collMod with no arguments is used to repair or cleanup metadata during FCV upgrade or
    // downgrade. This is not an index modification operation, and does not need to abort ongoing
    // migrations.
    if (collModCmd.nFields() > 1) {
        abortOngoingMigrationIfNeeded(opCtx, nss);
    }
};

void ShardServerOpObserver::onReplicationRollback(OperationContext* opCtx,
                                                  const RollbackObserverInfo& rbInfo) {
    ShardingRecoveryService::get(opCtx)->onReplicationRollback(opCtx, rbInfo.rollbackNamespaces);
}

void ShardServerOpObserver::onCreateDatabaseMetadata(OperationContext* opCtx,
                                                     const repl::OplogEntry& op) {
    // TODO (SERVER-91505): Determine if we should change this to check isDataConsistent.
    if (repl::ReplicationCoordinator::get(opCtx)->isInInitialSyncOrRollback()) {
        return;
    }

    auto entry = CreateDatabaseMetadataOplogEntry::parse(
        op.getObject(), IDLParserContext("OplogCreateDatabaseMetadataOplogEntryContext"));

    auto dbMetadata = entry.getDb();
    auto dbName = dbMetadata.getDbName();

    auto scopedDsr = DatabaseShardingRuntime::acquireExclusive(opCtx, dbName);
    scopedDsr->setDbMetadata(opCtx, dbMetadata);
}

void ShardServerOpObserver::onDropDatabaseMetadata(OperationContext* opCtx,
                                                   const repl::OplogEntry& op) {
    // TODO (SERVER-91505): Determine if we should change this to check isDataConsistent.
    if (repl::ReplicationCoordinator::get(opCtx)->isInInitialSyncOrRollback()) {
        return;
    }

    auto entry = DropDatabaseMetadataOplogEntry::parse(
        op.getObject(), IDLParserContext("OplogDropDatabaseMetadataOplogEntryContext"));

    auto dbName = entry.getDbName();

    auto scopedDsr = DatabaseShardingRuntime::acquireExclusive(opCtx, dbName);
    scopedDsr->clearDbMetadata();
}

}  // namespace mongo
