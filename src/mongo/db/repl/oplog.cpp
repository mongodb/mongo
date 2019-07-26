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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/oplog.h"

#include <deque>
#include <memory>
#include <set>
#include <vector>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/capped_utils.h"
#include "mongo/db/catalog/coll_mod.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog/drop_database.h"
#include "mongo/db/catalog/drop_indexes.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version_parser.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/repl/apply_ops.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/dbcheck.h"
#include "mongo/db/repl/local_oplog_info.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/timestamp_block.h"
#include "mongo/db/repl/transaction_oplog_application.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/server_write_concern_metrics.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/platform/random.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/elapsed_tracker.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/file.h"
#include "mongo/util/log.h"
#include "mongo/util/str.h"

namespace mongo {

using std::endl;
using std::string;
using std::stringstream;
using std::unique_ptr;
using std::vector;

using IndexVersion = IndexDescriptor::IndexVersion;

namespace repl {
namespace {

MONGO_FAIL_POINT_DEFINE(sleepBetweenInsertOpTimeGenerationAndLogOp);

// Failpoint to block after a write and its oplog entry have been written to the storage engine and
// are visible, but before we have advanced 'lastApplied' for the write.
MONGO_FAIL_POINT_DEFINE(hangBeforeLogOpAdvancesLastApplied);

// so we can fail the same way
void checkOplogInsert(Status result) {
    massert(17322, str::stream() << "write to oplog failed: " << result.toString(), result.isOK());
}

bool shouldBuildInForeground(OperationContext* opCtx,
                             const BSONObj& index,
                             const NamespaceString& indexNss,
                             repl::OplogApplication::Mode mode) {
    if (mode == OplogApplication::Mode::kRecovering) {
        LOG(3) << "apply op: building background index " << index
               << " in the foreground because the node is in recovery";
        return true;
    }

    // Primaries should build indexes in the foreground because failures cannot be handled
    // by the background thread.
    const bool isPrimary =
        repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, indexNss);
    if (isPrimary) {
        LOG(3) << "apply op: not building background index " << index
               << " in a background thread because this is a primary";
        return true;
    }

    // Without hybrid builds enabled, indexes should build with the behavior of their specs.
    bool hybrid = IndexBuilder::canBuildInBackground();
    if (!hybrid) {
        return !index["background"].trueValue();
    }

    return false;
}


}  // namespace

void setOplogCollectionName(ServiceContext* service) {
    LocalOplogInfo::get(service)->setOplogCollectionName(service);
}

/**
 * Parse the given BSON array of BSON into a vector of BSON.
 */
StatusWith<std::vector<BSONObj>> parseBSONSpecsIntoVector(const BSONElement& bsonArrayElem,
                                                          const NamespaceString& nss) {
    invariant(bsonArrayElem.type() == Array);
    std::vector<BSONObj> vec;
    for (auto& bsonElem : bsonArrayElem.Array()) {
        if (bsonElem.type() != BSONType::Object) {
            return {ErrorCodes::TypeMismatch,
                    str::stream() << "The elements of '" << bsonArrayElem.fieldName()
                                  << "' array must be objects, but found "
                                  << typeName(bsonElem.type())};
        }
        BSONObjBuilder builder(bsonElem.Obj());
        builder.append("ns", nss.toString());
        vec.emplace_back(builder.obj());
    }
    return vec;
}

Status startIndexBuild(OperationContext* opCtx,
                       const NamespaceString& nss,
                       const UUID& collUUID,
                       const UUID& indexBuildUUID,
                       const BSONElement& indexesElem,
                       OplogApplication::Mode mode) {
    auto statusWithIndexes = parseBSONSpecsIntoVector(indexesElem, nss);
    if (!statusWithIndexes.isOK()) {
        return statusWithIndexes.getStatus();
    }

    IndexBuildsCoordinator::IndexBuildOptions indexBuildOptions;
    invariant(!indexBuildOptions.commitQuorum);
    indexBuildOptions.replSetAndNotPrimary = true;

    // We don't pass in a commit quorum here because secondary nodes don't have any knowledge of it.
    return IndexBuildsCoordinator::get(opCtx)
        ->startIndexBuild(opCtx,
                          nss.db(),
                          collUUID,
                          statusWithIndexes.getValue(),
                          indexBuildUUID,
                          /* This oplog entry is only replicated for two-phase index builds */
                          IndexBuildProtocol::kTwoPhase,
                          indexBuildOptions)
        .getStatus();
}

Status commitIndexBuild(OperationContext* opCtx,
                        const NamespaceString& nss,
                        const UUID& indexBuildUUID,
                        const BSONElement& indexesElem,
                        OplogApplication::Mode mode) {
    auto statusWithIndexes = parseBSONSpecsIntoVector(indexesElem, nss);
    if (!statusWithIndexes.isOK()) {
        return statusWithIndexes.getStatus();
    }
    return IndexBuildsCoordinator::get(opCtx)->commitIndexBuild(
        opCtx, statusWithIndexes.getValue(), indexBuildUUID);
}

Status abortIndexBuild(OperationContext* opCtx,
                       const UUID& indexBuildUUID,
                       OplogApplication::Mode mode) {
    // Wait until the index build finishes aborting.
    Future<void> abort = IndexBuildsCoordinator::get(opCtx)->abortIndexBuildByBuildUUID(
        indexBuildUUID, "abortIndexBuild oplog entry encountered");
    return abort.waitNoThrow();
}

void createIndexForApplyOps(OperationContext* opCtx,
                            const BSONObj& indexSpec,
                            const NamespaceString& indexNss,
                            IncrementOpsAppliedStatsFn incrementOpsAppliedStats,
                            OplogApplication::Mode mode) {
    // Lock the database if it's not locked.
    boost::optional<Lock::DBLock> dbLock;
    if (!opCtx->lockState()->isLocked()) {
        dbLock.emplace(opCtx, indexNss.db(), MODE_X);
    }
    // Check if collection exists.
    auto databaseHolder = DatabaseHolder::get(opCtx);
    auto db = databaseHolder->getDb(opCtx, indexNss.ns());
    auto indexCollection = db ? db->getCollection(opCtx, indexNss) : nullptr;
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Failed to create index due to missing collection: " << indexNss.ns(),
            indexCollection);

    OpCounters* opCounters = opCtx->writesAreReplicated() ? &globalOpCounters : &replOpCounters;
    opCounters->gotInsert();
    if (opCtx->writesAreReplicated()) {
        ServerWriteConcernMetrics::get(opCtx)->recordWriteConcernForInsert(
            opCtx->getWriteConcern());
    }

    const auto constraints =
        ReplicationCoordinator::get(opCtx)->shouldRelaxIndexConstraints(opCtx, indexNss)
        ? IndexBuilder::IndexConstraints::kRelax
        : IndexBuilder::IndexConstraints::kEnforce;

    const auto replicatedWrites = opCtx->writesAreReplicated()
        ? IndexBuilder::ReplicatedWrites::kReplicated
        : IndexBuilder::ReplicatedWrites::kUnreplicated;

    if (shouldBuildInForeground(opCtx, indexSpec, indexNss, mode)) {
        IndexBuilder builder(indexSpec, constraints, replicatedWrites);
        Status status = builder.buildInForeground(opCtx, db);
        uassertStatusOK(status);
    } else {
        Lock::TempRelease release(opCtx->lockState());
        // TempRelease cannot fail because no recursive locks should be taken.
        invariant(!opCtx->lockState()->isLocked());
        auto collUUID = indexCollection->uuid();
        auto indexBuildUUID = UUID::gen();
        auto indexBuildsCoordinator = IndexBuildsCoordinator::get(opCtx);

        // We don't pass in a commit quorum here because secondary nodes don't have any knowledge of
        // it.
        IndexBuildsCoordinator::IndexBuildOptions indexBuildOptions;
        invariant(!indexBuildOptions.commitQuorum);
        indexBuildOptions.replSetAndNotPrimary = true;

        // This spawns a new thread and returns immediately.
        MONGO_COMPILER_VARIABLE_UNUSED auto fut = uassertStatusOK(
            indexBuildsCoordinator->startIndexBuild(opCtx,
                                                    indexNss.db(),
                                                    collUUID,
                                                    {indexSpec},
                                                    indexBuildUUID,
                                                    IndexBuildProtocol::kSinglePhase,
                                                    indexBuildOptions));
    }

    opCtx->recoveryUnit()->abandonSnapshot();

    if (incrementOpsAppliedStats) {
        incrementOpsAppliedStats();
    }
}

/* we write to local.oplog.rs:
     { ts : ..., h: ..., v: ..., op: ..., etc }
   ts: an OpTime timestamp
   h: hash
   v: version
   op:
    "i" insert
    "u" update
    "d" delete
    "c" db cmd
    "n" no op
*/


/*
 * records - a vector of oplog records to be written.
 * timestamps - a vector of respective Timestamp objects for each oplog record.
 * oplogCollection - collection to be written to.
 * finalOpTime - the OpTime of the last oplog record.
 * wallTime - the wall clock time of the last oplog record.
 */
