// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index_builds/index_build_attempts.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/collection_crud/container_write.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index_builds/index_build_knobs_gen.h"
#include "mongo/db/index_builds/primary_driven/util.h"
#include "mongo/db/index_builds/resumable_index_builds_common.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/server_recovery.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/control/journal_flusher.h"
#include "mongo/db/storage/lazy_record_store.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo::index_builds::build_attempts {

namespace two_phase {
namespace {

const auto& kNss = NamespaceString::kLocalIndexBuildsNamespace;
constexpr std::string_view kAttemptsField = "numAttemptsAfterUncleanShutdown";

CollectionAcquisition acquireLedger(OperationContext* opCtx,
                                    AcquisitionPrerequisites::OperationType operationType,
                                    LockMode mode) {
    return acquireCollection(opCtx,
                             CollectionAcquisitionRequest(kNss,
                                                          PlacementConcern::kPretendUnsharded,
                                                          repl::ReadConcernArgs::get(opCtx),
                                                          operationType),
                             mode);
}

boost::optional<BSONObj> findLedgerEntry(OperationContext* opCtx,
                                         const CollectionAcquisition& collection,
                                         const UUID& buildUUID) {
    if (!collection.exists()) {
        return boost::none;
    }

    const auto& collPtr = collection.getCollectionPtr();
    const auto rid = Helpers::findById(opCtx, collPtr, BSON("_id" << buildUUID));
    Snapshotted<BSONObj> doc;
    if (!collPtr->findDoc(opCtx, rid, &doc)) {
        return boost::none;
    }
    return doc.value().getOwned();
}

}  // namespace

bool createCollection(OperationContext* opCtx) {
    auto created = repl::StorageInterface::get(opCtx)->createCollection(
        opCtx,
        kNss,
        CollectionOptions{.clusteredIndex = clustered_util::makeDefaultClusteredIdIndex()});
    if (created == ErrorCodes::NamespaceExists) {
        return false;
    }
    uassertStatusOK(created);
    return true;
}

boost::optional<int> read(OperationContext* opCtx, const UUID& buildUUID) {
    return writeConflictRetry(opCtx, "readIndexBuildAttempts", kNss, [&]() -> boost::optional<int> {
        const auto collection = acquireLedger(opCtx, AcquisitionPrerequisites::kRead, MODE_IS);
        auto entry = findLedgerEntry(opCtx, collection, buildUUID);
        if (!entry) {
            return boost::none;
        }
        return (*entry)[kAttemptsField].safeNumberInt();
    });
}

bool checkWithinAttemptLimit(OperationContext* opCtx, const UUID& buildUUID) {
    tassert(13756101,
            "Expected no locks to be held when recording an index build attempt",
            !shard_role_details::getLocker(opCtx)->isLocked());

    if (createCollection(opCtx)) {
        LOGV2(13756100,
              "Index build: created the index build attempt counts collection outside startup",
              "buildUUID"_attr = buildUUID);
    }

    // Only the first attempt is recorded here, with a count of 0. Startup counts attempts after
    // unclean shutdowns, so restarts within a process (e.g. rollback) don't count again.
    const int attempts = writeConflictRetry(opCtx, "recordInitialIndexBuildAttempt", kNss, [&] {
        const auto collection = acquireLedger(opCtx, AcquisitionPrerequisites::kWrite, MODE_IX);
        WriteUnitOfWork wuow(opCtx);
        if (auto entry = findLedgerEntry(opCtx, collection, buildUUID)) {
            return (*entry)[kAttemptsField].safeNumberInt();
        }
        uassertStatusOK(collection_internal::insertDocument(
            opCtx,
            collection.getCollectionPtr(),
            InsertStatement(BSON("_id" << buildUUID << kAttemptsField << 0)),
            /*opDebug=*/nullptr));
        wuow.commit();
        return 0;
    });

    // Make the count durable before the build runs, since it may crash the node. This includes any
    // increment from startup, which doesn't wait for durability while holding the global write
    // lock.
    JournalFlusher::get(opCtx)->waitForJournalFlush(opCtx);

    return attempts <= gMaxIndexBuildAttemptsAfterUncleanShutdown.load();
}

void incrementAttempts(OperationContext* opCtx) {
    // Increment all ledger entries, spanning all the unfinished index builds. This ensures we
    // account for builds that are restarted through oplog replay (e.g. if they started and crashed
    // after the last stable checkpoint) rather than startup recovery, which only includes builds
    // present in the last stable checkpoint.
    uassertStatusOK(repl::StorageInterface::get(opCtx)->updateDocuments(
        opCtx, kNss, BSONObj(), {BSON("$inc" << BSON(kAttemptsField << 1)), Timestamp()}));
}

void trackSizeThroughRecovery(OperationContext* opCtx) {
    const auto collection = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, kNss);
    if (!collection) {
        return;
    }
    sizeRecoveryState(opCtx->getServiceContext())
        .markCollectionAsAlwaysNeedsSizeAdjustment(collection->getRecordStore()->getIdent());
}

