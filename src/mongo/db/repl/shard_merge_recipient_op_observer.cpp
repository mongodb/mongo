/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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


#include "mongo/db/repl/shard_merge_recipient_op_observer.h"

#include <algorithm>
#include <fmt/format.h>
#include <iterator>
#include <memory>
#include <string>

#include <absl/container/node_hash_set.h>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "boost/system/detail/error_code.hpp"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/database_name.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/repl/tenant_file_importer_service.h"
#include "mongo/db/repl/tenant_migration_access_blocker.h"
#include "mongo/db/repl/tenant_migration_access_blocker_registry.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/repl/tenant_migration_decoration.h"
#include "mongo/db/repl/tenant_migration_recipient_access_blocker.h"
#include "mongo/db/repl/tenant_migration_shard_merge_util.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"
#include "mongo/db/repl/tenant_migration_util.h"
#include "mongo/db/repl/timestamp_block.h"
#include "mongo/db/serverless/serverless_operation_lock_registry.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/tenant_id.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

namespace mongo::repl {
using namespace fmt;
using namespace shard_merge_utils;
namespace {
bool markedGCAfterMigrationStart(const ShardMergeRecipientDocument& doc) {
    return !doc.getStartGarbageCollect() && doc.getExpireAt();
}

template <typename Func>
void runOnAlternateClient(const std::string& name, Func func) {
    auto parentClientUnkillableByStepDown = [&] {
        return !cc().canKillSystemOperationInStepdown(WithLock::withoutLock());
    }();

    auto client = getGlobalServiceContext()->makeClient(name);
    AlternativeClientRegion acr(client);

    if (parentClientUnkillableByStepDown) {
        stdx::lock_guard<Client> lk(cc());
        cc().setSystemOperationUnkillableByStepdown(lk);
    }

    auto opCtx = cc().makeOperationContext();

    func(opCtx.get());
}

/**
 * Note: Refer to deleteTenantDataWhenMergeAborts() comment for the AlternativeClientRegion
 * requirement.
 */
void dropTempFilesAndCollsIfAny(OperationContext* opCtx, const UUID& migrationId) {
    // Drop the import done marker collection.
    runOnAlternateClient("dropShardMergeMarkerColl", [&migrationId](OperationContext* acrOpCtx) {
        dropImportDoneMarkerLocalCollection(acrOpCtx, migrationId);
    });

    const auto tempWTDirectory = fileClonerTempDir(migrationId);
    // Do an early exit if the temp dir is not present.
    if (!boost::filesystem::exists(tempWTDirectory))
        return;

    // Remove idents unknown to both storage and mdb_catalog.
    bool filesRemoved = false;
    const auto movingIdents = readMovingFilesMarker(tempWTDirectory);
    for (const auto& ident : movingIdents) {
        // It's impossible for files to be known by mdb_catalog but not storage. Files known to
        // storage but not mdb_catalog could occur if node restarts during import. However, startup
        // recovery removes such files. Therefore, we only need to handle files unknown to both
        // mdb_catalog and storage. Thus, verifying the file(ident) existence in storage is
        // sufficent.
        bool identKnown =
            getGlobalServiceContext()->getStorageEngine()->getEngine()->hasIdent(opCtx, ident);
        if (!identKnown) {
            filesRemoved = true;
            removeFile(constructDestinationPath(ident));
        }
    }
    if (filesRemoved)
        fsyncDataDirectory();

    // Remove the temp directory.
    fsyncRemoveDirectory(tempWTDirectory);
}

/**
 * Note: Though opObserver drops tenant collections only after the importer service stops importing
 * the collection, a collection might be imported after opObserver's storage txn has started(i.e,
 * import collection storage txnId >  opObserver storage txnId), causing the collection to be
 * invisible to the opObserver. To ensure visibility of all imported collections to the opObserver,
 * drop the tenant collection in AlternativeClientRegion.
 */
void deleteTenantDataWhenMergeAborts(const ShardMergeRecipientDocument& doc) {
    runOnAlternateClient("dropShardMergeDonorTenantColls", [&doc](OperationContext* opCtx) {
        auto storageEngine = opCtx->getServiceContext()->getStorageEngine();

        invariant(doc.getAbortOpTime());
        const auto dropOpTime = *doc.getAbortOpTime();
        TimestampBlock tsBlock(opCtx, dropOpTime.getTimestamp());

        UnreplicatedWritesBlock writeBlock{opCtx};

        writeConflictRetry(opCtx, "dropShardMergeDonorTenantColls", NamespaceString(), [&] {
            WriteUnitOfWork wuow(opCtx);

            for (const auto& tenantId : doc.getTenantIds()) {
                std::vector<DatabaseName> databases;
                if (gMultitenancySupport) {
                    databases = storageEngine->listDatabases(tenantId);
                } else {
                    auto allDatabases = storageEngine->listDatabases();
                    std::copy_if(allDatabases.begin(),
                                 allDatabases.end(),
                                 std::back_inserter(databases),
                                 [tenant = tenantId.toString() + "_"](const DatabaseName& db) {
                                     // In non multitenacy environment, check if the db has a
                                     // matched tenant prefix.
                                     return StringData{
                                         DatabaseNameUtil::serialize(
                                             db, SerializationContext::stateDefault())}
                                         .startsWith(tenant);
                                 });
                }

                for (const auto& database : databases) {
                    AutoGetDb autoDb{opCtx, database, MODE_X};
                    Database* db = autoDb.getDb();
                    if (!db) {
                        continue;
                    }

                    LOGV2(7221802,
                          "Dropping tenant database for shard merge garbage collection",
                          "tenant"_attr = tenantId,
                          "database"_attr = database,
                          "migrationId"_attr = doc.getId(),
                          "abortOpTime"_attr = dropOpTime);

                    IndexBuildsCoordinator::get(opCtx)->assertNoBgOpInProgForDb(db->name());

                    auto catalog = CollectionCatalog::get(opCtx);
                    for (auto&& collection : catalog->range(db->name())) {
                        if (!collection) {
                            break;
                        }

                        uassertStatusOK(
                            db->dropCollectionEvenIfSystem(opCtx, collection->ns(), dropOpTime));
                    }

                    auto databaseHolder = DatabaseHolder::get(opCtx);
                    databaseHolder->close(opCtx, db->name());
                }
            }

            wuow.commit();
        });
    });
}

void onShardMergeRecipientsNssInsert(OperationContext* opCtx,
                                     std::vector<InsertStatement>::const_iterator first,
                                     std::vector<InsertStatement>::const_iterator last) {
    if (tenant_migration_access_blocker::inRecoveryMode(opCtx))
        return;

    for (auto it = first; it != last; it++) {
        auto recipientStateDoc =
            ShardMergeRecipientDocument::parse(IDLParserContext("recipientStateDoc"), it->doc);
        switch (recipientStateDoc.getState()) {
            case ShardMergeRecipientStateEnum::kStarted: {
                invariant(!recipientStateDoc.getStartGarbageCollect());

                const auto migrationId = recipientStateDoc.getId();
                ServerlessOperationLockRegistry::get(opCtx->getServiceContext())
                    .acquireLock(ServerlessOperationLockRegistry::LockType::kMergeRecipient,
                                 migrationId);
                opCtx->recoveryUnit()->onRollback([migrationId](OperationContext* opCtx) {
                    ServerlessOperationLockRegistry::get(opCtx->getServiceContext())
                        .releaseLock(ServerlessOperationLockRegistry::LockType::kMergeRecipient,
                                     migrationId);
                });

                auto& registry =
                    TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext());
                for (const auto& tenantId : recipientStateDoc.getTenantIds()) {
                    registry.add(tenantId,
                                 std::make_shared<TenantMigrationRecipientAccessBlocker>(
                                     opCtx->getServiceContext(), migrationId));
                }
                opCtx->recoveryUnit()->onRollback([migrationId](OperationContext* opCtx) {
                    TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                        .removeAccessBlockersForMigration(
                            migrationId, TenantMigrationAccessBlocker::BlockerType::kRecipient);
                });

                const auto& startAtOpTimeOptional = recipientStateDoc.getStartAtOpTime();
                invariant(startAtOpTimeOptional);
                opCtx->recoveryUnit()->onCommit(
                    [migrationId, startAtOpTime = *startAtOpTimeOptional](OperationContext* opCtx,
                                                                          auto _) {
                        repl::TenantFileImporterService::get(opCtx)->startMigration(migrationId,
                                                                                    startAtOpTime);
                    });
            } break;
            case ShardMergeRecipientStateEnum::kCommitted:
            case ShardMergeRecipientStateEnum::kAborted:
                invariant(recipientStateDoc.getStartGarbageCollect());
                break;
            default:
                MONGO_UNREACHABLE;
        }
    }
}

void onDonatedFilesCollNssInsert(OperationContext* opCtx,
                                 std::vector<InsertStatement>::const_iterator first,
                                 std::vector<InsertStatement>::const_iterator last) {
    if (tenant_migration_access_blocker::inRecoveryMode(opCtx))
        return;

    for (auto it = first; it != last; it++) {
        const auto& metadataDoc = it->doc;
        const auto migrationId = uassertStatusOK(UUID::parse(metadataDoc[kMigrationIdFieldName]));
        repl::TenantFileImporterService::get(opCtx)->learnedFilename(migrationId, metadataDoc);
    }
}

void assertStateTransitionIsValid(ShardMergeRecipientStateEnum prevState,
                                  ShardMergeRecipientStateEnum nextState) {

    auto validPrevStates = [&]() -> stdx::unordered_set<ShardMergeRecipientStateEnum> {
        switch (nextState) {
            case ShardMergeRecipientStateEnum::kStarted:
                return {ShardMergeRecipientStateEnum::kStarted};
            case ShardMergeRecipientStateEnum::kLearnedFilenames:
                return {ShardMergeRecipientStateEnum::kStarted,
                        ShardMergeRecipientStateEnum::kLearnedFilenames};
            case ShardMergeRecipientStateEnum::kConsistent:
                return {ShardMergeRecipientStateEnum::kLearnedFilenames,
                        ShardMergeRecipientStateEnum::kConsistent};
            case ShardMergeRecipientStateEnum::kCommitted:
                return {ShardMergeRecipientStateEnum::kConsistent,
                        ShardMergeRecipientStateEnum::kCommitted};
            case ShardMergeRecipientStateEnum::kAborted:
                return {ShardMergeRecipientStateEnum::kStarted,
                        ShardMergeRecipientStateEnum::kLearnedFilenames,
                        ShardMergeRecipientStateEnum::kConsistent,
                        ShardMergeRecipientStateEnum::kAborted};
            default:
                MONGO_UNREACHABLE;
        }
    }();

    uassert(7339766, "Invalid state transition", validPrevStates.contains(prevState));
}

void onTransitioningToLearnedFilenames(OperationContext* opCtx,
                                       const ShardMergeRecipientDocument& recipientStateDoc) {
    opCtx->recoveryUnit()->onCommit(
        [migrationId = recipientStateDoc.getId()](OperationContext* opCtx, auto _) {
            repl::TenantFileImporterService::get(opCtx)->learnedAllFilenames(migrationId);
        });
}

void onTransitioningToConsistent(OperationContext* opCtx,
                                 const ShardMergeRecipientDocument& recipientStateDoc) {
    assertImportDoneMarkerLocalCollExistsOnMergeConsistent(opCtx, recipientStateDoc.getId());
    if (recipientStateDoc.getRejectReadsBeforeTimestamp()) {
        opCtx->recoveryUnit()->onCommit([recipientStateDoc](OperationContext* opCtx, auto _) {
            auto mtabVector =
                TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                    .getRecipientAccessBlockersForMigration(recipientStateDoc.getId());
            invariant(!mtabVector.empty());
            for (auto& mtab : mtabVector) {
                invariant(mtab);
                mtab->startRejectingReadsBefore(
                    recipientStateDoc.getRejectReadsBeforeTimestamp().get());
            }
        });
    }
}

void onTransitioningToCommitted(OperationContext* opCtx,
                                const ShardMergeRecipientDocument& recipientStateDoc) {
    auto migrationId = recipientStateDoc.getId();
    // It's safe to do interrupt outside of onCommit hook as the decision to forget a migration or
    // the migration decision is not reversible.
    repl::TenantFileImporterService::get(opCtx)->interruptMigration(migrationId);

    if (markedGCAfterMigrationStart(recipientStateDoc)) {
        opCtx->recoveryUnit()->onCommit([migrationId](OperationContext* opCtx, auto _) {
            auto mtabVector = TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                                  .getRecipientAccessBlockersForMigration(migrationId);
            invariant(!mtabVector.empty());
            for (auto& mtab : mtabVector) {
                invariant(mtab);
                // Once the migration is committed and state doc is marked garbage collectable,
                // the TTL deletions should be unblocked for the imported donor collections.
                mtab->stopBlockingTTL();
            }

            ServerlessOperationLockRegistry::get(opCtx->getServiceContext())
                .releaseLock(ServerlessOperationLockRegistry::LockType::kMergeRecipient,
                             migrationId);
        });

        repl::TenantFileImporterService::get(opCtx)->resetMigration(migrationId);
        dropTempFilesAndCollsIfAny(opCtx, migrationId);
    }
}

void onTransitioningToAborted(OperationContext* opCtx,
                              const ShardMergeRecipientDocument& recipientStateDoc) {
    auto migrationId = recipientStateDoc.getId();
    if (!markedGCAfterMigrationStart(recipientStateDoc)) {
        // It's safe to do interrupt outside of onCommit hook as the decision to forget a migration
        // or the migration decision is not reversible.
        repl::TenantFileImporterService::get(opCtx)->interruptMigration(migrationId);

        const auto& importCompletedFuture =
            repl::TenantFileImporterService::get(opCtx)->getImportCompletedFuture(migrationId);
        // Wait for the importer service to stop collection import task before dropping imported
        // collections.
        if (importCompletedFuture) {
            LOGV2(7458507, "Waiting for the importer service to finish importing task");
            importCompletedFuture->wait(opCtx);
        }
        deleteTenantDataWhenMergeAborts(recipientStateDoc);
    } else {
        opCtx->recoveryUnit()->onCommit([migrationId](OperationContext* opCtx, auto _) {
            // Remove access blocker and release locks to allow faster migration retry.
            // (Note: Not needed to unblock TTL deletions as we would have already dropped all
            // imported donor collections immediately on transitioning to `kAborted`).
            TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                .removeAccessBlockersForMigration(
                    migrationId, TenantMigrationAccessBlocker::BlockerType::kRecipient);

            ServerlessOperationLockRegistry::get(opCtx->getServiceContext())
                .releaseLock(ServerlessOperationLockRegistry::LockType::kMergeRecipient,
                             migrationId);
        });

        repl::TenantFileImporterService::get(opCtx)->resetMigration(migrationId);
        dropTempFilesAndCollsIfAny(opCtx, migrationId);
    }
}

void handleUpdateRecoveryMode(OperationContext* opCtx,
                              const ShardMergeRecipientDocument& recipientStateDoc) {
    // Note that we expect this path not running during initial sync(inconsistent data), as we
    // intentionally crash the server upon detecting the state document oplog entry for replay.
    const auto migrationId = recipientStateDoc.getId();

    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    invariant(!(replCoord->getSettings().isReplSet() &&
                repl::TenantFileImporterService::get(opCtx)->hasActiveMigration(migrationId)));

    if (markedGCAfterMigrationStart(recipientStateDoc)) {
        dropTempFilesAndCollsIfAny(opCtx, migrationId);
    } else if (recipientStateDoc.getState() == ShardMergeRecipientStateEnum::kAborted) {
        deleteTenantDataWhenMergeAborts(recipientStateDoc);
    }
}

}  // namespace