void _logOpsInner(OperationContext* opCtx,
                  const NamespaceString& nss,
                  std::vector<Record>* records,
                  const std::vector<Timestamp>& timestamps,
                  Collection* oplogCollection,
                  OpTime finalOpTime,
                  Date_t wallTime) {
    auto replCoord = ReplicationCoordinator::get(opCtx);
    if (nss.size() && replCoord->getReplicationMode() == ReplicationCoordinator::modeReplSet &&
        !replCoord->canAcceptWritesFor(opCtx, nss)) {
        uasserted(17405,
                  str::stream() << "logOp() but can't accept write to collection " << nss.ns());
    }

    checkOplogInsert(oplogCollection->insertDocumentsForOplog(opCtx, records, timestamps));

    // Set replCoord last optime only after we're sure the WUOW didn't abort and roll back.
    opCtx->recoveryUnit()->onCommit(
        [opCtx, replCoord, finalOpTime, wallTime](boost::optional<Timestamp> commitTime) {
            if (commitTime) {
                // The `finalOpTime` may be less than the `commitTime` if multiple oplog entries
                // are logging within one WriteUnitOfWork.
                invariant(finalOpTime.getTimestamp() <= *commitTime,
                          str::stream() << "Final OpTime: " << finalOpTime.toString()
                                        << ". Commit Time: " << commitTime->toString());
            }

            // Optionally hang before advancing lastApplied.
            if (MONGO_FAIL_POINT(hangBeforeLogOpAdvancesLastApplied)) {
                log() << "hangBeforeLogOpAdvancesLastApplied fail point enabled.";
                MONGO_FAIL_POINT_PAUSE_WHILE_SET_OR_INTERRUPTED(opCtx,
                                                                hangBeforeLogOpAdvancesLastApplied);
            }

            // Optimes on the primary should always represent consistent database states.
            replCoord->setMyLastAppliedOpTimeAndWallTimeForward(
                {finalOpTime, wallTime}, ReplicationCoordinator::DataConsistency::Consistent);

            // We set the last op on the client to 'finalOpTime', because that contains the
            // timestamp of the operation that the client actually performed.
            ReplClientInfo::forClient(opCtx->getClient()).setLastOp(opCtx, finalOpTime);
        });
}

OpTime logOp(OperationContext* opCtx, MutableOplogEntry* oplogEntry) {
    // All collections should have UUIDs now, so all insert, update, and delete oplog entries should
    // also have uuids. Some no-op (n) and command (c) entries may still elide the uuid field.
    invariant(oplogEntry->getUuid() || oplogEntry->getOpType() == OpTypeEnum::kNoop ||
                  oplogEntry->getOpType() == OpTypeEnum::kCommand,
              str::stream() << "Expected uuid for logOp with oplog entry: "
                            << redact(oplogEntry->toBSON()));

    auto replCoord = ReplicationCoordinator::get(opCtx);
    // For commands, the test below is on the command ns and therefore does not check for
    // specific namespaces such as system.profile. This is the caller's responsibility.
    if (replCoord->isOplogDisabledFor(opCtx, oplogEntry->getNss())) {
        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "retryable writes is not supported for unreplicated ns: "
                              << oplogEntry->getNss().ns(),
                !oplogEntry->getStatementId());
        return {};
    }

    auto oplogInfo = LocalOplogInfo::get(opCtx);
    // Obtain Collection exclusive intent write lock for non-document-locking storage engines.
    boost::optional<Lock::DBLock> dbWriteLock;
    boost::optional<Lock::CollectionLock> collWriteLock;
    if (!opCtx->getServiceContext()->getStorageEngine()->supportsDocLocking()) {
        dbWriteLock.emplace(opCtx, NamespaceString::kLocalDb, MODE_IX);
        collWriteLock.emplace(opCtx, oplogInfo->getOplogCollectionName(), MODE_IX);
    }

    // If an OpTime is not specified (i.e. isNull), a new OpTime will be assigned to the oplog entry
    // within the WUOW. If a new OpTime is assigned, it needs to be reset back to a null OpTime
    // before exiting this function so that the same oplog entry instance can be reused for logOp()
    // again. For example, if the WUOW gets aborted within a writeConflictRetry loop, we need to
    // reset the OpTime to null so a new OpTime will be assigned on retry.
    OplogSlot slot = oplogEntry->getOpTime();
    auto resetOpTimeGuard = makeGuard([&, resetOpTimeOnExit = bool(slot.isNull())] {
        if (resetOpTimeOnExit)
            oplogEntry->setOpTime(OplogSlot());
    });

    WriteUnitOfWork wuow(opCtx);
    if (slot.isNull()) {
        slot = oplogInfo->getNextOpTimes(opCtx, 1U)[0];
        // TODO: make the oplogEntry a const reference instead of using the guard.
        oplogEntry->setOpTime(slot);
    }

    auto oplog = oplogInfo->getCollection();
    auto wallClockTime = oplogEntry->getWallClockTime();
    invariant(wallClockTime);

    auto bsonOplogEntry = oplogEntry->toBSON();
    // The storage engine will assign the RecordId based on the "ts" field of the oplog entry, see
    // oploghack::extractKey.
    std::vector<Record> records{
        {RecordId(), RecordData(bsonOplogEntry.objdata(), bsonOplogEntry.objsize())}};
    std::vector<Timestamp> timestamps{slot.getTimestamp()};
    _logOpsInner(opCtx, oplogEntry->getNss(), &records, timestamps, oplog, slot, *wallClockTime);
    wuow.commit();
    return slot;
}

std::vector<OpTime> logInsertOps(OperationContext* opCtx,
                                 MutableOplogEntry* oplogEntryTemplate,
                                 std::vector<InsertStatement>::const_iterator begin,
                                 std::vector<InsertStatement>::const_iterator end) {
    invariant(begin != end);
    oplogEntryTemplate->setOpType(repl::OpTypeEnum::kInsert);

    auto nss = oplogEntryTemplate->getNss();
    auto replCoord = ReplicationCoordinator::get(opCtx);
    if (replCoord->isOplogDisabledFor(opCtx, nss)) {
        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "retryable writes is not supported for unreplicated ns: "
                              << nss.ns(),
                begin->stmtId == kUninitializedStmtId);
        return {};
    }

    const size_t count = end - begin;
    auto oplogInfo = LocalOplogInfo::get(opCtx);

    // Obtain Collection exclusive intent write lock for non-document-locking storage engines.
    boost::optional<Lock::DBLock> dbWriteLock;
    boost::optional<Lock::CollectionLock> collWriteLock;
    if (!opCtx->getServiceContext()->getStorageEngine()->supportsDocLocking()) {
        dbWriteLock.emplace(opCtx, NamespaceString::kLocalDb, MODE_IX);
        collWriteLock.emplace(opCtx, oplogInfo->getOplogCollectionName(), MODE_IX);
    }

    WriteUnitOfWork wuow(opCtx);

    std::vector<OpTime> opTimes(count);
    std::vector<Timestamp> timestamps(count);
    std::vector<BSONObj> bsonOplogEntries(count);
    std::vector<Record> records(count);
    for (size_t i = 0; i < count; i++) {
        // Make a copy from the template for each insert oplog entry.
        MutableOplogEntry oplogEntry = *oplogEntryTemplate;
        // Make a mutable copy.
        auto insertStatementOplogSlot = begin[i].oplogSlot;
        // Fetch optime now, if not already fetched.
        if (insertStatementOplogSlot.isNull()) {
            insertStatementOplogSlot = oplogInfo->getNextOpTimes(opCtx, 1U)[0];
        }
        oplogEntry.setObject(begin[i].doc);
        oplogEntry.setOpTime(insertStatementOplogSlot);

        OplogLink oplogLink;
        if (i > 0)
            oplogLink.prevOpTime = opTimes[i - 1];
        appendRetryableWriteInfo(opCtx, &oplogEntry, &oplogLink, begin[i].stmtId);

        opTimes[i] = insertStatementOplogSlot;
        timestamps[i] = insertStatementOplogSlot.getTimestamp();
        bsonOplogEntries[i] = oplogEntry.toBSON();
        // The storage engine will assign the RecordId based on the "ts" field of the oplog entry,
        // see oploghack::extractKey.
        records[i] = Record{
            RecordId(), RecordData(bsonOplogEntries[i].objdata(), bsonOplogEntries[i].objsize())};
    }

    MONGO_FAIL_POINT_BLOCK(sleepBetweenInsertOpTimeGenerationAndLogOp, customWait) {
        const BSONObj& data = customWait.getData();
        auto numMillis = data["waitForMillis"].numberInt();
        log() << "Sleeping for " << numMillis << "ms after receiving " << count << " optimes from "
              << opTimes.front() << " to " << opTimes.back();
        sleepmillis(numMillis);
    }

    invariant(!opTimes.empty());
    auto lastOpTime = opTimes.back();
    invariant(!lastOpTime.isNull());
    auto oplog = oplogInfo->getCollection();
    auto wallClockTime = oplogEntryTemplate->getWallClockTime();
    invariant(wallClockTime);
    _logOpsInner(opCtx, nss, &records, timestamps, oplog, lastOpTime, *wallClockTime);
    wuow.commit();
    return opTimes;
}

void appendRetryableWriteInfo(OperationContext* opCtx,
                              MutableOplogEntry* oplogEntry,
                              OplogLink* oplogLink,
                              StmtId stmtId) {
    // Not a retryable write.
    if (stmtId == kUninitializedStmtId)
        return;

    const auto txnParticipant = TransactionParticipant::get(opCtx);
    invariant(txnParticipant);
    oplogEntry->setSessionId(opCtx->getLogicalSessionId());
    oplogEntry->setTxnNumber(opCtx->getTxnNumber());
    oplogEntry->setStatementId(stmtId);
    if (oplogLink->prevOpTime.isNull()) {
        oplogLink->prevOpTime = txnParticipant.getLastWriteOpTime();
    }
    oplogEntry->setPrevWriteOpTimeInTransaction(oplogLink->prevOpTime);
    if (!oplogLink->preImageOpTime.isNull()) {
        oplogEntry->setPreImageOpTime(oplogLink->preImageOpTime);
    }
    if (!oplogLink->postImageOpTime.isNull()) {
        oplogEntry->setPostImageOpTime(oplogLink->postImageOpTime);
    }
}

