
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
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog/drop_database.h"
#include "mongo/db/catalog/drop_indexes.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/catalog/uuid_catalog.h"
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
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/repl/apply_ops.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/dbcheck.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/timestamp_block.h"
#include "mongo/db/repl/transaction_oplog_application.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/server_write_concern_metrics.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/platform/random.h"
#include "mongo/scripting/engine.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/elapsed_tracker.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/file.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/startup_test.h"

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

/**
 * This structure contains per-service-context state related to the oplog.
 */
struct LocalOplogInfo {
    MONGO_DISALLOW_COPYING(LocalOplogInfo);
    LocalOplogInfo() = default;

    // Name of the oplog collection.
    std::string oplogName;

    // The "oplog" pointer is always valid (or null) because an operation must take the global
    // exclusive lock to set the pointer to null when the Collection instance is destroyed. See
    // "oplogCheckCloseDatabase".
    Collection* oplog = nullptr;

    // Synchronizes the section where a new Timestamp is generated and when it is registered in the
    // storage engine.
    stdx::mutex newOpMutex;

    // Used to generate "h" fields in pv0. Synchronized by newOpMutex.
    PseudoRandom hashGenerator{std::unique_ptr<SecureRandom>(SecureRandom::create())->nextInt64()};
};

const auto localOplogInfo = ServiceContext::declareDecoration<LocalOplogInfo>();

// so we can fail the same way
void checkOplogInsert(Status result) {
    massert(17322, str::stream() << "write to oplog failed: " << result.toString(), result.isOK());
}

void _getNextOpTimes(OperationContext* opCtx,
                     Collection* oplog,
                     std::size_t count,
                     OplogSlot* slotsOut,
                     bool persist = true) {
    auto& oplogInfo = localOplogInfo(opCtx->getServiceContext());
    auto replCoord = ReplicationCoordinator::get(opCtx);
    long long term = OpTime::kUninitializedTerm;

    // Fetch term out of the newOpMutex.
    if (replCoord->getReplicationMode() == ReplicationCoordinator::modeReplSet) {
        // Current term. If we're not a replset of pv=1, it remains kOldProtocolVersionTerm.
        term = replCoord->getTerm();
    }

    // Allow the storage engine to start the transaction outside the critical section.
    opCtx->recoveryUnit()->preallocateSnapshot();
    stdx::lock_guard<stdx::mutex> lk(oplogInfo.newOpMutex);

    auto ts = LogicalClock::get(opCtx)->reserveTicks(count).asTimestamp();
    const bool orderedCommit = false;

    if (persist) {
        fassert(28560, oplog->getRecordStore()->oplogDiskLocRegister(opCtx, ts, orderedCommit));
    }

    for (std::size_t i = 0; i < count; i++) {
        slotsOut[i].opTime = {Timestamp(ts.asULL() + i), term};
        slotsOut[i].hash = oplogInfo.hashGenerator.nextInt64();
    }
}

/**
 * This allows us to stream the oplog entry directly into data region
 * main goal is to avoid copying the o portion
 * which can be very large
 * TODO: can have this build the entire doc
 */
class OplogDocWriter final : public DocWriter {
public:
    OplogDocWriter(BSONObj frame, BSONObj oField)
        : _frame(std::move(frame)), _oField(std::move(oField)) {}

    void writeDocument(char* start) const {
        char* buf = start;

        memcpy(buf, _frame.objdata(), _frame.objsize() - 1);  // don't copy final EOO

        DataView(buf).write<LittleEndian<int>>(documentSize());

        buf += (_frame.objsize() - 1);
        buf[0] = (char)Object;
        buf[1] = 'o';
        buf[2] = 0;
        memcpy(buf + 3, _oField.objdata(), _oField.objsize());
        buf += 3 + _oField.objsize();
        buf[0] = EOO;

        verify(static_cast<size_t>((buf + 1) - start) == documentSize());  // DEV?
    }

    size_t documentSize() const {
        return _frame.objsize() + _oField.objsize() + 1 /* type */ + 2 /* "o" */;
    }

private:
    BSONObj _frame;
    BSONObj _oField;
};

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
    switch (ReplicationCoordinator::get(service)->getReplicationMode()) {
        case ReplicationCoordinator::modeReplSet:
            localOplogInfo(service).oplogName = NamespaceString::kRsOplogNamespace.ns();
            break;
        case ReplicationCoordinator::modeNone:
            // leave empty.
            break;
    }
}