void ShardMergeRecipientOpObserver::onInserts(OperationContext* opCtx,
                                              const CollectionPtr& coll,
                                              std::vector<InsertStatement>::const_iterator first,
                                              std::vector<InsertStatement>::const_iterator last,
                                              std::vector<bool> fromMigrate,
                                              bool defaultFromMigrate,
                                              OpStateAccumulator* opAccumulator) {
    if (coll->ns() == NamespaceString::kShardMergeRecipientsNamespace) {
        onShardMergeRecipientsNssInsert(opCtx, first, last);
        return;
    }

    if (isDonatedFilesCollection(coll->ns())) {
        onDonatedFilesCollNssInsert(opCtx, first, last);
        return;
    }
}

void ShardMergeRecipientOpObserver::onUpdate(OperationContext* opCtx,
                                             const OplogUpdateEntryArgs& args,
                                             OpStateAccumulator* opAccumulator) {
    if (args.coll->ns() != NamespaceString::kShardMergeRecipientsNamespace) {
        return;
    }

    auto recipientStateDoc = ShardMergeRecipientDocument::parse(
        IDLParserContext("recipientStateDoc"), args.updateArgs->updatedDoc);
    if (tenant_migration_access_blocker::inRecoveryMode(opCtx)) {
        handleUpdateRecoveryMode(opCtx, recipientStateDoc);
        return;
    }

    auto nextState = recipientStateDoc.getState();
    auto prevState = ShardMergeRecipientState_parse(
        IDLParserContext("preImageRecipientStateDoc"),
        args.updateArgs->preImageDoc[ShardMergeRecipientDocument::kStateFieldName]
            .valueStringData());
    assertStateTransitionIsValid(prevState, nextState);

    switch (nextState) {
        case ShardMergeRecipientStateEnum::kStarted:
            break;
        case ShardMergeRecipientStateEnum::kLearnedFilenames:
            onTransitioningToLearnedFilenames(opCtx, recipientStateDoc);
            break;
        case ShardMergeRecipientStateEnum::kConsistent:
            onTransitioningToConsistent(opCtx, recipientStateDoc);
            break;
        case ShardMergeRecipientStateEnum::kCommitted:
            onTransitioningToCommitted(opCtx, recipientStateDoc);
            break;
        case ShardMergeRecipientStateEnum::kAborted:
            onTransitioningToAborted(opCtx, recipientStateDoc);
            break;
        default:
            MONGO_UNREACHABLE;
    }
}