namespace {
long long getNewOplogSizeBytes(OperationContext* opCtx, const ReplSettings& replSettings) {
    if (replSettings.getOplogSizeBytes() != 0) {
        return replSettings.getOplogSizeBytes();
    }
    /* not specified. pick a default size */
    ProcessInfo pi;
    if (pi.getAddrSize() == 32) {
        const auto sz = 50LL * 1024LL * 1024LL;
        LOG(3) << "32bit system; choosing " << sz << " bytes oplog";
        return sz;
    }
    // First choose a minimum size.

#if defined(__APPLE__)
    // typically these are desktops (dev machines), so keep it smallish
    const auto sz = 192 * 1024 * 1024;
    LOG(3) << "Apple system; choosing " << sz << " bytes oplog";
    return sz;
#else
    long long lowerBound = 0;
    double bytes = 0;
    if (opCtx->getClient()->getServiceContext()->getStorageEngine()->isEphemeral()) {
        // in memory: 50MB minimum size
        lowerBound = 50LL * 1024 * 1024;
        bytes = pi.getMemSizeMB() * 1024 * 1024;
        LOG(3) << "Ephemeral storage system; lowerBound: " << lowerBound << " bytes, " << bytes
               << " bytes total memory";
    } else {
        // disk: 990MB minimum size
        lowerBound = 990LL * 1024 * 1024;
        bytes = File::freeSpace(storageGlobalParams.dbpath);  //-1 if call not supported.
        LOG(3) << "Disk storage system; lowerBound: " << lowerBound << " bytes, " << bytes
               << " bytes free space on device";
    }
    long long fivePct = static_cast<long long>(bytes * 0.05);
    auto sz = std::max(fivePct, lowerBound);
    // we use 5% of free [disk] space up to 50GB (1TB free)
    const long long upperBound = 50LL * 1024 * 1024 * 1024;
    sz = std::min(sz, upperBound);
    return sz;
#endif
}

}  // namespace

void createOplog(OperationContext* opCtx,
                 const NamespaceString& oplogCollectionName,
                 bool isReplSet) {
    Lock::GlobalWrite lk(opCtx);

    const auto service = opCtx->getServiceContext();

    const ReplSettings& replSettings = ReplicationCoordinator::get(opCtx)->getSettings();

    OldClientContext ctx(opCtx, oplogCollectionName.ns());
    Collection* collection = ctx.db()->getCollection(opCtx, oplogCollectionName);

    if (collection) {
        if (replSettings.getOplogSizeBytes() != 0) {
            const CollectionOptions oplogOpts =
                DurableCatalog::get(opCtx)->getCollectionOptions(opCtx, oplogCollectionName);

            int o = (int)(oplogOpts.cappedSize / (1024 * 1024));
            int n = (int)(replSettings.getOplogSizeBytes() / (1024 * 1024));
            if (n != o) {
                stringstream ss;
                ss << "cmdline oplogsize (" << n << ") different than existing (" << o
                   << ") see: http://dochub.mongodb.org/core/increase-oplog";
                log() << ss.str() << endl;
                uasserted(13257, ss.str());
            }
        }
        acquireOplogCollectionForLogging(opCtx);
        if (!isReplSet)
            initTimestampFromOplog(opCtx, oplogCollectionName);
        return;
    }

    /* create an oplog collection, if it doesn't yet exist. */
    const auto sz = getNewOplogSizeBytes(opCtx, replSettings);

    log() << "******" << endl;
    log() << "creating replication oplog of size: " << (int)(sz / (1024 * 1024)) << "MB..." << endl;

    CollectionOptions options;
    options.capped = true;
    options.cappedSize = sz;
    options.autoIndexId = CollectionOptions::NO;

    writeConflictRetry(opCtx, "createCollection", oplogCollectionName.ns(), [&] {
        WriteUnitOfWork uow(opCtx);
        invariant(ctx.db()->createCollection(opCtx, oplogCollectionName, options));
        acquireOplogCollectionForLogging(opCtx);
        if (!isReplSet) {
            service->getOpObserver()->onOpMessage(opCtx, BSONObj());
        }
        uow.commit();
    });

    /* sync here so we don't get any surprising lag later when we try to sync */
    StorageEngine* storageEngine = service->getStorageEngine();
    storageEngine->flushAllFiles(opCtx, true);

    log() << "******" << endl;
}

void createOplog(OperationContext* opCtx) {
    const auto isReplSet = ReplicationCoordinator::get(opCtx)->getReplicationMode() ==
        ReplicationCoordinator::modeReplSet;
    createOplog(opCtx, LocalOplogInfo::get(opCtx)->getOplogCollectionName(), isReplSet);
}

std::vector<OplogSlot> getNextOpTimes(OperationContext* opCtx, std::size_t count) {
    return LocalOplogInfo::get(opCtx)->getNextOpTimes(opCtx, count);
}

// -------------------------------------

namespace {
NamespaceString parseNs(const string& ns, const BSONObj& cmdObj) {
    BSONElement first = cmdObj.firstElement();
    uassert(40073,
            str::stream() << "collection name has invalid type " << typeName(first.type()),
            first.canonicalType() == canonicalizeBSONType(mongo::String));
    std::string coll = first.valuestr();
    uassert(28635, "no collection name specified", !coll.empty());
    return NamespaceString(NamespaceString(ns).db().toString(), coll);
}

std::pair<OptionalCollectionUUID, NamespaceString> parseCollModUUIDAndNss(OperationContext* opCtx,
                                                                          const BSONElement& ui,
                                                                          const char* ns,
                                                                          BSONObj& cmd) {
    if (ui.eoo()) {
        return std::pair<OptionalCollectionUUID, NamespaceString>(boost::none, parseNs(ns, cmd));
    }
    CollectionUUID uuid = uassertStatusOK(UUID::parse(ui));
    auto& catalog = CollectionCatalog::get(opCtx);
    const auto nsByUUID = catalog.lookupNSSByUUID(uuid);
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Failed to apply operation due to missing collection (" << uuid
                          << "): " << redact(cmd.toString()),
            nsByUUID);
    return std::pair<OptionalCollectionUUID, NamespaceString>(uuid, *nsByUUID);
}

NamespaceString parseUUID(OperationContext* opCtx, const BSONElement& ui) {
    auto statusWithUUID = UUID::parse(ui);
    uassertStatusOK(statusWithUUID);
    auto uuid = statusWithUUID.getValue();
    auto& catalog = CollectionCatalog::get(opCtx);
    auto nss = catalog.lookupNSSByUUID(uuid);
    uassert(ErrorCodes::NamespaceNotFound, "No namespace with UUID " + uuid.toString(), nss);
    return *nss;
}

NamespaceString parseUUIDorNs(OperationContext* opCtx,
                              const char* ns,
                              const BSONElement& ui,
                              BSONObj& cmd) {
    return ui.ok() ? parseUUID(opCtx, ui) : parseNs(ns, cmd);
}

using OpApplyFn = std::function<Status(OperationContext* opCtx,
                                       const char* ns,
                                       const BSONElement& ui,
                                       BSONObj& cmd,
                                       const OpTime& opTime,
                                       const OplogEntry& entry,
                                       OplogApplication::Mode mode,
                                       boost::optional<Timestamp> stableTimestampForRecovery)>;

struct ApplyOpMetadata {
    OpApplyFn applyFunc;
    std::set<ErrorCodes::Error> acceptableErrors;

    ApplyOpMetadata(OpApplyFn fun) {
        applyFunc = fun;
    }