/**
 * Parse the given BSON array of BSON into a vector of BSON.
 */
StatusWith<std::vector<BSONObj>> parseBSONArrayIntoVector(const BSONElement& bsonArrayElem) {
    invariant(bsonArrayElem.type() == Array);
    std::vector<BSONObj> vec;
    for (auto& bsonElem : bsonArrayElem.Obj()) {
        if (bsonElem.type() != BSONType::Object) {
            return {ErrorCodes::TypeMismatch,
                    str::stream() << "The elements of '" << bsonArrayElem.fieldName()
                                  << "' array must be objects, but found "
                                  << typeName(bsonElem.type())};
        }
        BSONObjBuilder builder;
        builder.append(bsonElem);
        vec.emplace_back(builder.obj());
    }
    return vec;
}

Status startIndexBuild(OperationContext* opCtx,
                       const UUID& collUUID,
                       const UUID& indexBuildUUID,
                       const BSONElement& indexesElem,
                       OplogApplication::Mode mode) {
    auto statusWithIndexes = parseBSONArrayIntoVector(indexesElem);
    if (!statusWithIndexes.isOK()) {
        return statusWithIndexes.getStatus();
    }
    return IndexBuildsCoordinator::get(opCtx)
        ->startIndexBuild(opCtx, collUUID, statusWithIndexes.getValue(), indexBuildUUID)
        .getStatus();
}

Status commitIndexBuild(OperationContext* opCtx,
                        const UUID& indexBuildUUID,
                        const BSONElement& indexesElem,
                        OplogApplication::Mode mode) {
    auto statusWithIndexes = parseBSONArrayIntoVector(indexesElem);
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

        IndexBuilder* builder = new IndexBuilder(
            indexSpec, constraints, replicatedWrites, opCtx->recoveryUnit()->getCommitTimestamp());
        // This spawns a new thread and returns immediately.
        builder->go();
        // Wait for thread to start and register itself
        IndexBuilder::waitForBgIndexStarting();
    }

    opCtx->recoveryUnit()->abandonSnapshot();

    if (incrementOpsAppliedStats) {
        incrementOpsAppliedStats();
    }
}

namespace {

/**
 * Attaches the session information of a write to an oplog entry if it exists.
 */
void appendSessionInfo(OperationContext* opCtx,
                       BSONObjBuilder* builder,
                       StmtId statementId,
                       const OperationSessionInfo& sessionInfo,
                       const OplogLink& oplogLink) {
    if (!sessionInfo.getTxnNumber()) {
        return;
    }

    // Note: certain operations, like implicit collection creation will not have a stmtId.
    if (statementId == kUninitializedStmtId) {
        return;
    }

    sessionInfo.serialize(builder);

    builder->append(OplogEntryBase::kStatementIdFieldName, statementId);
    oplogLink.prevOpTime.append(builder,
                                OplogEntryBase::kPrevWriteOpTimeInTransactionFieldName.toString());

    if (!oplogLink.preImageOpTime.isNull()) {
        oplogLink.preImageOpTime.append(builder,
                                        OplogEntryBase::kPreImageOpTimeFieldName.toString());
    }

    if (!oplogLink.postImageOpTime.isNull()) {
        oplogLink.postImageOpTime.append(builder,
                                         OplogEntryBase::kPostImageOpTimeFieldName.toString());
    }
}

OplogDocWriter _logOpWriter(OperationContext* opCtx,
                            const char* opstr,
                            const NamespaceString& nss,
                            OptionalCollectionUUID uuid,
                            const BSONObj& obj,
                            const BSONObj* o2,
                            bool fromMigrate,
                            OpTime optime,
                            long long hashNew,
                            Date_t wallTime,
                            const OperationSessionInfo& sessionInfo,
                            StmtId statementId,
                            const OplogLink& oplogLink,
                            bool prepare) {
    BSONObjBuilder b(256);

    b.append("ts", optime.getTimestamp());
    if (optime.getTerm() != -1)
        b.append("t", optime.getTerm());
    b.append("h", hashNew);
    b.append("v", OplogEntry::kOplogVersion);
    b.append("op", opstr);
    b.append("ns", nss.ns());
    if (uuid)
        uuid->appendToBuilder(&b, "ui");

    if (fromMigrate)
        b.appendBool("fromMigrate", true);

    if (o2)
        b.append("o2", *o2);

    invariant(wallTime != Date_t{});
    b.appendDate("wall", wallTime);

    appendSessionInfo(opCtx, &b, statementId, sessionInfo, oplogLink);

    if (prepare) {
        b.appendBool(OplogEntryBase::kPrepareFieldName, true);
    }

    return OplogDocWriter(OplogDocWriter(b.obj(), obj));
}
}  // end anon namespace

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
 * writers - an array with size nDocs of DocWriter objects.
 * timestamps - an array with size nDocs of respective Timestamp objects for each DocWriter.
 * oplogCollection - collection to be written to.
  * finalOpTime - the OpTime of the last DocWriter object.
 */
