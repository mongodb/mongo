// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index_builds/index_build_attempts.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index_builds/index_build_knobs_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/control/journal_flusher.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo::index_builds::build_attempts {
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

}  // namespace mongo::index_builds::build_attempts