void ShardMergeRecipientOpObserver::aboutToDelete(OperationContext* opCtx,
                                                  const CollectionPtr& coll,
                                                  BSONObj const& doc,
                                                  OplogDeleteEntryArgs* args,
                                                  OpStateAccumulator* opAccumulator) {
    if (coll->ns() != NamespaceString::kShardMergeRecipientsNamespace ||
        tenant_migration_access_blocker::inRecoveryMode(opCtx)) {
        return;
    }

    auto recipientStateDoc =
        ShardMergeRecipientDocument::parse(IDLParserContext("recipientStateDoc"), doc);

    bool isDocMarkedGarbageCollectable = [&] {
        auto state = recipientStateDoc.getState();
        auto expireAtIsSet = recipientStateDoc.getExpireAt().has_value();
        invariant(!expireAtIsSet || state == ShardMergeRecipientStateEnum::kCommitted ||
                  state == ShardMergeRecipientStateEnum::kAborted);
        return expireAtIsSet;
    }();

    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "Cannot delete the recipient state document "
                          << " since it has not been marked as garbage collectable: "
                          << tenant_migration_util::redactStateDoc(recipientStateDoc.toBSON()),
            isDocMarkedGarbageCollectable);

    tenantMigrationInfo(opCtx) = TenantMigrationInfo(recipientStateDoc.getId());
}