void removeEntry(OperationContext* opCtx, const UUID& buildUUID) {
    removeEntries(opCtx, {buildUUID});
}

void removeEntries(OperationContext* opCtx, const std::vector<UUID>& buildUUIDs) {
    tassert(13756102,
            "Expected to remove index build attempt counts in a WriteUnitOfWork",
            shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());
    const auto collection = acquireLedger(opCtx, AcquisitionPrerequisites::kWrite, MODE_IX);
    const auto& collPtr = collection.getCollectionPtr();
    for (const auto& buildUUID : buildUUIDs) {
        if (!findLedgerEntry(opCtx, collection, buildUUID)) {
            continue;
        }
        collection_internal::deleteDocument(
            opCtx,
            collPtr,
            kUninitializedStmtId,
            Helpers::findById(opCtx, collPtr, BSON("_id" << buildUUID)),
            /*opDebug=*/nullptr);
    }
}

}  // namespace two_phase

namespace primary_driven {
namespace {

bool recordResumesWithoutProgress(OperationContext* opCtx,
                                  const DatabaseName& dbName,
                                  const std::string& ident,
                                  const ResumeIndexInfo& resumeInfo,
                                  int resumesWithoutProgress) {
    IndexBuildMetadata metadata;
    metadata.setBuildUUID(resumeInfo.getBuildUUID());
    metadata.setPhase(resumeInfo.getPhase());
    metadata.setCollectionUUID(resumeInfo.getCollectionUUID());
    metadata.setResumesWithoutProgress(resumesWithoutProgress);
    const auto obj = metadata.toBSON();

    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);

    return writeConflictRetry(
        opCtx,
        ru,
        "recordIndexBuildResumesWithoutProgress",
        NamespaceStringOrUUID{dbName, resumeInfo.getCollectionUUID()},
        [&] {
            const auto collection =
                acquireCollection(opCtx,
                                  CollectionAcquisitionRequest(
                                      NamespaceStringOrUUID{dbName, resumeInfo.getCollectionUUID()},
                                      PlacementConcern::kPretendUnsharded,
                                      repl::ReadConcernArgs::get(opCtx),
                                      AcquisitionPrerequisites::kWrite),
                                  MODE_IX);

            if (!collection.exists() ||
                !index_builds::primary_driven::registry(opCtx->getServiceContext())
                     .contains(resumeInfo.getBuildUUID())) {
                LOGV2(13800505,
                      "Index build: skipping resume count write because the build no longer exists",
                      "buildUUID"_attr = resumeInfo.getBuildUUID(),
                      "collectionUUID"_attr = resumeInfo.getCollectionUUID());
                return false;
            }

            LazyRecordStore table(opCtx, ident, LazyRecordStore::CreateMode::openExisting);
            auto& container = std::get<std::reference_wrapper<IntegerKeyedContainer>>(
                                  table.getTableOrThrow().getContainer())
                                  .get();
            WriteUnitOfWork wuow{opCtx};
            if (!container.getCursor(ru)->find(index_builds::primary_driven::metadataKey)) {
                LOGV2(13800504,
                      "Index build: skipping resume count write because the metadata record does "
                      "not exist",
                      "buildUUID"_attr = resumeInfo.getBuildUUID(),
                      "collectionUUID"_attr = resumeInfo.getCollectionUUID(),
                      "phase"_attr = idl::serialize(resumeInfo.getPhase()));
                return false;
            }
            uassertStatusOK(
                container_write::update(opCtx,
                                        ru,
                                        container,
                                        index_builds::primary_driven::metadataKey,
                                        {obj.objdata(), static_cast<size_t>(obj.objsize())}));
            wuow.commit();
            return true;
        });
}

}  // namespace