void _logOpsInner(OperationContext* opCtx,
                  const NamespaceString& nss,
                  const DocWriter* const* writers,
                  Timestamp* timestamps,
                  size_t nDocs,
                  Collection* oplogCollection,
                  OpTime finalOpTime) {
    auto replCoord = ReplicationCoordinator::get(opCtx);
    if (nss.size() && replCoord->getReplicationMode() == ReplicationCoordinator::modeReplSet &&
        !replCoord->canAcceptWritesFor(opCtx, nss)) {
        uasserted(17405,
                  str::stream() << "logOp() but can't accept write to collection " << nss.ns());
    }

    // we jump through a bunch of hoops here to avoid copying the obj buffer twice --
    // instead we do a single copy to the destination in the record store.
    checkOplogInsert(oplogCollection->insertDocumentsForOplog(opCtx, writers, timestamps, nDocs));

    // Set replCoord last optime only after we're sure the WUOW didn't abort and roll back.
    opCtx->recoveryUnit()->onCommit(
        [opCtx, replCoord, finalOpTime](boost::optional<Timestamp> commitTime) {
            if (commitTime) {
                // The `finalOpTime` may be less than the `commitTime` if multiple oplog entries
                // are logging within one WriteUnitOfWork.
                invariant(finalOpTime.getTimestamp() <= *commitTime,
                          str::stream() << "Final OpTime: " << finalOpTime.toString()
                                        << ". Commit Time: "
                                        << commitTime->toString());
            }

            // Optimes on the primary should always represent consistent database states.
            replCoord->setMyLastAppliedOpTimeForward(
                finalOpTime, ReplicationCoordinator::DataConsistency::Consistent);

            // We set the last op on the client to 'finalOpTime', because that contains the
            // timestamp of the operation that the client actually performed.
            ReplClientInfo::forClient(opCtx->getClient()).setLastOp(finalOpTime);
        });
}

OpTime logOp(OperationContext* opCtx,
             const char* opstr,
             const NamespaceString& nss,
             OptionalCollectionUUID uuid,
             const BSONObj& obj,
             const BSONObj* o2,
             bool fromMigrate,
             Date_t wallClockTime,
             const OperationSessionInfo& sessionInfo,
             StmtId statementId,
             const OplogLink& oplogLink,
             bool prepare,
             const OplogSlot& oplogSlot) {
    // All collections should have UUIDs now, so all insert, update, and delete oplog entries should
    // also have uuids. Some no-op (n) and command (c) entries may still elide the uuid field.
    invariant(uuid || 'n' == *opstr || 'c' == *opstr,
              str::stream() << "Expected uuid for logOp with opstr: " << opstr << ", nss: "
                            << nss.ns()
                            << ", obj: "
                            << obj
                            << ", os: "
                            << o2);

    auto replCoord = ReplicationCoordinator::get(opCtx);
    // For commands, the test below is on the command ns and therefore does not check for
    // specific namespaces such as system.profile. This is the caller's responsibility.
    if (replCoord->isOplogDisabledFor(opCtx, nss)) {
        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "retryable writes is not supported for unreplicated ns: "
                              << nss.ns(),
                statementId == kUninitializedStmtId);
        return {};
    }

    const auto& oplogInfo = localOplogInfo(opCtx->getServiceContext());

    // Obtain Collection exclusive intent write lock for non-document-locking storage engines.
    boost::optional<Lock::DBLock> dbWriteLock;
    boost::optional<Lock::CollectionLock> collWriteLock;
    if (!opCtx->getServiceContext()->getStorageEngine()->supportsDocLocking()) {
        dbWriteLock.emplace(opCtx, NamespaceString::kLocalDb, MODE_IX);
        collWriteLock.emplace(opCtx->lockState(), oplogInfo.oplogName, MODE_IX);
    }

    auto const oplog = oplogInfo.oplog;
    OplogSlot slot;
    WriteUnitOfWork wuow(opCtx);
    if (oplogSlot.opTime.isNull()) {
        _getNextOpTimes(opCtx, oplog, 1, &slot);
    } else {
        slot = oplogSlot;
    }

    auto writer = _logOpWriter(opCtx,
                               opstr,
                               nss,
                               uuid,
                               obj,
                               o2,
                               fromMigrate,
                               slot.opTime,
                               slot.hash,
                               wallClockTime,
                               sessionInfo,
                               statementId,
                               oplogLink,
                               prepare);
    const DocWriter* basePtr = &writer;
    auto timestamp = slot.opTime.getTimestamp();
    _logOpsInner(opCtx, nss, &basePtr, &timestamp, 1, oplog, slot.opTime);
    wuow.commit();
    return slot.opTime;
}