void ShardMergeRecipientOpObserver::onDelete(OperationContext* opCtx,
                                             const CollectionPtr& coll,
                                             StmtId stmtId,
                                             const BSONObj& doc,
                                             const OplogDeleteEntryArgs& args,
                                             OpStateAccumulator* opAccumulator) {
    if (coll->ns() != NamespaceString::kShardMergeRecipientsNamespace ||
        tenant_migration_access_blocker::inRecoveryMode(opCtx)) {
        return;
    }

    if (auto tmi = tenantMigrationInfo(opCtx)) {
        opCtx->recoveryUnit()->onCommit([migrationId = tmi->uuid](OperationContext* opCtx, auto _) {
            LOGV2_INFO(7339765,
                       "Removing expired recipient access blocker",
                       "migrationId"_attr = migrationId);
            TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                .removeAccessBlockersForMigration(
                    migrationId, TenantMigrationAccessBlocker::BlockerType::kRecipient);
        });
    }
}

repl::OpTime ShardMergeRecipientOpObserver::onDropCollection(OperationContext* opCtx,
                                                             const NamespaceString& collectionName,
                                                             const UUID& uuid,
                                                             std::uint64_t numRecords,
                                                             const CollectionDropType dropType,
                                                             bool markFromMigrate) {
    if (collectionName == NamespaceString::kShardMergeRecipientsNamespace &&
        !tenant_migration_access_blocker::inRecoveryMode(opCtx)) {

        uassert(
            ErrorCodes::IllegalOperation,
            str::stream() << "Cannot drop "
                          << NamespaceString::kShardMergeRecipientsNamespace.toStringForErrorMsg()
                          << " collection as it is not empty",
            !numRecords);
    }
    return OpTime();
}

}  // namespace mongo::repl