bool checkWithinAttemptLimit(OperationContext* opCtx,
                             const DatabaseName& dbName,
                             const UUID& buildUUID,
                             const UUID& collectionUUID,
                             const std::string& ident,
                             ResumeIndexInfo& resumeInfo,
                             bool plannedHandoff) {
    // Draining is exempt so unrelated failovers during a long drain do not abort the build.
    if (resumeInfo.getPhase() == IndexBuildPhaseEnum::kDrainWrites) {
        return true;
    }

    const int limit = gMaxIndexBuildAttemptsAfterUncleanShutdown.load();
    const auto prevResumesWithoutProgress = resumeInfo.getResumesWithoutProgress();
    // The first unplanned resume establishes a baseline at 0. Later unplanned resumes without
    // persisted progress increment it.
    const int64_t resumesWithoutProgress = prevResumesWithoutProgress
        ? static_cast<int64_t>(*prevResumesWithoutProgress) + (plannedHandoff ? 0 : 1)
        : 0;
    if (resumesWithoutProgress > limit) {
        LOGV2(13800500,
              "Index build: resumed too many times without progress, aborting",
              "buildUUID"_attr = buildUUID,
              "collectionUUID"_attr = collectionUUID,
              "resumesWithoutProgress"_attr = resumesWithoutProgress,
              "limit"_attr = limit,
              "phase"_attr = idl::serialize(resumeInfo.getPhase()));
        return false;
    }

    if (plannedHandoff) {
        LOGV2(13800506,
              "Index build: skipping resume count increment because we are in a planned handoff",
              "buildUUID"_attr = buildUUID,
              "collectionUUID"_attr = collectionUUID,
              "resumesWithoutProgress"_attr = resumesWithoutProgress,
              "limit"_attr = limit,
              "phase"_attr = idl::serialize(resumeInfo.getPhase()));
        return true;
    }

    if (recordResumesWithoutProgress(
            opCtx, dbName, ident, resumeInfo, static_cast<int>(resumesWithoutProgress))) {
        auto opTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
        uassertStatusOK(
            repl::ReplicationCoordinator::get(opCtx)
                ->awaitReplication(opCtx,
                                   opTime,
                                   WriteConcernOptions{WriteConcernOptions::kMajority,
                                                       WriteConcernOptions::SyncMode::UNSET,
                                                       WriteConcernOptions::kNoTimeout})
                .status);

        resumeInfo.setResumesWithoutProgress(static_cast<int>(resumesWithoutProgress));
        LOGV2(13800501,
              "Index build: recorded a resume attempt without progress",
              "buildUUID"_attr = buildUUID,
              "collectionUUID"_attr = collectionUUID,
              "resumesWithoutProgress"_attr = resumesWithoutProgress,
              "plannedHandoff"_attr = plannedHandoff);
    }
    return true;
}

}  // namespace primary_driven

}  // namespace mongo::index_builds::build_attempts