    ApplyOpMetadata(OpApplyFn fun, std::set<ErrorCodes::Error> theAcceptableErrors) {
        applyFunc = fun;
        acceptableErrors = theAcceptableErrors;
    }
};

const StringMap<ApplyOpMetadata> kOpsMap = {
    {"create",
     {[](OperationContext* opCtx,
         const char* ns,
         const BSONElement& ui,
         BSONObj& cmd,
         const OpTime& opTime,
         const OplogEntry& entry,
         OplogApplication::Mode mode,
         boost::optional<Timestamp> stableTimestampForRecovery) -> Status {
          const NamespaceString nss(parseNs(ns, cmd));
          Lock::DBLock dbXLock(opCtx, nss.db(), MODE_X);
          if (auto idIndexElem = cmd["idIndex"]) {
              // Remove "idIndex" field from command.
              auto cmdWithoutIdIndex = cmd.removeField("idIndex");
              return createCollectionForApplyOps(
                  opCtx, nss.db().toString(), ui, cmdWithoutIdIndex, idIndexElem.Obj());
          }

          // No _id index spec was provided, so we should build a v:1 _id index.
          BSONObjBuilder idIndexSpecBuilder;
          idIndexSpecBuilder.append(IndexDescriptor::kIndexVersionFieldName,
                                    static_cast<int>(IndexVersion::kV1));
          idIndexSpecBuilder.append(IndexDescriptor::kIndexNameFieldName, "_id_");
          idIndexSpecBuilder.append(IndexDescriptor::kNamespaceFieldName, nss.ns());
          idIndexSpecBuilder.append(IndexDescriptor::kKeyPatternFieldName, BSON("_id" << 1));
          return createCollectionForApplyOps(
              opCtx, nss.db().toString(), ui, cmd, idIndexSpecBuilder.done());
      },
      {ErrorCodes::NamespaceExists}}},
    {"createIndexes",
     {[](OperationContext* opCtx,
         const char* ns,
         const BSONElement& ui,
         BSONObj& cmd,
         const OpTime& opTime,
         const OplogEntry& entry,
         OplogApplication::Mode mode,
         boost::optional<Timestamp> stableTimestampForRecovery) -> Status {
          const NamespaceString nss(parseUUIDorNs(opCtx, ns, ui, cmd));
          BSONElement first = cmd.firstElement();
          invariant(first.fieldNameStringData() == "createIndexes");
          uassert(ErrorCodes::InvalidNamespace,
                  "createIndexes value must be a string",
                  first.type() == mongo::String);
          BSONObj indexSpec = cmd.removeField("createIndexes");
          // The UUID determines the collection to build the index on, so create new 'ns' field.
          BSONObj nsObj = BSON("ns" << nss.ns());
          indexSpec = indexSpec.addField(nsObj.firstElement());

          createIndexForApplyOps(opCtx, indexSpec, nss, {}, mode);
          return Status::OK();
      },
      {ErrorCodes::IndexAlreadyExists,
       ErrorCodes::IndexBuildAlreadyInProgress,
       ErrorCodes::NamespaceNotFound}}},
    {"startIndexBuild",
     {[](OperationContext* opCtx,
         const char* ns,
         const BSONElement& ui,
         BSONObj& cmd,
         const OpTime& opTime,
         const OplogEntry& entry,
         OplogApplication::Mode mode,
         boost::optional<Timestamp> stableTimestampForRecovery) -> Status {
         // {
         //     "startIndexBuild" : "coll",
         //     "indexBuildUUID" : <UUID>,
         //     "indexes" : [
         //         {
         //             "key" : {
         //                 "x" : 1
         //             },
         //             "name" : "x_1",
         //             "v" : 2
         //         },
         //         {
         //             "key" : {
         //                 "k" : 1
         //             },
         //             "name" : "k_1",
         //             "v" : 2
         //         }
         //     ]
         // }

         if (OplogApplication::Mode::kApplyOpsCmd == mode) {
             return {ErrorCodes::CommandNotSupported,
                     "The startIndexBuild operation is not supported in applyOps mode"};
         }

         const NamespaceString nss(parseUUIDorNs(opCtx, ns, ui, cmd));

         auto buildUUIDElem = cmd.getField("indexBuildUUID");
         uassert(ErrorCodes::BadValue,
                 "Error parsing 'startIndexBuild' oplog entry, missing required field "
                 "'indexBuildUUID'.",
                 !buildUUIDElem.eoo());
         UUID indexBuildUUID = uassertStatusOK(UUID::parse(buildUUIDElem));

         auto indexesElem = cmd.getField("indexes");
         uassert(ErrorCodes::BadValue,
                 "Error parsing 'startIndexBuild' oplog entry, missing required field 'indexes'.",
                 !indexesElem.eoo());
         uassert(ErrorCodes::BadValue,
                 "Error parsing 'startIndexBuild' oplog entry, field 'indexes' must be an array.",
                 indexesElem.type() == Array);

         auto collUUID = uassertStatusOK(UUID::parse(ui));

         return startIndexBuild(opCtx, nss, collUUID, indexBuildUUID, indexesElem, mode);
     }}},
    {"commitIndexBuild",
     {[](OperationContext* opCtx,
         const char* ns,
         const BSONElement& ui,
         BSONObj& cmd,
         const OpTime& opTime,
         const OplogEntry& entry,
         OplogApplication::Mode mode,
         boost::optional<Timestamp> stableTimestampForRecovery) -> Status {
         // {
         //     "commitIndexBuild" : "coll",
         //     "indexBuildUUID" : <UUID>,
         //     "indexes" : [
         //         {
         //             "key" : {
         //                 "x" : 1
         //             },
         //             "name" : "x_1",
         //             "v" : 2
         //         },
         //         {
         //             "key" : {
         //                 "k" : 1
         //             },
         //             "name" : "k_1",
         //             "v" : 2
         //         }
         //     ]
         // }

         if (OplogApplication::Mode::kApplyOpsCmd == mode) {
             return {ErrorCodes::CommandNotSupported,
                     "The commitIndexBuild operation is not supported in applyOps mode"};
         }

         // Ensure the collection name is specified
         BSONElement first = cmd.firstElement();
         invariant(first.fieldNameStringData() == "commitIndexBuild");
         uassert(ErrorCodes::InvalidNamespace,
                 "commitIndexBuild value must be a string",
                 first.type() == mongo::String);

         const NamespaceString nss(parseUUIDorNs(opCtx, ns, ui, cmd));

         auto buildUUIDElem = cmd.getField("indexBuildUUID");
         uassert(ErrorCodes::BadValue,
                 "Error parsing 'commitIndexBuild' oplog entry, missing required field "
                 "'indexBuildUUID'.",
                 !buildUUIDElem.eoo());
         UUID indexBuildUUID = uassertStatusOK(UUID::parse(buildUUIDElem));

         auto indexesElem = cmd.getField("indexes");
         uassert(ErrorCodes::BadValue,
                 "Error parsing 'commitIndexBuild' oplog entry, missing required field 'indexes'.",
                 !indexesElem.eoo());
         uassert(ErrorCodes::BadValue,
                 "Error parsing 'commitIndexBuild' oplog entry, field 'indexes' must be an array.",
                 indexesElem.type() == Array);

         return commitIndexBuild(opCtx, nss, indexBuildUUID, indexesElem, mode);
     }}},
    {"abortIndexBuild",
     {[](OperationContext* opCtx,
         const char* ns,
         const BSONElement& ui,
         BSONObj& cmd,
         const OpTime& opTme,
         const OplogEntry& entry,
         OplogApplication::Mode mode,
         boost::optional<Timestamp> stableTimestampForRecovery) -> Status {
         // {
         //     "abortIndexBuild" : "coll",
         //     "indexBuildUUID" : <UUID>,
         //     "indexes" : [
         //         {
         //             "key" : {
         //                 "x" : 1
         //             },
         //             "name" : "x_1",
         //             "v" : 2
         //         },
         //         {
         //             "key" : {
         //                 "k" : 1
         //             },
         //             "name" : "k_1",
         //             "v" : 2
         //         }
         //     ]
         // }

         if (OplogApplication::Mode::kApplyOpsCmd == mode) {
             return {ErrorCodes::CommandNotSupported,
                     "The abortIndexBuild operation is not supported in applyOps mode"};
         }

         // Ensure that the first element is the 'abortIndexBuild' field.
         BSONElement first = cmd.firstElement();
         invariant(first.fieldNameStringData() == "abortIndexBuild");
         uassert(ErrorCodes::InvalidNamespace,
                 "abortIndexBuild value must be a string specifying the collection name",
                 first.type() == mongo::String);

         auto buildUUIDElem = cmd.getField("indexBuildUUID");
         uassert(ErrorCodes::BadValue,
                 "Error parsing 'abortIndexBuild' oplog entry, missing required field "
                 "'indexBuildUUID'.",
                 buildUUIDElem.eoo());
         UUID indexBuildUUID = uassertStatusOK(UUID::parse(buildUUIDElem));

         // We require the indexes field to ensure that rollback via refetch knows the appropriate
         // indexes to rebuild.
         auto indexesElem = cmd.getField("indexes");
         uassert(ErrorCodes::BadValue,
                 "Error parsing 'abortIndexBuild' oplog entry, missing required field 'indexes'.",
                 indexesElem.eoo());
         uassert(ErrorCodes::BadValue,
                 "Error parsing 'abortIndexBuild' oplog entry, field 'indexes' must be an array of "
                 "index names.",
                 indexesElem.type() == Array);

         return abortIndexBuild(opCtx, indexBuildUUID, mode);
     }}},
    {"collMod",
     {[](OperationContext* opCtx,
         const char* ns,
         const BSONElement& ui,
         BSONObj& cmd,
         const OpTime& opTime,
         const OplogEntry& entry,
         OplogApplication::Mode mode,
         boost::optional<Timestamp> stableTimestampForRecovery) -> Status {
          NamespaceString nss;
          BSONObjBuilder resultWeDontCareAbout;
          std::tie(std::ignore, nss) = parseCollModUUIDAndNss(opCtx, ui, ns, cmd);
          return collMod(opCtx, nss, cmd, &resultWeDontCareAbout);
      },
      {ErrorCodes::IndexNotFound, ErrorCodes::NamespaceNotFound}}},
    {"dbCheck", {dbCheckOplogCommand, {}}},
    {"dropDatabase",
     {[](OperationContext* opCtx,
         const char* ns,
         const BSONElement& ui,
         BSONObj& cmd,
         const OpTime& opTime,
         const OplogEntry& entry,
         OplogApplication::Mode mode,
         boost::optional<Timestamp> stableTimestampForRecovery) -> Status {
          return dropDatabase(opCtx, NamespaceString(ns).db().toString());
      },
      {ErrorCodes::NamespaceNotFound}}},
    {"drop",
     {[](OperationContext* opCtx,
         const char* ns,
         const BSONElement& ui,
         BSONObj& cmd,
         const OpTime& opTime,
         const OplogEntry& entry,
         OplogApplication::Mode mode,
         boost::optional<Timestamp> stableTimestampForRecovery) -> Status {
          BSONObjBuilder resultWeDontCareAbout;
          auto nss = parseUUIDorNs(opCtx, ns, ui, cmd);
          if (nss.isDropPendingNamespace()) {
              log()
                  << "applyCommand: " << nss << " (UUID: " << ui.toString(false)
                  << "): collection is already in a drop-pending state: ignoring collection drop: "
                  << redact(cmd);
              return Status::OK();
          }
          return dropCollection(opCtx,
                                nss,
                                resultWeDontCareAbout,
                                opTime,
                                DropCollectionSystemCollectionMode::kAllowSystemCollectionDrops);
      },
      {ErrorCodes::NamespaceNotFound}}},
    // deleteIndex(es) is deprecated but still works as of April 10, 2015
    {"deleteIndex",
     {[](OperationContext* opCtx,
         const char* ns,
         const BSONElement& ui,
         BSONObj& cmd,
         const OpTime& opTime,
         const OplogEntry& entry,
         OplogApplication::Mode mode,
         boost::optional<Timestamp> stableTimestampForRecovery) -> Status {
          BSONObjBuilder resultWeDontCareAbout;
          return dropIndexes(opCtx, parseUUIDorNs(opCtx, ns, ui, cmd), cmd, &resultWeDontCareAbout);
      },
      {ErrorCodes::NamespaceNotFound, ErrorCodes::IndexNotFound}}},
    {"deleteIndexes",
     {[](OperationContext* opCtx,
         const char* ns,
         const BSONElement& ui,
         BSONObj& cmd,
         const OpTime& opTime,
         const OplogEntry& entry,
         OplogApplication::Mode mode,
         boost::optional<Timestamp> stableTimestampForRecovery) -> Status {
          BSONObjBuilder resultWeDontCareAbout;
          return dropIndexes(opCtx, parseUUIDorNs(opCtx, ns, ui, cmd), cmd, &resultWeDontCareAbout);
      },
      {ErrorCodes::NamespaceNotFound, ErrorCodes::IndexNotFound}}},
    {"dropIndex",
     {[](OperationContext* opCtx,
         const char* ns,
         const BSONElement& ui,
         BSONObj& cmd,
         const OpTime& opTime,
         const OplogEntry& entry,
         OplogApplication::Mode mode,
         boost::optional<Timestamp> stableTimestampForRecovery) -> Status {
          BSONObjBuilder resultWeDontCareAbout;
          return dropIndexes(opCtx, parseUUIDorNs(opCtx, ns, ui, cmd), cmd, &resultWeDontCareAbout);
      },
      {ErrorCodes::NamespaceNotFound, ErrorCodes::IndexNotFound}}},
    {"dropIndexes",
     {[](OperationContext* opCtx,
         const char* ns,
         const BSONElement& ui,
         BSONObj& cmd,
         const OpTime& opTime,
         const OplogEntry& entry,
         OplogApplication::Mode mode,
         boost::optional<Timestamp> stableTimestampForRecovery) -> Status {
          BSONObjBuilder resultWeDontCareAbout;
          return dropIndexes(opCtx, parseUUIDorNs(opCtx, ns, ui, cmd), cmd, &resultWeDontCareAbout);
      },
      {ErrorCodes::NamespaceNotFound, ErrorCodes::IndexNotFound}}},
    {"renameCollection",
     {[](OperationContext* opCtx,
         const char* ns,
         const BSONElement& ui,
         BSONObj& cmd,
         const OpTime& opTime,
         const OplogEntry& entry,
         OplogApplication::Mode mode,
         boost::optional<Timestamp> stableTimestampForRecovery) -> Status {
          return renameCollectionForApplyOps(opCtx, nsToDatabase(ns), ui, cmd, opTime);
      },
      {ErrorCodes::NamespaceNotFound, ErrorCodes::NamespaceExists}}},
    {"applyOps",
     {[](OperationContext* opCtx,
         const char* ns,
         const BSONElement& ui,
         BSONObj& cmd,
         const OpTime& opTime,
         const OplogEntry& entry,
         OplogApplication::Mode mode,
         boost::optional<Timestamp> stableTimestampForRecovery) -> Status {
         // The 'applyOps' is either an implicit prepare oplog entry or is an entry that was not
         // generated by a transaction. Partial and unprepared commit applyOps should
         // have been dispatched before this point.
         invariant(!entry.isPartialTransaction());
         return entry.shouldPrepare() ? applyPrepareTransaction(opCtx, entry, mode)
                                      : applyApplyOpsOplogEntry(opCtx, entry, mode);
     }}},
    {"convertToCapped",
     {[](OperationContext* opCtx,
         const char* ns,
         const BSONElement& ui,
         BSONObj& cmd,
         const OpTime& opTime,
         const OplogEntry& entry,
         OplogApplication::Mode mode,
         boost::optional<Timestamp> stableTimestampForRecovery) -> Status {
          convertToCapped(opCtx, parseUUIDorNs(opCtx, ns, ui, cmd), cmd["size"].number());
          return Status::OK();
      },
      {ErrorCodes::NamespaceNotFound}}},
    {"emptycapped",
     {[](OperationContext* opCtx,
         const char* ns,
         const BSONElement& ui,
         BSONObj& cmd,
         const OpTime& opTime,
         const OplogEntry& entry,
         OplogApplication::Mode mode,
         boost::optional<Timestamp> stableTimestampForRecovery) -> Status {
          return emptyCapped(opCtx, parseUUIDorNs(opCtx, ns, ui, cmd));
      },
      {ErrorCodes::NamespaceNotFound}}},
    {"commitTransaction",
     {[](OperationContext* opCtx,
         const char* ns,
         const BSONElement& ui,
         BSONObj& cmd,
         const OpTime& opTime,
         const OplogEntry& entry,
         OplogApplication::Mode mode,
         boost::optional<Timestamp> stableTimestampForRecovery) -> Status {
         return applyCommitTransaction(opCtx, entry, mode);
     }}},
    {"abortTransaction",
     {[](OperationContext* opCtx,
         const char* ns,
         const BSONElement& ui,
         BSONObj& cmd,
         const OpTime& opTime,
         const OplogEntry& entry,
         OplogApplication::Mode mode,
         boost::optional<Timestamp> stableTimestampForRecovery) -> Status {
         return applyAbortTransaction(opCtx, entry, mode);
     }}},
};

}  // namespace