std::vector<OpTime> logInsertOps(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 OptionalCollectionUUID uuid,
                                 std::vector<InsertStatement>::const_iterator begin,
                                 std::vector<InsertStatement>::const_iterator end,
                                 bool fromMigrate,
                                 Date_t wallClockTime) {
    invariant(begin != end);

    auto replCoord = ReplicationCoordinator::get(opCtx);
    if (replCoord->isOplogDisabledFor(opCtx, nss)) {
        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "retryable writes is not supported for unreplicated ns: "
                              << nss.ns(),
                begin->stmtId == kUninitializedStmtId);
        return {};
    }

    const size_t count = end - begin;
    std::vector<OplogDocWriter> writers;
    writers.reserve(count);
    const auto& oplogInfo = localOplogInfo(opCtx->getServiceContext());

    // Obtain Collection exclusive intent write lock for non-document-locking storage engines.
    boost::optional<Lock::DBLock> dbWriteLock;
    boost::optional<Lock::CollectionLock> collWriteLock;
    if (!opCtx->getServiceContext()->getStorageEngine()->supportsDocLocking()) {
        dbWriteLock.emplace(opCtx, NamespaceString::kLocalDb, MODE_IX);
        collWriteLock.emplace(opCtx->lockState(), oplogInfo.oplogName, MODE_IX);
    }

    auto oplog = oplogInfo.oplog;
    WriteUnitOfWork wuow(opCtx);

    OperationSessionInfo sessionInfo;
    OplogLink oplogLink;

    const auto txnParticipant = TransactionParticipant::get(opCtx);
    if (txnParticipant) {
        sessionInfo.setSessionId(*opCtx->getLogicalSessionId());
        sessionInfo.setTxnNumber(*opCtx->getTxnNumber());
        oplogLink.prevOpTime = txnParticipant->getLastWriteOpTime();
    }

    auto timestamps = stdx::make_unique<Timestamp[]>(count);
    std::vector<OpTime> opTimes;
    for (size_t i = 0; i < count; i++) {
        // Make a mutable copy.
        auto insertStatementOplogSlot = begin[i].oplogSlot;
        // Fetch optime now, if not already fetched.
        if (insertStatementOplogSlot.opTime.isNull()) {
            _getNextOpTimes(opCtx, oplog, 1, &insertStatementOplogSlot);
        }
        // Only 'applyOps' oplog entries can be prepared.
        constexpr bool prepare = false;
        writers.emplace_back(_logOpWriter(opCtx,
                                          "i",
                                          nss,
                                          uuid,
                                          begin[i].doc,
                                          NULL,
                                          fromMigrate,
                                          insertStatementOplogSlot.opTime,
                                          insertStatementOplogSlot.hash,
                                          wallClockTime,
                                          sessionInfo,
                                          begin[i].stmtId,
                                          oplogLink,
                                          prepare));
        oplogLink.prevOpTime = insertStatementOplogSlot.opTime;
        timestamps[i] = oplogLink.prevOpTime.getTimestamp();
        opTimes.push_back(insertStatementOplogSlot.opTime);
    }

    MONGO_FAIL_POINT_BLOCK(sleepBetweenInsertOpTimeGenerationAndLogOp, customWait) {
        const BSONObj& data = customWait.getData();
        auto numMillis = data["waitForMillis"].numberInt();
        log() << "Sleeping for " << numMillis << "ms after receiving " << count << " optimes from "
              << opTimes.front() << " to " << opTimes.back();
        sleepmillis(numMillis);
    }

    std::unique_ptr<DocWriter const* []> basePtrs(new DocWriter const*[count]);
    for (size_t i = 0; i < count; i++) {
        basePtrs[i] = &writers[i];
    }

    invariant(!opTimes.empty());
    auto lastOpTime = opTimes.back();
    invariant(!lastOpTime.isNull());
    _logOpsInner(opCtx, nss, basePtrs.get(), timestamps.get(), count, oplog, lastOpTime);
    wuow.commit();
    return opTimes;
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

