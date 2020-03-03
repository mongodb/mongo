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
#define LOGV2_FOR_RECOVERY(ID, DLEVEL, MESSAGE, ...) \
    LOGV2_DEBUG_OPTIONS(ID, DLEVEL, {logv2::LogComponent::kStorageRecovery}, MESSAGE, ##__VA_ARGS__)


#include "mongo/platform/basic.h"

#include "mongo/db/repl/replication_recovery.h"

#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/apply_ops.h"
#include "mongo/db/repl/oplog_applier_impl.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/repl/replication_consistency_markers_impl.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/transaction_oplog_application.h"
#include "mongo/db/server_recovery.h"
#include "mongo/db/session.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/transaction_history_iterator.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/logv2/log.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace repl {

namespace {

const auto kRecoveryBatchLogLevel = logger::LogSeverity::Debug(2);
const auto kRecoveryOperationLogLevel = logger::LogSeverity::Debug(3);
const auto kRecoveryBatchLogLevelV2 = logv2::LogSeverity::Debug(2);
const auto kRecoveryOperationLogLevelV2 = logv2::LogSeverity::Debug(3);

/**
 * Tracks and logs operations applied during recovery.
 */
class RecoveryOplogApplierStats : public OplogApplier::Observer {
public:
    void onBatchBegin(const std::vector<OplogEntry>& batch) final {
        _numBatches++;
        LOGV2_FOR_RECOVERY(24098,
                           logSeverityV1toV2(kRecoveryBatchLogLevel).toInt(),
                           "Applying operations in batch: {numBatches}({batch_size} operations "
                           "from {batch_front_getOpTime} (inclusive) to {batch_back_getOpTime} "
                           "(inclusive)). Operations applied so far: {numOpsApplied}",
                           "numBatches"_attr = _numBatches,
                           "batch_size"_attr = batch.size(),
                           "batch_front_getOpTime"_attr = batch.front().getOpTime(),
                           "batch_back_getOpTime"_attr = batch.back().getOpTime(),
                           "numOpsApplied"_attr = _numOpsApplied);

        _numOpsApplied += batch.size();
        if (shouldLog(::mongo::logv2::LogComponent::kStorageRecovery,
                      kRecoveryOperationLogLevelV2)) {
            std::size_t i = 0;
            for (const auto& entry : batch) {
                i++;
                LOGV2_FOR_RECOVERY(
                    24099,
                    logSeverityV1toV2(kRecoveryOperationLogLevel).toInt(),
                    "Applying op {i} of {batch_size} (in batch {numBatches}) during replication "
                    "recovery: {entry_getRaw}",
                    "i"_attr = i,
                    "batch_size"_attr = batch.size(),
                    "numBatches"_attr = _numBatches,
                    "entry_getRaw"_attr = redact(entry.getRaw()));
            }
        }
    }

    void onBatchEnd(const StatusWith<OpTime>&, const std::vector<OplogEntry>&) final {}

    void complete(const OpTime& applyThroughOpTime) const {
        LOGV2(21536,
              "Applied {numOpsApplied} operations in {numBatches} batches. Last operation applied "
              "with optime: {applyThroughOpTime}",
              "numOpsApplied"_attr = _numOpsApplied,
              "numBatches"_attr = _numBatches,
              "applyThroughOpTime"_attr = applyThroughOpTime);
    }

private:
    std::size_t _numBatches = 0;
    std::size_t _numOpsApplied = 0;
};

/**
 * OplogBuffer adaptor for a DBClient query on the oplog.
 * Implements only functions used by OplogApplier::getNextApplierBatch().
 */
class OplogBufferLocalOplog final : public OplogBuffer {
public:
    explicit OplogBufferLocalOplog(Timestamp oplogApplicationStartPoint,
                                   boost::optional<Timestamp> oplogApplicationEndPoint)
        : _oplogApplicationStartPoint(oplogApplicationStartPoint),
          _oplogApplicationEndPoint(oplogApplicationEndPoint) {}

    void startup(OperationContext* opCtx) final {
        _client = std::make_unique<DBDirectClient>(opCtx);
        BSONObj predicate = _oplogApplicationEndPoint
            ? BSON("$gte" << _oplogApplicationStartPoint << "$lte" << *_oplogApplicationEndPoint)
            : BSON("$gte" << _oplogApplicationStartPoint);
        _cursor = _client->query(NamespaceString::kRsOplogNamespace,
                                 QUERY("ts" << predicate),
                                 /*batchSize*/ 0,
                                 /*skip*/ 0,
                                 /*projection*/ nullptr,
                                 QueryOption_OplogReplay);

        // Check that the first document matches our appliedThrough point then skip it since it's
        // already been applied.
        if (!_cursor->more()) {
            // This should really be impossible because we check above that the top of the oplog is
            // strictly > appliedThrough. If this fails it represents a serious bug in either the
            // storage engine or query's implementation of OplogReplay.
            std::stringstream ss;
            ss << " >= " << _oplogApplicationStartPoint.toBSON();
            if (_oplogApplicationEndPoint) {
                ss << " and <= " << _oplogApplicationEndPoint->toBSON();
            }

            LOGV2_FATAL(
                21559,
                "Couldn't find any entries in the oplog{oplog}, which should be impossible.",
                "oplog"_attr = ss.str());
            fassertFailedNoTrace(40293);
        }

        auto firstTimestampFound =
            fassert(40291, OpTime::parseFromOplogEntry(_cursor->nextSafe())).getTimestamp();
        if (firstTimestampFound != _oplogApplicationStartPoint) {
            LOGV2_FATAL(21560,
                        "Oplog entry at {oplogApplicationStartPoint} is missing; actual entry "
                        "found is {firstTimestampFound}",
                        "oplogApplicationStartPoint"_attr = _oplogApplicationStartPoint.toBSON(),
                        "firstTimestampFound"_attr = firstTimestampFound.toBSON());
            fassertFailedNoTrace(40292);
        }
    }

    void shutdown(OperationContext*) final {
        _cursor = {};
        _client = {};
    }

    bool isEmpty() const final {
        return !_cursor->more();
    }

    bool tryPop(OperationContext*, Value* value) final {
        return _peekOrPop(value, Mode::kPop);
    }

    bool peek(OperationContext*, Value* value) final {
        return _peekOrPop(value, Mode::kPeek);
    }

    void push(OperationContext*, Batch::const_iterator, Batch::const_iterator) final {
        MONGO_UNREACHABLE;
    }
    void waitForSpace(OperationContext*, std::size_t) final {
        MONGO_UNREACHABLE;
    }
    std::size_t getMaxSize() const final {
        MONGO_UNREACHABLE;
    }
    std::size_t getSize() const final {
        MONGO_UNREACHABLE;
    }
    std::size_t getCount() const final {
        MONGO_UNREACHABLE;
    }
    void clear(OperationContext*) final {
        MONGO_UNREACHABLE;
    }
    bool waitForData(Seconds) final {
        MONGO_UNREACHABLE;
    }
    boost::optional<Value> lastObjectPushed(OperationContext*) const final {
        MONGO_UNREACHABLE;
    }

private:
    enum class Mode { kPeek, kPop };
    bool _peekOrPop(Value* value, Mode mode) {
        if (isEmpty()) {
            return false;
        }
        *value = mode == Mode::kPeek ? _cursor->peekFirst() : _cursor->nextSafe();
        invariant(!value->isEmpty());
        return true;
    }

    const Timestamp _oplogApplicationStartPoint;
    const boost::optional<Timestamp> _oplogApplicationEndPoint;
    std::unique_ptr<DBDirectClient> _client;
    std::unique_ptr<DBClientCursor> _cursor;
};

boost::optional<Timestamp> recoverFromOplogPrecursor(OperationContext* opCtx,
                                                     StorageInterface* storageInterface) {
    if (!storageInterface->supportsRecoveryTimestamp(opCtx->getServiceContext())) {
        LOGV2_FATAL(21561,
                    "Cannot recover from the oplog with a storage engine that does not support "
                    "recover to stable timestamp.");
        fassertFailedNoTrace(50805);
    }

    // A non-existent recoveryTS means the checkpoint is unstable. If the recoveryTS exists but
    // is null, that means a stable checkpoint was taken at a null timestamp. This should never
    // happen.
    auto recoveryTS = storageInterface->getRecoveryTimestamp(opCtx->getServiceContext());
    if (recoveryTS && recoveryTS->isNull()) {
        LOGV2_FATAL(21562,
                    "Cannot recover from the oplog with stable checkpoint at null timestamp.");
        fassertFailedNoTrace(50806);
    }

    return recoveryTS;
}

}  // namespace

ReplicationRecoveryImpl::ReplicationRecoveryImpl(StorageInterface* storageInterface,
                                                 ReplicationConsistencyMarkers* consistencyMarkers)
    : _storageInterface(storageInterface), _consistencyMarkers(consistencyMarkers) {}

void ReplicationRecoveryImpl::_assertNoRecoveryNeededOnUnstableCheckpoint(OperationContext* opCtx) {
    invariant(_storageInterface->supportsRecoveryTimestamp(opCtx->getServiceContext()));
    invariant(!_storageInterface->getRecoveryTimestamp(opCtx->getServiceContext()));

    if (_consistencyMarkers->getInitialSyncFlag(opCtx)) {
        LOGV2_FATAL(21563, "Unexpected recovery needed, initial sync flag set.");
        fassertFailedNoTrace(31362);
    }

    const auto truncateAfterPoint = _consistencyMarkers->getOplogTruncateAfterPoint(opCtx);
    if (!truncateAfterPoint.isNull()) {
        LOGV2_FATAL(21564,
                    "Unexpected recovery needed, oplog requires truncation. Truncate after point: "
                    "{truncateAfterPoint}",
                    "truncateAfterPoint"_attr = truncateAfterPoint.toString());
        fassertFailedNoTrace(31363);
    }

    auto topOfOplogSW = _getTopOfOplog(opCtx);
    if (!topOfOplogSW.isOK()) {
        LOGV2_FATAL(21565,
                    "Recovery not possible, no oplog found: {status}",
                    "status"_attr = topOfOplogSW.getStatus());
        fassertFailedNoTrace(31364);
    }
    const auto topOfOplog = topOfOplogSW.getValue();

    const auto appliedThrough = _consistencyMarkers->getAppliedThrough(opCtx);
    if (!appliedThrough.isNull() && appliedThrough != topOfOplog) {
        LOGV2_FATAL(21566,
                    "Unexpected recovery needed, appliedThrough is not at top of oplog, indicating "
                    "oplog has not been fully applied. appliedThrough: {appliedThrough}",
                    "appliedThrough"_attr = appliedThrough.toString());
        fassertFailedNoTrace(31365);
    }

    const auto minValid = _consistencyMarkers->getMinValid(opCtx);
    if (minValid > topOfOplog) {
        LOGV2_FATAL(21567,
                    "Unexpected recovery needed, top of oplog is not consistent. topOfOplog: "
                    "{topOfOplog}, minValid: {minValid}",
                    "topOfOplog"_attr = topOfOplog,
                    "minValid"_attr = minValid);
        fassertFailedNoTrace(31366);
    }
}

void ReplicationRecoveryImpl::recoverFromOplogAsStandalone(OperationContext* opCtx) {
    auto recoveryTS = recoverFromOplogPrecursor(opCtx, _storageInterface);

    // Initialize the cached pointer to the oplog collection.
    acquireOplogCollectionForLogging(opCtx);

    if (recoveryTS) {
        // We pass in "none" for the stable timestamp so that recoverFromOplog asks storage
        // for the recoveryTimestamp just like on replica set recovery.
        const auto stableTimestamp = boost::none;
        recoverFromOplog(opCtx, stableTimestamp);
    } else {
        if (gTakeUnstableCheckpointOnShutdown) {
            // Ensure 'recoverFromOplogAsStandalone' with 'takeUnstableCheckpointOnShutdown'
            // is safely idempotent when it succeeds.
            LOGV2(21537,
                  "Recovering from unstable checkpoint with 'takeUnstableCheckpointOnShutdown'. "
                  "Confirming that no oplog recovery is needed.");
            _assertNoRecoveryNeededOnUnstableCheckpoint(opCtx);
            LOGV2(21538,
                  "Not doing any oplog recovery since there is an unstable checkpoint that is up "
                  "to date.");
        } else {
            LOGV2_FATAL(21568,
                        "Cannot use 'recoverFromOplogAsStandalone' without a stable checkpoint.");
            fassertFailedNoTrace(31229);
        }
    }

    reconstructPreparedTransactions(opCtx, OplogApplication::Mode::kRecovering);

    LOGV2_WARNING(21558,
                  "Setting mongod to readOnly mode as a result of specifying "
                  "'recoverFromOplogAsStandalone'.");
    storageGlobalParams.readOnly = true;
}

void ReplicationRecoveryImpl::recoverFromOplogUpTo(OperationContext* opCtx, Timestamp endPoint) {
    uassert(
        ErrorCodes::InitialSyncActive,
        str::stream() << "Cannot recover from oplog while the node is performing an initial sync",
        !_consistencyMarkers->getInitialSyncFlag(opCtx));

    auto recoveryTS = recoverFromOplogPrecursor(opCtx, _storageInterface);
    if (!recoveryTS) {
        LOGV2_FATAL(21569, "Cannot use 'recoverToOplogTimestamp' without a stable checkpoint.");
        fassertFailedNoTrace(31399);
    }

    // This may take an IS lock on the oplog collection.
    _truncateOplogIfNeededAndThenClearOplogTruncateAfterPoint(opCtx, recoveryTS);

    boost::optional<Timestamp> startPoint =
        _storageInterface->getRecoveryTimestamp(opCtx->getServiceContext());
    if (!startPoint) {
        fassert(31436, "No recovery timestamp, cannot recover from the oplog.");
    }

    invariant(!endPoint.isNull());

    if (*startPoint == endPoint) {
        LOGV2(21540,
              "No oplog entries to apply for recovery. Start point '{startPoint}' is at the end "
              "point '{endPoint}' in the oplog.",
              "startPoint"_attr = startPoint,
              "endPoint"_attr = endPoint);
        return;
    } else if (*startPoint > endPoint) {
        uasserted(ErrorCodes::BadValue,
                  str::stream() << "No oplog entries to apply for recovery. Start point '"
                                << startPoint->toString() << "' is beyond the end point '"
                                << endPoint.toString() << "' in the oplog.");
    }

    Timestamp appliedUpTo = _applyOplogOperations(opCtx, *startPoint, endPoint);
    if (appliedUpTo.isNull()) {
        LOGV2(21541,
              "No stored oplog entries to apply for recovery between {startPoint} (inclusive) and "
              "{endPoint} (inclusive).",
              "startPoint"_attr = startPoint->toString(),
              "endPoint"_attr = endPoint.toString());
    } else {
        invariant(appliedUpTo <= endPoint);
    }

    reconstructPreparedTransactions(opCtx, OplogApplication::Mode::kRecovering);
}

void ReplicationRecoveryImpl::recoverFromOplog(OperationContext* opCtx,
                                               boost::optional<Timestamp> stableTimestamp) try {
    if (_consistencyMarkers->getInitialSyncFlag(opCtx)) {
        LOGV2(21542, "No recovery needed. Initial sync flag set.");
        return;  // Initial Sync will take over so no cleanup is needed.
    }

    const auto serviceCtx = getGlobalServiceContext();
    inReplicationRecovery(serviceCtx) = true;
    ON_BLOCK_EXIT([serviceCtx] {
        invariant(
            inReplicationRecovery(serviceCtx),
            "replication recovery flag is unexpectedly unset when exiting recoverFromOplog()");
        inReplicationRecovery(serviceCtx) = false;
    });

    // If we were passed in a stable timestamp, we are in rollback recovery and should recover from
    // that stable timestamp. Otherwise, we're recovering at startup. If this storage engine
    // supports recover to stable timestamp or enableMajorityReadConcern=false, we ask it for the
    // recovery timestamp. If the storage engine returns a timestamp, we recover from that point.
    // However, if the storage engine returns "none", the storage engine does not have a stable
    // checkpoint and we must recover from an unstable checkpoint instead.
    const bool supportsRecoveryTimestamp =
        _storageInterface->supportsRecoveryTimestamp(opCtx->getServiceContext());
    if (!stableTimestamp && supportsRecoveryTimestamp) {
        stableTimestamp = _storageInterface->getRecoveryTimestamp(opCtx->getServiceContext());
    }

    const auto appliedThrough = _consistencyMarkers->getAppliedThrough(opCtx);
    invariant(!stableTimestamp || stableTimestamp->isNull() || appliedThrough.isNull() ||
                  *stableTimestamp == appliedThrough.getTimestamp(),
              str::stream() << "Stable timestamp " << stableTimestamp->toString()
                            << " does not equal appliedThrough timestamp "
                            << appliedThrough.toString());

    // This may take an IS lock on the oplog collection.
    _truncateOplogIfNeededAndThenClearOplogTruncateAfterPoint(opCtx, stableTimestamp);

    auto topOfOplogSW = _getTopOfOplog(opCtx);
    if (topOfOplogSW.getStatus() == ErrorCodes::CollectionIsEmpty ||
        topOfOplogSW.getStatus() == ErrorCodes::NamespaceNotFound) {
        // Oplog is empty. There are no oplog entries to apply, so we exit recovery and go into
        // initial sync.
        LOGV2(21543, "No oplog entries to apply for recovery. Oplog is empty.");
        return;
    }
    fassert(40290, topOfOplogSW);
    const auto topOfOplog = topOfOplogSW.getValue();

    if (stableTimestamp) {
        invariant(supportsRecoveryTimestamp);
        _recoverFromStableTimestamp(opCtx, *stableTimestamp, appliedThrough, topOfOplog);
    } else {
        _recoverFromUnstableCheckpoint(opCtx, appliedThrough, topOfOplog);
    }
} catch (...) {
    LOGV2_FATAL(21570,
                "Caught exception during replication recovery: {exception}",
                "exception"_attr = exceptionToStatus());
    std::terminate();
}

void ReplicationRecoveryImpl::_recoverFromStableTimestamp(OperationContext* opCtx,
                                                          Timestamp stableTimestamp,
                                                          OpTime appliedThrough,
                                                          OpTime topOfOplog) {
    invariant(!stableTimestamp.isNull());
    invariant(!topOfOplog.isNull());

    const auto truncateAfterPoint = _consistencyMarkers->getOplogTruncateAfterPoint(opCtx);

    LOGV2(21544,
          "Recovering from stable timestamp: {stableTimestamp} (top of oplog: {topOfOplog}, "
          "appliedThrough: {appliedThrough}, TruncateAfter: {truncateAfterPoint})",
          "stableTimestamp"_attr = stableTimestamp,
          "topOfOplog"_attr = topOfOplog,
          "appliedThrough"_attr = appliedThrough,
          "truncateAfterPoint"_attr = truncateAfterPoint);

    LOGV2(21545,
          "Starting recovery oplog application at the stable timestamp: {stableTimestamp}",
          "stableTimestamp"_attr = stableTimestamp);
    _applyToEndOfOplog(opCtx, stableTimestamp, topOfOplog.getTimestamp());
}

void ReplicationRecoveryImpl::_recoverFromUnstableCheckpoint(OperationContext* opCtx,
                                                             OpTime appliedThrough,
                                                             OpTime topOfOplog) {
    invariant(!topOfOplog.isNull());
    LOGV2(21546,
          "Recovering from an unstable checkpoint (top of oplog: {topOfOplog}, appliedThrough: "
          "{appliedThrough})",
          "topOfOplog"_attr = topOfOplog,
          "appliedThrough"_attr = appliedThrough);

    if (appliedThrough.isNull()) {
        // The appliedThrough would be null if we shut down cleanly or crashed as a primary. Either
        // way we are consistent at the top of the oplog.
        LOGV2(21547, "No oplog entries to apply for recovery. appliedThrough is null.");
    } else {
        // If the appliedThrough is not null, then we shut down uncleanly during secondary oplog
        // application and must apply from the appliedThrough to the top of the oplog.
        LOGV2(21548,
              "Starting recovery oplog application at the appliedThrough: {appliedThrough}, "
              "through the top of the oplog: {topOfOplog}",
              "appliedThrough"_attr = appliedThrough,
              "topOfOplog"_attr = topOfOplog);

        // When `recoverFromOplog` truncates the oplog, that also happens to set the "oldest
        // timestamp" to the truncation point[1]. `_applyToEndOfOplog` will then perform writes
        // before the truncation point. Doing so violates the constraint that all updates must be
        // timestamped newer than the "oldest timestamp". This call will move the "oldest
        // timestamp" back to the `startPoint`.
        //
        // [1] This is arguably incorrect. On rollback for nodes that are not keeping history to
        // the "majority point", the "oldest timestamp" likely needs to go back in time. The
        // oplog's `cappedTruncateAfter` method was a convenient location for this logic, which,
        // unfortunately, conflicts with the usage above.
        opCtx->getServiceContext()->getStorageEngine()->setOldestTimestamp(
            appliedThrough.getTimestamp());

        _applyToEndOfOplog(opCtx, appliedThrough.getTimestamp(), topOfOplog.getTimestamp());
    }

    // `_recoverFromUnstableCheckpoint` is only expected to be called on startup.
    _storageInterface->setInitialDataTimestamp(opCtx->getServiceContext(),
                                               topOfOplog.getTimestamp());

    // Ensure the `appliedThrough` is set to the top of oplog, specifically if the node was
    // previously running as a primary. If a crash happens before the first stable checkpoint on
    // upgrade, replication recovery will know it must apply from this point and not assume the
    // datafiles contain any writes that were taken before the crash.
    _consistencyMarkers->setAppliedThrough(opCtx, topOfOplog);

    // Force the set `appliedThrough` to become durable on disk in a checkpoint. This method would
    // typically take a stable checkpoint, but because we're starting up from a checkpoint that
    // has no checkpoint timestamp, the stable checkpoint "degrades" into an unstable checkpoint.
    //
    // Not waiting for checkpoint durability here can result in a scenario where the node takes
    // writes and persists them to the oplog, but crashes before a stable checkpoint persists a
    // "recovery timestamp". The typical startup path for data-bearing nodes is to use the recovery
    // timestamp to determine where to play oplog forward from. As this method shows, when a
    // recovery timestamp does not exist, the applied through is used to determine where to start
    // playing oplog entries from.
    opCtx->recoveryUnit()->waitUntilUnjournaledWritesDurable(opCtx, /*stableCheckpoint*/ true);
}

void ReplicationRecoveryImpl::_applyToEndOfOplog(OperationContext* opCtx,
                                                 const Timestamp& oplogApplicationStartPoint,
                                                 const Timestamp& topOfOplog) {
    invariant(!oplogApplicationStartPoint.isNull());
    invariant(!topOfOplog.isNull());

    // Check if we have any unapplied ops in our oplog. It is important that this is done after
    // deleting the ragged end of the oplog.
    if (oplogApplicationStartPoint == topOfOplog) {
        LOGV2(21549,
              "No oplog entries to apply for recovery. Start point is at the top of the oplog.");
        return;  // We've applied all the valid oplog we have.
    } else if (oplogApplicationStartPoint > topOfOplog) {
        LOGV2_FATAL(
            21571,
            "Applied op {oplogApplicationStartPoint} not found. Top of oplog is {topOfOplog}.",
            "oplogApplicationStartPoint"_attr = oplogApplicationStartPoint.toBSON(),
            "topOfOplog"_attr = topOfOplog.toBSON());
        fassertFailedNoTrace(40313);
    }

    Timestamp appliedUpTo = _applyOplogOperations(opCtx, oplogApplicationStartPoint, topOfOplog);
    invariant(!appliedUpTo.isNull());
    invariant(appliedUpTo == topOfOplog,
              str::stream() << "Did not apply to top of oplog. Applied through: "
                            << appliedUpTo.toString()
                            << ". Top of oplog: " << topOfOplog.toString());
}

Timestamp ReplicationRecoveryImpl::_applyOplogOperations(OperationContext* opCtx,
                                                         const Timestamp& startPoint,
                                                         const Timestamp& endPoint) {
    LOGV2(21550,
          "Replaying stored operations from {startPoint} (inclusive) to {endPoint} (inclusive).",
          "startPoint"_attr = startPoint,
          "endPoint"_attr = endPoint);

    OplogBufferLocalOplog oplogBuffer(startPoint, endPoint);
    oplogBuffer.startup(opCtx);

    RecoveryOplogApplierStats stats;

    auto writerPool = makeReplWriterPool();
    OplogApplierImpl oplogApplier(nullptr,
                                  &oplogBuffer,
                                  &stats,
                                  ReplicationCoordinator::get(opCtx),
                                  _consistencyMarkers,
                                  _storageInterface,
                                  OplogApplier::Options(OplogApplication::Mode::kRecovering),
                                  writerPool.get());

    OplogApplier::BatchLimits batchLimits;
    batchLimits.bytes = getBatchLimitOplogBytes(opCtx, _storageInterface);
    batchLimits.ops = getBatchLimitOplogEntries();

    OpTime applyThroughOpTime;
    std::vector<OplogEntry> batch;
    while (
        !(batch = fassert(50763, oplogApplier.getNextApplierBatch(opCtx, batchLimits))).empty()) {
        applyThroughOpTime = uassertStatusOK(oplogApplier.applyOplogBatch(opCtx, std::move(batch)));
    }
    stats.complete(applyThroughOpTime);
    invariant(oplogBuffer.isEmpty(),
              str::stream() << "Oplog buffer not empty after applying operations. Last operation "
                               "applied with optime: "
                            << applyThroughOpTime.toBSON());
    oplogBuffer.shutdown(opCtx);

    // The applied up to timestamp will be null if no oplog entries were applied.
    if (applyThroughOpTime.isNull()) {
        return Timestamp();
    }

    // We may crash before setting appliedThrough. If we have a stable checkpoint, we will recover
    // to that checkpoint at a replication consistent point, and applying the oplog is safe.
    // If we don't have a stable checkpoint, then we must be in startup recovery, and not rollback
    // recovery, because we only roll back to a stable timestamp when we have a stable checkpoint.
    // Startup recovery from an unstable checkpoint only ever applies a single batch and it is safe
    // to replay the batch from any point.
    _consistencyMarkers->setAppliedThrough(opCtx, applyThroughOpTime);
    return applyThroughOpTime.getTimestamp();
}

StatusWith<OpTime> ReplicationRecoveryImpl::_getTopOfOplog(OperationContext* opCtx) const {
    // OplogInterfaceLocal creates a backwards iterator over the oplog collection.
    OplogInterfaceLocal localOplog(opCtx);
    auto localOplogIter = localOplog.makeIterator();
    const auto topOfOplogSW = localOplogIter->next();
    if (!topOfOplogSW.isOK()) {
        return topOfOplogSW.getStatus();
    }
    const auto topOfOplogBSON = topOfOplogSW.getValue().first;
    return OpTime::parseFromOplogEntry(topOfOplogBSON);
}

void ReplicationRecoveryImpl::_truncateOplogTo(OperationContext* opCtx,
                                               Timestamp truncateAfterTimestamp) {
    Timer timer;

    // Fetch the oplog collection.
    const NamespaceString oplogNss(NamespaceString::kRsOplogNamespace);
    AutoGetDb autoDb(opCtx, oplogNss.db(), MODE_IX);
    Lock::CollectionLock oplogCollectionLoc(opCtx, oplogNss, MODE_X);
    Collection* oplogCollection =
        CollectionCatalog::get(opCtx).lookupCollectionByNamespace(opCtx, oplogNss);
    if (!oplogCollection) {
        fassertFailedWithStatusNoTrace(
            34418,
            Status(ErrorCodes::NamespaceNotFound,
                   str::stream() << "Can't find " << NamespaceString::kRsOplogNamespace.ns()));
    }

    // Find an oplog entry <= truncateAfterTimestamp.
    boost::optional<BSONObj> truncateAfterOplogEntryBSON =
        _storageInterface->findOplogEntryLessThanOrEqualToTimestamp(
            opCtx, oplogCollection, truncateAfterTimestamp);
    if (!truncateAfterOplogEntryBSON) {
        LOGV2_FATAL(21572,
                    "Reached end of oplog looking for an oplog entry lte to "
                    "{truncateAfterTimestamp} but did not find one",
                    "truncateAfterTimestamp"_attr = truncateAfterTimestamp.toBSON());
        fassertFailedNoTrace(40296);
    }

    // Parse the response.
    auto truncateAfterOplogEntry =
        fassert(51766, repl::OplogEntry::parse(truncateAfterOplogEntryBSON.get()));
    auto truncateAfterRecordId = RecordId(truncateAfterOplogEntry.getTimestamp().asULL());

    invariant(truncateAfterRecordId <= RecordId(truncateAfterTimestamp.asULL()),
              str::stream() << "Should have found a oplog entry timestamp lte to "
                            << truncateAfterTimestamp.toString() << ", but instead found "
                            << truncateAfterOplogEntry.toString() << " with timestamp "
                            << Timestamp(truncateAfterRecordId.repr()).toString());

    // Truncate the oplog AFTER the oplog entry found to be <= truncateAfterTimestamp.
    LOGV2(21553,
          "Truncating oplog from {timestamp} (non-inclusive). Truncate "
          "after point is {truncateAfterTimestamp}",
          "timestamp"_attr = truncateAfterOplogEntry.getTimestamp(),
          "truncateAfterTimestamp"_attr = truncateAfterTimestamp);

    oplogCollection->cappedTruncateAfter(opCtx, truncateAfterRecordId, /*inclusive*/ false);

    LOGV2(21554,
          "Replication recovery oplog truncation finished in: {ms}ms",
          "ms"_attr = timer.millis());
}

void ReplicationRecoveryImpl::_truncateOplogIfNeededAndThenClearOplogTruncateAfterPoint(
    OperationContext* opCtx, boost::optional<Timestamp> stableTimestamp) {

    Timestamp truncatePoint = _consistencyMarkers->getOplogTruncateAfterPoint(opCtx);

    if (truncatePoint.isNull()) {
        // There are no holes in the oplog that necessitate truncation.
        return;
    }

    if (stableTimestamp && !stableTimestamp->isNull() && truncatePoint <= stableTimestamp) {
        LOGV2(21556,
              "The oplog truncation point ({truncatePoint}) is equal to or earlier than the stable "
              "timestamp ({stableTimestamp}), so truncating after the stable timestamp instead",
              "truncatePoint"_attr = truncatePoint,
              "stableTimestamp"_attr = stableTimestamp.get());

        truncatePoint = stableTimestamp.get();
    }

    LOGV2(21557,
          "Removing unapplied oplog entries starting after: {truncatePoint}",
          "truncatePoint"_attr = truncatePoint.toBSON());
    _truncateOplogTo(opCtx, truncatePoint);

    // Clear the oplogTruncateAfterPoint now that we have removed any holes that might exist in the
    // oplog -- and so that we do not truncate future entries erroneously.
    _consistencyMarkers->setOplogTruncateAfterPoint(opCtx, Timestamp());
    opCtx->recoveryUnit()->waitUntilDurable(opCtx);
}

}  // namespace repl
}  // namespace mongo