constexpr StringData OplogApplication::kInitialSyncOplogApplicationMode;
constexpr StringData OplogApplication::kRecoveringOplogApplicationMode;
constexpr StringData OplogApplication::kSecondaryOplogApplicationMode;
constexpr StringData OplogApplication::kApplyOpsCmdOplogApplicationMode;

StringData OplogApplication::modeToString(OplogApplication::Mode mode) {
    switch (mode) {
        case OplogApplication::Mode::kInitialSync:
            return OplogApplication::kInitialSyncOplogApplicationMode;
        case OplogApplication::Mode::kRecovering:
            return OplogApplication::kRecoveringOplogApplicationMode;
        case OplogApplication::Mode::kSecondary:
            return OplogApplication::kSecondaryOplogApplicationMode;
        case OplogApplication::Mode::kApplyOpsCmd:
            return OplogApplication::kApplyOpsCmdOplogApplicationMode;
    }
    MONGO_UNREACHABLE;
}

StatusWith<OplogApplication::Mode> OplogApplication::parseMode(const std::string& mode) {
    if (mode == OplogApplication::kInitialSyncOplogApplicationMode) {
        return OplogApplication::Mode::kInitialSync;
    } else if (mode == OplogApplication::kRecoveringOplogApplicationMode) {
        return OplogApplication::Mode::kRecovering;
    } else if (mode == OplogApplication::kSecondaryOplogApplicationMode) {
        return OplogApplication::Mode::kSecondary;
    } else if (mode == OplogApplication::kApplyOpsCmdOplogApplicationMode) {
        return OplogApplication::Mode::kApplyOpsCmd;
    } else {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "Invalid oplog application mode provided: " << mode);
    }
    MONGO_UNREACHABLE;
}