void createOplog(OperationContext* opCtx, const std::string& oplogCollectionName, bool isReplSet) {
    Lock::GlobalWrite lk(opCtx);

    const auto service = opCtx->getServiceContext();

    const ReplSettings& replSettings = ReplicationCoordinator::get(opCtx)->getSettings();

    OldClientContext ctx(opCtx, oplogCollectionName);
    Collection* collection = ctx.db()->getCollection(opCtx, oplogCollectionName);

    if (collection) {
        if (replSettings.getOplogSizeBytes() != 0) {
            const CollectionOptions oplogOpts =
                collection->getCatalogEntry()->getCollectionOptions(opCtx);

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

    writeConflictRetry(opCtx, "createCollection", oplogCollectionName, [&] {
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
    createOplog(opCtx, localOplogInfo(opCtx->getServiceContext()).oplogName, isReplSet);
}

MONGO_REGISTER_SHIM(GetNextOpTimeClass::getNextOpTime)(OperationContext* opCtx)->OplogSlot {
    // The local oplog collection pointer must already be established by this point.
    // We can't establish it here because that would require locking the local database, which would
    // be a lock order violation.
    auto oplog = localOplogInfo(opCtx->getServiceContext()).oplog;
    invariant(oplog);
    OplogSlot os;
    _getNextOpTimes(opCtx, oplog, 1, &os);
    return os;
}

OplogSlot getNextOpTimeNoPersistForTesting(OperationContext* opCtx) {
    auto oplog = localOplogInfo(opCtx->getServiceContext()).oplog;
    invariant(oplog);
    OplogSlot os;
    bool persist = false;  // Don't update the storage engine with the allocated OpTime.
    _getNextOpTimes(opCtx, oplog, 1, &os, persist);
    return os;
}

std::vector<OplogSlot> getNextOpTimes(OperationContext* opCtx, std::size_t count) {
    // The local oplog collection pointer must already be established by this point.
    // We can't establish it here because that would require locking the local database, which would
    // be a lock order violation.
    auto oplog = localOplogInfo(opCtx->getServiceContext()).oplog;
    invariant(oplog);
    std::vector<OplogSlot> oplogSlots(count);
    auto oplogSlot = oplogSlots.begin();
    _getNextOpTimes(opCtx, oplog, count, &(*oplogSlot));
    return oplogSlots;
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
    auto& catalog = UUIDCatalog::get(opCtx);
    const auto nsByUUID = catalog.lookupNSSByUUID(uuid);
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Failed to apply operation due to missing collection (" << uuid
                          << "): "
                          << redact(cmd.toString()),
            !nsByUUID.isEmpty());
    return std::pair<OptionalCollectionUUID, NamespaceString>(uuid, nsByUUID);
}

NamespaceString parseUUID(OperationContext* opCtx, const BSONElement& ui) {
    auto statusWithUUID = UUID::parse(ui);
    uassertStatusOK(statusWithUUID);
    auto uuid = statusWithUUID.getValue();
    auto& catalog = UUIDCatalog::get(opCtx);
    auto nss = catalog.lookupNSSByUUID(uuid);
    uassert(
        ErrorCodes::NamespaceNotFound, "No namespace with UUID " + uuid.toString(), !nss.isEmpty());
    return nss;
}

NamespaceString parseUUIDorNs(OperationContext* opCtx,
                              const char* ns,
                              const BSONElement& ui,
                              BSONObj& cmd) {
    return ui.ok() ? parseUUID(opCtx, ui) : parseNs(ns, cmd);
}

using OpApplyFn = stdx::function<Status(OperationContext* opCtx,
                                        const char* ns,
                                        const BSONElement& ui,
                                        BSONObj& cmd,
                                        const OpTime& opTime,
                                        const OplogEntry& entry,
                                        OplogApplication::Mode mode)>;

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

std::map<std::string, ApplyOpMetadata> opsMap = {
    {"create",
     {[](OperationContext* opCtx,
         const char* ns,
         const BSONElement& ui,
         BSONObj& cmd,
         const OpTime& opTime,
         const OplogEntry& entry,
         OplogApplication::Mode mode) -> Status {
          const NamespaceString nss(parseNs(ns, cmd));
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
         OplogApplication::Mode mode) -> Status {
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
      {ErrorCodes::IndexAlreadyExists, ErrorCodes::NamespaceNotFound}}},
    {"startIndexBuild",
     {[](OperationContext* opCtx,
         const char* ns,
         const BSONElement& ui,
         BSONObj& cmd,
         const OpTime& opTime,
         const OplogEntry& entry,
         OplogApplication::Mode mode) -> Status {
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
                 buildUUIDElem.eoo());
         UUID indexBuildUUID = uassertStatusOK(UUID::parse(buildUUIDElem));

         auto indexesElem = cmd.getField("indexes");
         uassert(ErrorCodes::BadValue,
                 "Error parsing 'startIndexBuild' oplog entry, missing required field 'indexes'.",
                 indexesElem.eoo());
         uassert(ErrorCodes::BadValue,
                 "Error parsing 'startIndexBuild' oplog entry, field 'indexes' must be an array.",
                 indexesElem.type() == Array);

         auto collUUID = uassertStatusOK(UUID::parse(ui));

         return startIndexBuild(opCtx, collUUID, indexBuildUUID, indexesElem, mode);
     }}},
    {"commitIndexBuild",
     {[](OperationContext* opCtx,
         const char* ns,
         const BSONElement& ui,
         BSONObj& cmd,
         const OpTime& opTime,
         const OplogEntry& entry,
         OplogApplication::Mode mode) -> Status {
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

         auto buildUUIDElem = cmd.getField("indexBuildUUID");
         uassert(ErrorCodes::BadValue,
                 "Error parsing 'commitIndexBuild' oplog entry, missing required field "
                 "'indexBuildUUID'.",
                 buildUUIDElem.eoo());
         UUID indexBuildUUID = uassertStatusOK(UUID::parse(buildUUIDElem));

         auto indexesElem = cmd.getField("indexes");
         uassert(ErrorCodes::BadValue,
                 "Error parsing 'commitIndexBuild' oplog entry, missing required field 'indexes'.",
                 indexesElem.eoo());
         uassert(ErrorCodes::BadValue,
                 "Error parsing 'commitIndexBuild' oplog entry, field 'indexes' must be an array.",
                 indexesElem.type() == Array);

         return commitIndexBuild(opCtx, indexBuildUUID, indexesElem, mode);
     }}},
    {"abortIndexBuild",
     {[](OperationContext* opCtx,
         const char* ns,
         const BSONElement& ui,
         BSONObj& cmd,
         const OpTime& opTme,
         const OplogEntry& entry,
         OplogApplication::Mode mode) -> Status {
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
         OplogApplication::Mode mode) -> Status {
          NamespaceString nss;
          std::tie(std::ignore, nss) = parseCollModUUIDAndNss(opCtx, ui, ns, cmd);
          // The collMod for apply ops could be either a user driven collMod or a collMod triggered
          // by an upgrade.
          return collModWithUpgrade(opCtx, nss, cmd);
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
         OplogApplication::Mode mode) -> Status {
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
         OplogApplication::Mode mode) -> Status {
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
         OplogApplication::Mode mode) -> Status {
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
         OplogApplication::Mode mode) -> Status {
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
         OplogApplication::Mode mode) -> Status {
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
         OplogApplication::Mode mode) -> Status {
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
         OplogApplication::Mode mode) -> Status {
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
         OplogApplication::Mode mode) -> Status {
         return applyApplyOpsOplogEntry(opCtx, entry, mode);
     }}},
    {"convertToCapped",
     {[](OperationContext* opCtx,
         const char* ns,
         const BSONElement& ui,
         BSONObj& cmd,
         const OpTime& opTime,
         const OplogEntry& entry,
         OplogApplication::Mode mode) -> Status {
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
         OplogApplication::Mode mode) -> Status {
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
         OplogApplication::Mode mode) -> Status {
         return applyCommitTransaction(opCtx, entry, mode);
     }}},
    {"abortTransaction",
     {[](OperationContext* opCtx,
         const char* ns,
         const BSONElement& ui,
         BSONObj& cmd,
         const OpTime& opTime,
         const OplogEntry& entry,
         OplogApplication::Mode mode) -> Status {
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
        UUIDCatalog& catalog = UUIDCatalog::get(opCtx);
        auto uuid = uassertStatusOK(UUID::parse(fieldUI));
        collection = catalog.lookupCollectionByUUID(uuid);
        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "Failed to apply operation due to missing collection (" << uuid
                              << "): "
                              << redact(op.toString()),
                collection);
        requestNss = collection->ns();
        dassert(opCtx->lockState()->isCollectionLockedForMode(
                    requestNss.ns(), supportsDocLocking() ? MODE_IX : MODE_X),
                requestNss.ns());
    } else {
        uassert(ErrorCodes::InvalidNamespace,
                "'ns' must be of type String",
                fieldNs.type() == BSONType::String);
        const StringData ns = fieldNs.valuestrsafe();
        requestNss = NamespaceString(ns);
        if (nsIsFull(ns)) {
            if (supportsDocLocking()) {
                // WiredTiger, and others requires MODE_IX since the applier threads driving
                // this allow writes to the same collection on any thread.
                dassert(opCtx->lockState()->isCollectionLockedForMode(ns, MODE_IX),
                        requestNss.ns());
            } else {
                // mmapV1 ensures that all operations to the same collection are executed from
                // the same worker thread, so it takes an exclusive lock (MODE_X)
                dassert(opCtx->lockState()->isCollectionLockedForMode(ns, MODE_X), requestNss.ns());
            }
        }
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
            collection || !db->getViewCatalog()->lookup(opCtx, requestNss.ns()));

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
        } else {
            switch (replMode) {
                case ReplicationCoordinator::modeReplSet: {
                    if (haveWrappingWriteUnitOfWork) {
                        // We do not assign timestamps to non-replicated writes that have a wrapping
                        // WUOW. These must be operations inside of atomic 'applyOps' commands being
                        // applied on a secondary. They will get the timestamp of the outer
                        // 'applyOps' oplog entry in their wrapper WUOW.
                        return false;
                    }
                    break;
                }
                case ReplicationCoordinator::modeNone: {
                    // Only assign timestamps on standalones during replication recovery when
                    // started with 'recoverFromOplogAsStandalone'.
                    return mode == OplogApplication::Mode::kRecovering;
                }
            }
        }
        return true;
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

            // 1. Try insert first, if we have no wrappingWriteUnitOfWork
            // 2. If okay, commit
            // 3. If not, do upsert (and commit)
            // 4. If both !Ok, return status

            // We cannot rely on a DuplicateKey error if we're part of a larger transaction,
            // because that would require the transaction to abort. So instead, use upsert in that
            // case.
            bool needToDoUpsert = haveWrappingWriteUnitOfWork;

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
                request.setUpdates(o);
                request.setUpsert();
                request.setFromOplogApplication(true);

                const StringData ns = fieldNs.valuestrsafe();
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
        request.setUpdates(o);
        request.setUpsert(upsert);
        request.setFromOplogApplication(true);

        Timestamp timestamp;
        if (assignOperationTimestamp) {
            timestamp = fieldTs.timestamp();
        }

        const StringData ns = fieldNs.valuestrsafe();
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
                    if (collection == NULL ||
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

        const StringData ns = fieldNs.valuestrsafe();
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
                           OplogApplication::Mode mode) {
    LOG(3) << "applying command op: " << redact(op)
           << ", oplog application mode: " << OplogApplication::modeToString(mode);

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
        if (db && !db->getCollection(opCtx, nss) && db->getViewCatalog()->lookup(opCtx, nss.ns())) {
            return {ErrorCodes::CommandNotSupportedOnView,
                    str::stream() << "applyOps not supported on view:" << nss.ns()};
        }
    }

    // The feature compatibility version in the server configuration collection cannot change during
    // initial sync.
    // We do not attempt to parse the whitelisted ops because they do not have a collection
    // namespace. If we drop the 'admin' database we will also log a 'drop' oplog entry for each
    // collection dropped. 'applyOps' will try to apply each individual operation, and those
    // will be caught then if they are a problem.
    auto whitelistedOps = std::vector<std::string>{"dropDatabase", "applyOps", "dbCheck"};
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

    const bool assignCommandTimestamp = [opCtx, mode, &op, &o] {
        const auto replMode = ReplicationCoordinator::get(opCtx)->getReplicationMode();
        if (opCtx->writesAreReplicated()) {
            // We do not assign timestamps on replicated writes since they will get their oplog
            // timestamp once they are logged.
            return false;
        }

        // Don't assign commit timestamp for transaction commands.
        const StringData commandName(o.firstElementFieldName());
        if (op.getBoolField("prepare") || commandName == "abortTransaction" ||
            commandName == "commitTransaction")
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
        auto op = opsMap.find(o.firstElementFieldName());
        if (op == opsMap.end()) {
            return Status(ErrorCodes::BadValue,
                          mongoutils::str::stream() << "Invalid key '" << o.firstElementFieldName()
                                                    << "' found in field 'o'");
        }
        ApplyOpMetadata curOpToApply = op->second;
        Status status = Status::OK();
        try {
            // If 'writeTime' is not null, any writes in this scope will be given 'writeTime' as
            // their timestamp at commit.
            TimestampBlock tsBlock(opCtx, writeTime);
            status =
                curOpToApply.applyFunc(opCtx, nss.ns().c_str(), fieldUI, o, opTime, entry, mode);
        } catch (...) {
            status = exceptionToStatus();
        }
        switch (status.code()) {
            case ErrorCodes::WriteConflict: {
                // Need to throw this up to a higher level where it will be caught and the
                // operation retried.
                throw WriteConflictException();
            }
            case ErrorCodes::BackgroundOperationInProgressForDatabase: {
                Lock::TempRelease release(opCtx->lockState());

                BackgroundOperation::awaitNoBgOpInProgForDb(nss.db());
                opCtx->recoveryUnit()->abandonSnapshot();
                opCtx->checkForInterrupt();
                break;
            }
            case ErrorCodes::BackgroundOperationInProgressForNamespace: {
                Lock::TempRelease release(opCtx->lockState());

                Command* cmd = CommandHelpers::findCommand(o.firstElement().fieldName());
                invariant(cmd);

                // TODO: This parse could be expensive and not worth it.
                BackgroundOperation::awaitNoBgOpInProgForNs(
                    cmd->parse(opCtx, OpMsgRequest::fromDBAndBody(nss.db(), o))->ns().toString());

                opCtx->recoveryUnit()->abandonSnapshot();
                opCtx->checkForInterrupt();
                break;
            }
            default:
                if (!curOpToApply.acceptableErrors.count(status.code())) {
                    error() << "Failed command " << redact(o) << " on " << nss.db()
                            << " with status " << status << " during oplog application";
                    return status;
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
    stdx::lock_guard<stdx::mutex> lk(localOplogInfo(service).newOpMutex);
    LogicalClock::get(service)->setClusterTimeFromTrustedSource(LogicalTime(newTime));
}

void initTimestampFromOplog(OperationContext* opCtx, const std::string& oplogNS) {
    DBDirectClient c(opCtx);
    static const BSONObj reverseNaturalObj = BSON("$natural" << -1);
    BSONObj lastOp = c.findOne(oplogNS, Query().sort(reverseNaturalObj), NULL, QueryOption_SlaveOk);

    if (!lastOp.isEmpty()) {
        LOG(1) << "replSet setting last Timestamp";
        const OpTime opTime = fassert(28696, OpTime::parseFromOplogEntry(lastOp));
        setNewTimestamp(opCtx->getServiceContext(), opTime.getTimestamp());
    }
}

void oplogCheckCloseDatabase(OperationContext* opCtx, Database* db) {
    invariant(opCtx->lockState()->isW());
    if (db->name() == "local") {
        localOplogInfo(opCtx->getServiceContext()).oplog = nullptr;
    }
}

void acquireOplogCollectionForLogging(OperationContext* opCtx) {
    auto& oplogInfo = localOplogInfo(opCtx->getServiceContext());
    if (!oplogInfo.oplogName.empty()) {
        AutoGetCollection autoColl(opCtx, NamespaceString(oplogInfo.oplogName), MODE_IX);
        oplogInfo.oplog = autoColl.getCollection();
    }
}

void establishOplogCollectionForLogging(OperationContext* opCtx, Collection* oplog) {
    invariant(opCtx->lockState()->isW());
    invariant(oplog);
    localOplogInfo(opCtx->getServiceContext()).oplog = oplog;
}

void signalOplogWaiters() {
    auto oplog = localOplogInfo(getGlobalServiceContext()).oplog;
    if (oplog) {
        oplog->getCappedCallback()->notifyCappedWaitersIfNeeded();
    }
}

}  // namespace repl
}  // namespace mongo