// @return failure status if an update should have happened and the document DNE.
// See replset initial sync code.
Status applyOperation_inlock(OperationContext* opCtx,
                             Database* db,
                             const BSONObj& op,
                             bool alwaysUpsert,
                             OplogApplication::Mode mode,
                             IncrementOpsAppliedStatsFn incrementOpsAppliedStats) {
    LOG(3) << "applying op: " << redact(op)
           << ", oplog application mode: " << OplogApplication::modeToString(mode);

    // Choose opCounters based on running on standalone/primary or secondary by checking
    // whether writes are replicated. Atomic applyOps command is an exception, which runs
    // on primary/standalone but disables write replication.
    const bool shouldUseGlobalOpCounters =
        mode == repl::OplogApplication::Mode::kApplyOpsCmd || opCtx->writesAreReplicated();
    OpCounters* opCounters = shouldUseGlobalOpCounters ? &globalOpCounters : &replOpCounters;

    std::array<StringData, 8> names = {"ts", "t", "o", "ui", "ns", "op", "b", "o2"};
    std::array<BSONElement, 8> fields;
    op.getFields(names, &fields);
    BSONElement& fieldTs = fields[0];
    BSONElement& fieldT = fields[1];
    BSONElement& fieldO = fields[2];
    BSONElement& fieldUI = fields[3];
    BSONElement& fieldNs = fields[4];
    BSONElement& fieldOp = fields[5];
    BSONElement& fieldB = fields[6];
    BSONElement& fieldO2 = fields[7];

    BSONObj o;
    if (fieldO.isABSONObj())
        o = fieldO.embeddedObject();

    // operation type -- see logOp() comments for types
    const char* opType = fieldOp.valuestrsafe();

    if (*opType == 'n') {
        // no op
        if (incrementOpsAppliedStats) {
            incrementOpsAppliedStats();
        }

        return Status::OK();
    }

    NamespaceString requestNss;
    Collection* collection = nullptr;
    if (fieldUI) {
        CollectionCatalog& catalog = CollectionCatalog::get(opCtx);
        auto uuid = uassertStatusOK(UUID::parse(fieldUI));
        collection = catalog.lookupCollectionByUUID(uuid);
        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "Failed to apply operation due to missing collection (" << uuid
                              << "): " << redact(op.toString()),
                collection);
        requestNss = collection->ns();
        dassert(opCtx->lockState()->isCollectionLockedForMode(
            requestNss, supportsDocLocking() ? MODE_IX : MODE_X));
    } else {
        uassert(ErrorCodes::InvalidNamespace,
                "'ns' must be of type String",
                fieldNs.type() == BSONType::String);
        const StringData ns = fieldNs.valueStringDataSafe();
        requestNss = NamespaceString(ns);
        invariant(requestNss.coll().size());
        dassert(opCtx->lockState()->isCollectionLockedForMode(
                    requestNss, supportsDocLocking() ? MODE_IX : MODE_X),
                requestNss.ns());
        collection = db->getCollection(opCtx, requestNss);
    }

    // The feature compatibility version in the server configuration collection must not change
    // during initial sync.
    if ((mode == OplogApplication::Mode::kInitialSync) &&
        requestNss == NamespaceString::kServerConfigurationNamespace) {
        std::string oID;
        auto status = bsonExtractStringField(o, "_id", &oID);
        if (status.isOK() && oID == FeatureCompatibilityVersionParser::kParameterName) {
            return Status(ErrorCodes::OplogOperationUnsupported,
                          str::stream() << "Applying operation on feature compatibility version "
                                           "document not supported in initial sync: "
                                        << redact(op));
        }
    }

    BSONObj o2;
    if (fieldO2.isABSONObj())
        o2 = fieldO2.Obj();

    bool valueB = fieldB.booleanSafe();

    IndexCatalog* indexCatalog = collection == nullptr ? nullptr : collection->getIndexCatalog();
    const bool haveWrappingWriteUnitOfWork = opCtx->lockState()->inAWriteUnitOfWork();
    uassert(ErrorCodes::CommandNotSupportedOnView,
            str::stream() << "applyOps not supported on view: " << requestNss.ns(),
            collection || !ViewCatalog::get(db)->lookup(opCtx, requestNss.ns()));

    // This code must decide what timestamp the storage engine should make the upcoming writes
    // visible with. The requirements and use-cases:
    //
    // Requirement: A client calling the `applyOps` command must not be able to dictate timestamps
    //      that violate oplog ordering. Disallow this regardless of whether the timestamps chosen
    //      are otherwise legal.
    //
    // Use cases:
    //   Secondary oplog application: Use the timestamp in the operation document. These
    //     operations are replicated to the oplog and this is not nested in a parent
    //     `WriteUnitOfWork`.
    //
    //   Non-atomic `applyOps`: The server receives an `applyOps` command with a series of
    //     operations that cannot be run under a single transaction. The common exemption from
    //     being "transactionable" is containing a command operation. These will not be under a
    //     parent `WriteUnitOfWork`. We do not use the timestamps provided by the operations; if
    //     replicated, these operations will be assigned timestamps when logged in the oplog.
    //
    //   Atomic `applyOps`: The server receives an `applyOps` command with operations that can be
    //    run under a single transaction. In this case the caller has already opened a
    //    `WriteUnitOfWork` and expects all writes to become visible at the same time. Moreover,
    //    the individual operations will not contain a `ts` field. The caller is responsible for
    //    setting the timestamp before committing. Assigning a competing timestamp in this
    //    codepath would break that atomicity. Sharding is a consumer of this use-case.
    const bool assignOperationTimestamp = [opCtx, haveWrappingWriteUnitOfWork, mode] {
        const auto replMode = ReplicationCoordinator::get(opCtx)->getReplicationMode();
        if (opCtx->writesAreReplicated()) {
            // We do not assign timestamps on replicated writes since they will get their oplog
            // timestamp once they are logged.
            return false;
        } else if (haveWrappingWriteUnitOfWork) {
            // We do not assign timestamps to non-replicated writes that have a wrapping
            // WriteUnitOfWork, as they will get the timestamp on that WUOW.
            // The typical usage of this is for operations inside of atomic 'applyOps' commands
            // being applied on a secondary. They will get the timestamp of the outer 'applyOps'
            // oplog entry in their wrapper WUOW.
            // We also use a WUOW for replaying a prepared transaction when we encounter its
            // corresponding commitTransaction entry during recovery. We set the timestamp on the
            // WUOW to be the commit timestamp.
            return false;
        } else {
            switch (replMode) {
                case ReplicationCoordinator::modeReplSet: {
                    // We typically timestamp these writes, unless they are in a WUOW.
                    return true;
                }
                case ReplicationCoordinator::modeNone: {
                    // Only assign timestamps on standalones during replication recovery when
                    // started with the 'recoverFromOplogAsStandalone' flag.
                    return mode == OplogApplication::Mode::kRecovering;
                }
            }
        }
        MONGO_UNREACHABLE;
    }();
    invariant(!assignOperationTimestamp || !fieldTs.eoo(),
              str::stream() << "Oplog entry did not have 'ts' field when expected: " << redact(op));

    if (*opType == 'i') {
        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "Failed to apply insert due to missing collection: "
                              << op.toString(),
                collection);

        if (fieldO.type() == Array) {
            // Batched inserts.

            // Cannot apply an array insert with applyOps command.  No support for wiping out
            // the provided timestamps and using new ones for oplog.
            uassert(ErrorCodes::OperationFailed,
                    "Cannot apply an array insert with applyOps",
                    !opCtx->writesAreReplicated());

            uassert(ErrorCodes::BadValue,
                    "Expected array for field 'ts'",
                    fieldTs.ok() && fieldTs.type() == Array);
            uassert(ErrorCodes::BadValue,
                    "Expected array for field 't'",
                    fieldT.ok() && fieldT.type() == Array);

            uassert(ErrorCodes::OperationFailed,
                    str::stream() << "Failed to apply insert due to empty array element: "
                                  << op.toString(),
                    !fieldO.Obj().isEmpty() && !fieldTs.Obj().isEmpty() && !fieldT.Obj().isEmpty());

            std::vector<InsertStatement> insertObjs;
            auto fieldOIt = BSONObjIterator(fieldO.Obj());
            auto fieldTsIt = BSONObjIterator(fieldTs.Obj());
            auto fieldTIt = BSONObjIterator(fieldT.Obj());

            while (true) {
                auto oElem = fieldOIt.next();
                auto tsElem = fieldTsIt.next();
                auto tElem = fieldTIt.next();

                // Note: we don't care about statement ids here since the secondaries don't create
                // their own oplog entries.
                insertObjs.emplace_back(oElem.Obj(), tsElem.timestamp(), tElem.Long());
                if (!fieldOIt.more()) {
                    // Make sure arrays are the same length.
                    uassert(ErrorCodes::OperationFailed,
                            str::stream()
                                << "Failed to apply insert due to invalid array elements: "
                                << op.toString(),
                            !fieldTsIt.more());
                    break;
                }
                // Make sure arrays are the same length.
                uassert(ErrorCodes::OperationFailed,
                        str::stream() << "Failed to apply insert due to invalid array elements: "
                                      << op.toString(),
                        fieldTsIt.more());
            }

            WriteUnitOfWork wuow(opCtx);
            OpDebug* const nullOpDebug = nullptr;
            Status status = collection->insertDocuments(
                opCtx, insertObjs.begin(), insertObjs.end(), nullOpDebug, true);
            if (!status.isOK()) {
                return status;
            }
            wuow.commit();
            for (auto entry : insertObjs) {
                opCounters->gotInsert();
                if (shouldUseGlobalOpCounters) {
                    ServerWriteConcernMetrics::get(opCtx)->recordWriteConcernForInsert(
                        opCtx->getWriteConcern());
                }
                if (incrementOpsAppliedStats) {
                    incrementOpsAppliedStats();
                }
            }
        } else {
            // Single insert.
            opCounters->gotInsert();
            if (shouldUseGlobalOpCounters) {
                ServerWriteConcernMetrics::get(opCtx)->recordWriteConcernForInsert(
                    opCtx->getWriteConcern());
            }

            // No _id.
            // This indicates an issue with the upstream server:
            //     The oplog entry is corrupted; or
            //     The version of the upstream server is obsolete.
            uassert(ErrorCodes::NoSuchKey,
                    str::stream() << "Failed to apply insert due to missing _id: " << op.toString(),
                    o.hasField("_id"));

            // 1. Insert if
            //   a) we do not have a wrapping WriteUnitOfWork, which implies we are not part of an
            //      "applyOps" command, OR
            //   b) we are part of a multi-document transaction[1].
            //
            // 2. Upsert[2] if
            //   a) we have a wrapping WriteUnitOfWork AND we are not part of a transaction, which
            //      implies we are part of an "applyOps" command, OR
            //   b) the previous insert failed with a DuplicateKey error AND we are not part of a
            //      transaction.
            //
            // [1] Transactions should not convert inserts to upserts because on secondaries they
            //     will perform a lookup that never occurred on the primary. This may cause an
            //     unintended prepare conflict and block replication. For this reason, transactions
            //     should always fail with DuplicateKey errors and never retry inserts as upserts.
            // [2] This upsert behavior exists to support idempotency guarantees outside
            //     steady-state replication and existing users of applyOps.

            const auto txnParticipant = TransactionParticipant::get(opCtx);
            const bool inTxn = txnParticipant && txnParticipant.inMultiDocumentTransaction();
            bool needToDoUpsert = haveWrappingWriteUnitOfWork && !inTxn;

            Timestamp timestamp;
            long long term = OpTime::kUninitializedTerm;
            if (assignOperationTimestamp) {
                if (fieldTs) {
                    timestamp = fieldTs.timestamp();
                }
                if (fieldT) {
                    term = fieldT.Long();
                }
            }

            if (!needToDoUpsert) {
                WriteUnitOfWork wuow(opCtx);

                // Do not use supplied timestamps if running through applyOps, as that would allow
                // a user to dictate what timestamps appear in the oplog.
                if (assignOperationTimestamp) {
                    if (fieldTs.ok()) {
                        timestamp = fieldTs.timestamp();
                    }
                    if (fieldT.ok()) {
                        term = fieldT.Long();
                    }
                }

                OpDebug* const nullOpDebug = nullptr;
                Status status = collection->insertDocument(
                    opCtx, InsertStatement(o, timestamp, term), nullOpDebug, true);

                if (status.isOK()) {
                    wuow.commit();
                } else if (status == ErrorCodes::DuplicateKey) {
                    // Transactions cannot be retried as upserts once they fail with a duplicate key
                    // error.
                    if (inTxn) {
                        return status;
                    }
                    // Continue to the next block to retry the operation as an upsert.
                    needToDoUpsert = true;
                } else {
                    return status;
                }
            }

            // Now see if we need to do an upsert.
            if (needToDoUpsert) {
                // Do update on DuplicateKey errors.
                // This will only be on the _id field in replication,
                // since we disable non-_id unique constraint violations.
                BSONObjBuilder b;
                b.append(o.getField("_id"));

                UpdateRequest request(requestNss);
                request.setQuery(b.done());
                request.setUpdateModification(o);
                request.setUpsert();
                request.setFromOplogApplication(true);

                const StringData ns = fieldNs.valueStringDataSafe();
                writeConflictRetry(opCtx, "applyOps_upsert", ns, [&] {
                    WriteUnitOfWork wuow(opCtx);
                    // If this is an atomic applyOps (i.e: `haveWrappingWriteUnitOfWork` is true),
                    // do not timestamp the write.
                    if (assignOperationTimestamp && timestamp != Timestamp::min()) {
                        uassertStatusOK(opCtx->recoveryUnit()->setTimestamp(timestamp));
                    }

                    UpdateResult res = update(opCtx, db, request);
                    if (res.numMatched == 0 && res.upserted.isEmpty()) {
                        error() << "No document was updated even though we got a DuplicateKey "
                                   "error when inserting";
                        fassertFailedNoTrace(28750);
                    }
                    wuow.commit();
                });
            }

            if (incrementOpsAppliedStats) {
                incrementOpsAppliedStats();
            }
        }
    } else if (*opType == 'u') {
        opCounters->gotUpdate();
        if (shouldUseGlobalOpCounters) {
            ServerWriteConcernMetrics::get(opCtx)->recordWriteConcernForUpdate(
                opCtx->getWriteConcern());
        }

        auto idField = o2["_id"];
        uassert(ErrorCodes::NoSuchKey,
                str::stream() << "Failed to apply update due to missing _id: " << op.toString(),
                !idField.eoo());

        // The o2 field may contain additional fields besides the _id (like the shard key fields),
        // but we want to do the update by just _id so we can take advantage of the IDHACK.
        BSONObj updateCriteria = idField.wrap();
        const bool upsert = valueB || alwaysUpsert;

        UpdateRequest request(requestNss);
        request.setQuery(updateCriteria);
        request.setUpdateModification(o);
        request.setUpsert(upsert);
        request.setFromOplogApplication(true);

        Timestamp timestamp;
        if (assignOperationTimestamp) {
            timestamp = fieldTs.timestamp();
        }

        const StringData ns = fieldNs.valueStringDataSafe();
        auto status = writeConflictRetry(opCtx, "applyOps_update", ns, [&] {
            WriteUnitOfWork wuow(opCtx);
            if (timestamp != Timestamp::min()) {
                uassertStatusOK(opCtx->recoveryUnit()->setTimestamp(timestamp));
            }

            UpdateResult ur = update(opCtx, db, request);
            if (ur.numMatched == 0 && ur.upserted.isEmpty()) {
                if (ur.modifiers) {
                    if (updateCriteria.nFields() == 1) {
                        // was a simple { _id : ... } update criteria
                        string msg = str::stream() << "failed to apply update: " << redact(op);
                        error() << msg;
                        return Status(ErrorCodes::UpdateOperationFailed, msg);
                    }

                    // Need to check to see if it isn't present so we can exit early with a
                    // failure. Note that adds some overhead for this extra check in some cases,
                    // such as an updateCriteria of the form
                    // { _id:..., { x : {$size:...} }
                    // thus this is not ideal.
                    if (collection == nullptr ||
                        (indexCatalog->haveIdIndex(opCtx) &&
                         Helpers::findById(opCtx, collection, updateCriteria).isNull()) ||
                        // capped collections won't have an _id index
                        (!indexCatalog->haveIdIndex(opCtx) &&
                         Helpers::findOne(opCtx, collection, updateCriteria, false).isNull())) {
                        string msg = str::stream() << "couldn't find doc: " << redact(op);
                        error() << msg;
                        return Status(ErrorCodes::UpdateOperationFailed, msg);
                    }

                    // Otherwise, it's present; zero objects were updated because of additional
                    // specifiers in the query for idempotence
                } else {
                    // this could happen benignly on an oplog duplicate replay of an upsert
                    // (because we are idempotent),
                    // if an regular non-mod update fails the item is (presumably) missing.
                    if (!upsert) {
                        string msg = str::stream() << "update of non-mod failed: " << redact(op);
                        error() << msg;
                        return Status(ErrorCodes::UpdateOperationFailed, msg);
                    }
                }
            }

            wuow.commit();
            return Status::OK();
        });

        if (!status.isOK()) {
            return status;
        }

        if (incrementOpsAppliedStats) {
            incrementOpsAppliedStats();
        }
    } else if (*opType == 'd') {
        opCounters->gotDelete();
        if (shouldUseGlobalOpCounters) {
            ServerWriteConcernMetrics::get(opCtx)->recordWriteConcernForDelete(
                opCtx->getWriteConcern());
        }

        auto idField = o["_id"];
        uassert(ErrorCodes::NoSuchKey,
                str::stream() << "Failed to apply delete due to missing _id: " << op.toString(),
                !idField.eoo());

        // The o field may contain additional fields besides the _id (like the shard key fields),
        // but we want to do the delete by just _id so we can take advantage of the IDHACK.
        BSONObj deleteCriteria = idField.wrap();

        Timestamp timestamp;
        if (assignOperationTimestamp) {
            timestamp = fieldTs.timestamp();
        }

        const StringData ns = fieldNs.valueStringDataSafe();
        writeConflictRetry(opCtx, "applyOps_delete", ns, [&] {
            WriteUnitOfWork wuow(opCtx);
            if (timestamp != Timestamp::min()) {
                uassertStatusOK(opCtx->recoveryUnit()->setTimestamp(timestamp));
            }

            if (opType[1] == 0) {
                const auto justOne = true;
                deleteObjects(opCtx, collection, requestNss, deleteCriteria, justOne);
            } else
                verify(opType[1] == 'b');  // "db" advertisement
            wuow.commit();
        });

        if (incrementOpsAppliedStats) {
            incrementOpsAppliedStats();
        }
    } else {
        invariant(*opType != 'c');  // commands are processed in applyCommand_inlock()
        uasserted(14825, str::stream() << "error in applyOperation : unknown opType " << *opType);
    }

    return Status::OK();
}

Status applyCommand_inlock(OperationContext* opCtx,
                           const BSONObj& op,
                           const OplogEntry& entry,
                           OplogApplication::Mode mode,
                           boost::optional<Timestamp> stableTimestampForRecovery) {
    // We should only have a stableTimestampForRecovery during replication recovery.
    invariant(stableTimestampForRecovery == boost::none ||
              mode == OplogApplication::Mode::kRecovering);
    LOG(3) << "applying command op: " << redact(op)
           << ", oplog application mode: " << OplogApplication::modeToString(mode)
           << ", stable timestamp for recovery: " << stableTimestampForRecovery;

    std::array<StringData, 4> names = {"o", "ui", "ns", "op"};
    std::array<BSONElement, 4> fields;
    op.getFields(names, &fields);
    BSONElement& fieldO = fields[0];
    BSONElement& fieldUI = fields[1];
    BSONElement& fieldNs = fields[2];
    BSONElement& fieldOp = fields[3];

    const char* opType = fieldOp.valuestrsafe();
    invariant(*opType == 'c');  // only commands are processed here

    // Choose opCounters based on running on standalone/primary or secondary by checking
    // whether writes are replicated.
    OpCounters* opCounters = opCtx->writesAreReplicated() ? &globalOpCounters : &replOpCounters;
    opCounters->gotCommand();

    if (fieldO.eoo()) {
        return Status(ErrorCodes::NoSuchKey, "Missing expected field 'o'");
    }

    if (!fieldO.isABSONObj()) {
        return Status(ErrorCodes::BadValue, "Expected object for field 'o'");
    }

    BSONObj o = fieldO.embeddedObject();

    uassert(ErrorCodes::InvalidNamespace,
            "'ns' must be of type String",
            fieldNs.type() == BSONType::String);
    const NamespaceString nss(fieldNs.valueStringData());
    if (!nss.isValid()) {
        return {ErrorCodes::InvalidNamespace, "invalid ns: " + std::string(nss.ns())};
    }
    {
        // Command application doesn't always acquire the global writer lock for transaction
        // commands, so we acquire its own locks here.
        Lock::DBLock lock(opCtx, nss.db(), MODE_IS);
        auto databaseHolder = DatabaseHolder::get(opCtx);
        auto db = databaseHolder->getDb(opCtx, nss.ns());
        if (db && !db->getCollection(opCtx, nss) && ViewCatalog::get(db)->lookup(opCtx, nss.ns())) {
            return {ErrorCodes::CommandNotSupportedOnView,
                    str::stream() << "applyOps not supported on view:" << nss.ns()};
        }
    }

    // The feature compatibility version in the server configuration collection cannot change during
    // initial sync. We do not attempt to parse the whitelisted ops because they do not have a
    // collection namespace. If we drop the 'admin' database we will also log a 'drop' oplog entry
    // for each collection dropped. 'applyOps' and 'commitTransaction' will try to apply each
    // individual operation, and those will be caught then if they are a problem. 'abortTransaction'
    // won't ever change the server configuration collection.
    std::vector<std::string> whitelistedOps{
        "dropDatabase", "applyOps", "dbCheck", "commitTransaction", "abortTransaction"};
    if ((mode == OplogApplication::Mode::kInitialSync) &&
        (std::find(whitelistedOps.begin(), whitelistedOps.end(), o.firstElementFieldName()) ==
         whitelistedOps.end()) &&
        parseNs(nss.ns(), o) == NamespaceString::kServerConfigurationNamespace) {
        return Status(ErrorCodes::OplogOperationUnsupported,
                      str::stream() << "Applying command to feature compatibility version "
                                       "collection not supported in initial sync: "
                                    << redact(op));
    }

    // Parse optime from oplog entry unless we are applying this command in standalone or on a
    // primary (replicated writes enabled).
    OpTime opTime;
    if (!opCtx->writesAreReplicated()) {
        auto opTimeResult = OpTime::parseFromOplogEntry(op);
        if (opTimeResult.isOK()) {
            opTime = opTimeResult.getValue();
        }
    }

    const bool assignCommandTimestamp = [&] {
        const auto replMode = ReplicationCoordinator::get(opCtx)->getReplicationMode();
        if (opCtx->writesAreReplicated()) {
            // We do not assign timestamps on replicated writes since they will get their oplog
            // timestamp once they are logged.
            return false;
        }

        // Don't assign commit timestamp for transaction commands.
        const StringData commandName(o.firstElementFieldName());
        if (entry.shouldPrepare() ||
            entry.getCommandType() == OplogEntry::CommandType::kCommitTransaction ||
            entry.getCommandType() == OplogEntry::CommandType::kAbortTransaction)
            return false;

        switch (replMode) {
            case ReplicationCoordinator::modeReplSet: {
                // The 'applyOps' command never logs 'applyOps' oplog entries with nested
                // command operations, so this code will never be run from inside the 'applyOps'
                // command on secondaries. Thus, the timestamps in the command oplog
                // entries are always real timestamps from this oplog and we should
                // timestamp our writes with them.
                return true;
            }
            case ReplicationCoordinator::modeNone: {
                // Only assign timestamps on standalones during replication recovery when
                // started with 'recoverFromOplogAsStandalone'.
                return mode == OplogApplication::Mode::kRecovering;
            }
        }
        MONGO_UNREACHABLE;
    }();
    invariant(!assignCommandTimestamp || !opTime.isNull(),
              str::stream() << "Oplog entry did not have 'ts' field when expected: " << redact(op));

    const Timestamp writeTime = (assignCommandTimestamp ? opTime.getTimestamp() : Timestamp());

    bool done = false;
    while (!done) {
        auto op = kOpsMap.find(o.firstElementFieldName());
        if (op == kOpsMap.end()) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "Invalid key '" << o.firstElementFieldName()
                                        << "' found in field 'o'");
        }

        const ApplyOpMetadata& curOpToApply = op->second;

        Status status = [&] {
            try {
                // If 'writeTime' is not null, any writes in this scope will be given 'writeTime' as
                // their timestamp at commit.
                TimestampBlock tsBlock(opCtx, writeTime);
                return curOpToApply.applyFunc(opCtx,
                                              nss.ns().c_str(),
                                              fieldUI,
                                              o,
                                              opTime,
                                              entry,
                                              mode,
                                              stableTimestampForRecovery);
            } catch (const DBException& ex) {
                return ex.toStatus();
            }
        }();

        switch (status.code()) {
            case ErrorCodes::WriteConflict: {
                // Need to throw this up to a higher level where it will be caught and the
                // operation retried.
                throw WriteConflictException();
            }
            case ErrorCodes::BackgroundOperationInProgressForDatabase: {
                Lock::TempRelease release(opCtx->lockState());

                BackgroundOperation::awaitNoBgOpInProgForDb(nss.db());
                IndexBuildsCoordinator::get(opCtx)->awaitNoBgOpInProgForDb(nss.db());
                opCtx->recoveryUnit()->abandonSnapshot();
                opCtx->checkForInterrupt();
                break;
            }
            case ErrorCodes::BackgroundOperationInProgressForNamespace: {
                Lock::TempRelease release(opCtx->lockState());

                Command* cmd = CommandHelpers::findCommand(o.firstElement().fieldName());
                invariant(cmd);

                // TODO: This parse could be expensive and not worth it.
                auto ns =
                    cmd->parse(opCtx, OpMsgRequest::fromDBAndBody(nss.db(), o))->ns().toString();
                auto swUUID = UUID::parse(fieldUI);
                if (!swUUID.isOK()) {
                    error() << "Failed command " << redact(o) << " on " << ns << " with status "
                            << swUUID.getStatus() << "during oplog application. Expected a UUID.";
                }
                BackgroundOperation::awaitNoBgOpInProgForNs(ns);
                IndexBuildsCoordinator::get(opCtx)->awaitNoIndexBuildInProgressForCollection(
                    swUUID.getValue());

                opCtx->recoveryUnit()->abandonSnapshot();
                opCtx->checkForInterrupt();
                break;
            }
            default: {
                if (!curOpToApply.acceptableErrors.count(status.code())) {
                    error() << "Failed command " << redact(o) << " on " << nss.db()
                            << " with status " << status << " during oplog application";
                    return status;
                }
            }
            // fallthrough
            case ErrorCodes::OK:
                done = true;
                break;
        }
    }

    AuthorizationManager::get(opCtx->getServiceContext())->logOp(opCtx, opType, nss, o, nullptr);
    return Status::OK();
}

void setNewTimestamp(ServiceContext* service, const Timestamp& newTime) {
    LocalOplogInfo::get(service)->setNewTimestamp(service, newTime);
}

void initTimestampFromOplog(OperationContext* opCtx, const NamespaceString& oplogNss) {
    DBDirectClient c(opCtx);
    static const BSONObj reverseNaturalObj = BSON("$natural" << -1);
    BSONObj lastOp =
        c.findOne(oplogNss.ns(), Query().sort(reverseNaturalObj), nullptr, QueryOption_SlaveOk);

    if (!lastOp.isEmpty()) {
        LOG(1) << "replSet setting last Timestamp";
        const OpTime opTime = fassert(28696, OpTime::parseFromOplogEntry(lastOp));
        setNewTimestamp(opCtx->getServiceContext(), opTime.getTimestamp());
    }
}

void clearLocalOplogPtr() {
    LocalOplogInfo::get(getGlobalServiceContext())->resetCollection();
}

void acquireOplogCollectionForLogging(OperationContext* opCtx) {
    auto oplogInfo = LocalOplogInfo::get(opCtx);
    const auto& nss = oplogInfo->getOplogCollectionName();
    if (!nss.isEmpty()) {
        AutoGetCollection autoColl(opCtx, nss, MODE_IX);
        LocalOplogInfo::get(opCtx)->setCollection(autoColl.getCollection());
    }
}

void establishOplogCollectionForLogging(OperationContext* opCtx, Collection* oplog) {
    invariant(opCtx->lockState()->isW());
    invariant(oplog);
    LocalOplogInfo::get(opCtx)->setCollection(oplog);
}

void signalOplogWaiters() {
    auto oplog = LocalOplogInfo::get(getGlobalServiceContext())->getCollection();
    if (oplog) {
        oplog->getCappedCallback()->notifyCappedWaitersIfNeeded();
    }
}

}  // namespace repl
}  // namespace mongo
