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
#define LOGV2_FOR_RECOVERY(ID, DLEVEL, MESSAGE, ...) \
    LOGV2_DEBUG_OPTIONS(ID, DLEVEL, {logv2::LogComponent::kStorageRecovery}, MESSAGE, ##__VA_ARGS__)


#include "mongo/db/repl/replication_recovery.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/oplog_applier_batcher.h"
#include "mongo/db/repl/oplog_applier_impl.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_interface.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/replication_consistency_markers.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/transaction_oplog_application.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_recovery.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/control/journal_flusher.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/attribute_storage.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

#include <cstddef>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {

MONGO_FAIL_POINT_DEFINE(hangAfterOplogTruncationInRollback);
MONGO_FAIL_POINT_DEFINE(skipResettingValidateFeaturesAsPrimaryAfterRecoveryOplogApplication);

namespace {

const auto kRecoveryBatchLogLevel = logv2::LogSeverity::Debug(2);
const auto kRecoveryOperationLogLevel = logv2::LogSeverity::Debug(3);

/**
 * ServerStatus section for oplog recovery.
 */
class RecoveryOplogApplierSSS : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const final {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement& configElement) const final {
        BSONObjBuilder recoveryOplogApplier;

        recoveryOplogApplier.append("numBatches", (int)numBatches.loadRelaxed());
        recoveryOplogApplier.append("numOpsApplied", (int)numOpsApplied.loadRelaxed());

        return recoveryOplogApplier.obj();
    }

    AtomicWord<size_t> numBatches{0};
    AtomicWord<size_t> numOpsApplied{0};
};

auto& recoveryOplogApplierSection =
    *ServerStatusSectionBuilder<RecoveryOplogApplierSSS>("recoveryOplogApplier").forShard();

/**
 * Tracks and logs operations applied during recovery.
 */
class RecoveryOplogApplierStats : public OplogApplier::Observer {
public:
    void onBatchBegin(const std::vector<OplogEntry>& batch) final {
        _numBatches++;
        recoveryOplogApplierSection.numBatches.fetchAndAdd(1);
        LOGV2_FOR_RECOVERY(24098,
                           kRecoveryBatchLogLevel.toInt(),
                           "About to apply operations in batch",
                           "numBatches"_attr = _numBatches,
                           "numOpsInBatch"_attr = batch.size(),
                           "firstOpTime"_attr = batch.front().getOpTime(),
                           "lastOpTime"_attr = batch.back().getOpTime(),
                           "numOpsAppliedBeforeThisBatch"_attr = _numOpsApplied);

        _numOpsApplied += batch.size();
        recoveryOplogApplierSection.numOpsApplied.fetchAndAdd(batch.size());
        if (shouldLog(::mongo::logv2::LogComponent::kStorageRecovery, kRecoveryOperationLogLevel)) {
            std::size_t i = 0;
            for (const auto& entry : batch) {
                i++;
                LOGV2_FOR_RECOVERY(24099,
                                   kRecoveryOperationLogLevel.toInt(),
                                   "Applying op during replication recovery",
                                   "opIndex"_attr = i,
                                   "batchSize"_attr = batch.size(),
                                   "numBatches"_attr = _numBatches,
                                   "oplogEntry"_attr = redact(entry.toBSONForLogging()));
            }
        }
    }

    void onBatchEnd(const StatusWith<OpTime>&, const std::vector<OplogEntry>&) final {}

    void complete(const OpTime& applyThroughOpTime) const {
        LOGV2(21536,
              "Completed oplog application for recovery",
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
        invariant(shard_role_details::getRecoveryUnit(opCtx)->getTimestampReadSource() ==
                  RecoveryUnit::ReadSource::kNoTimestamp);

        _client = std::make_unique<DBDirectClient>(opCtx);
        BSONObj predicate = _oplogApplicationEndPoint
            ? BSON("$gte" << _oplogApplicationStartPoint << "$lte" << *_oplogApplicationEndPoint)
            : BSON("$gte" << _oplogApplicationStartPoint);
        FindCommandRequest findRequest{NamespaceString::kRsOplogNamespace};
        findRequest.setFilter(BSON("ts" << predicate));
        // Don't kill the cursor just because applying a batch oplog takes a long time.
        findRequest.setNoCursorTimeout(true);
        _cursor = _client->find(std::move(findRequest));

        // Check that the first document matches our appliedThrough point then skip it since it's
        // already been applied.
        if (!_cursor->more()) {
            // This should really be impossible because we check above that the top of the oplog is
            // strictly > appliedThrough. If this fails it represents a serious bug in either the
            // storage engine or query's implementation of the oplog scan.
            logv2::DynamicAttributes attrs;
            attrs.add("oplogApplicationStartPoint", _oplogApplicationStartPoint.toBSON());
            if (_oplogApplicationEndPoint) {
                attrs.add("oplogApplicationEndPoint", _oplogApplicationEndPoint->toBSON());
            }

            LOGV2_FATAL_NOTRACE(
                40293, "Couldn't find any entries in the oplog, which should be impossible", attrs);
        }

        _opTimeAtStartPoint = fassert(40291, OpTime::parseFromOplogEntry(_cursor->nextSafe()));
        const auto firstTimestampFound = _opTimeAtStartPoint.getTimestamp();
        if (firstTimestampFound != _oplogApplicationStartPoint) {
            LOGV2_FATAL_NOTRACE(40292,
                                "Oplog entry at oplogApplicationStartPoint is missing",
                                "oplogApplicationStartPoint"_attr =
                                    _oplogApplicationStartPoint.toBSON(),
                                "firstTimestampFound"_attr = firstTimestampFound.toBSON());
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

    void push(OperationContext*,
              Batch::const_iterator,
              Batch::const_iterator,
              boost::optional<const Cost&> cost) final {
        MONGO_UNREACHABLE;
    }
    void waitForSpace(OperationContext*, const Cost&) final {
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
    bool waitForDataFor(Milliseconds, Interruptible*) final {
        MONGO_UNREACHABLE;
    }
    bool waitForDataUntil(Date_t, Interruptible*) final {
        MONGO_UNREACHABLE;
    }
    boost::optional<Value> lastObjectPushed(OperationContext*) const final {
        MONGO_UNREACHABLE;
    }

    OpTime getOpTimeAtStartPoint() {
        return _opTimeAtStartPoint;
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
    OpTime _opTimeAtStartPoint;
    std::unique_ptr<DBDirectClient> _client;
    std::unique_ptr<DBClientCursor> _cursor;
};

boost::optional<Timestamp> recoverFromOplogPrecursor(OperationContext* opCtx,
                                                     StorageInterface* storageInterface) {
    if (!storageInterface->supportsRecoveryTimestamp(opCtx->getServiceContext())) {
        LOGV2_FATAL_NOTRACE(
            50805,
            "Cannot recover from the oplog with a storage engine that does not support "
            "recover to stable timestamp");
    }

    // A non-existent recoveryTS means the checkpoint is unstable. If the recoveryTS exists but
    // is null, that means a stable checkpoint was taken at a null timestamp. This should never
    // happen.
    auto recoveryTS = storageInterface->getRecoveryTimestamp(opCtx->getServiceContext());
    if (recoveryTS && recoveryTS->isNull()) {
        LOGV2_FATAL_NOTRACE(
            50806, "Cannot recover from the oplog with stable checkpoint at null timestamp");
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
        LOGV2_FATAL_NOTRACE(31362, "Unexpected recovery needed, initial sync flag set");
    }

    const auto truncateAfterPoint = _consistencyMarkers->getOplogTruncateAfterPoint(opCtx);
    if (!truncateAfterPoint.isNull()) {
        LOGV2_FATAL_NOTRACE(31363,
                            "Unexpected recovery needed, oplog requires truncation",
                            "oplogTruncateAfterPoint"_attr = truncateAfterPoint.toString());
    }

    auto topOfOplogSW = _getTopOfOplog(opCtx);
    if (!topOfOplogSW.isOK()) {
        LOGV2_FATAL_NOTRACE(31364,
                            "Recovery not possible, no oplog found",
                            "error"_attr = topOfOplogSW.getStatus());
    }
    const auto topOfOplog = topOfOplogSW.getValue();

    const auto appliedThrough = _consistencyMarkers->getAppliedThrough(opCtx);
    if (!appliedThrough.isNull() && appliedThrough != topOfOplog) {
        LOGV2_FATAL_NOTRACE(
            31365,
            "Unexpected recovery needed, appliedThrough is not at top of oplog, indicating "
            "oplog has not been fully applied",
            "appliedThrough"_attr = appliedThrough.toString());
    }
}

void ReplicationRecoveryImpl::recoverFromOplogAsStandalone(OperationContext* opCtx,
                                                           bool duringInitialSync) {
    _duringInitialSync = duringInitialSync;
    ScopeGuard resetInitialSyncFlagOnExit([this] { _duringInitialSync = false; });
    auto recoveryTS = recoverFromOplogPrecursor(opCtx, _storageInterface);

    // We support only recovery from stable checkpoints during initial sync.
    invariant(!_duringInitialSync || recoveryTS);

    boost::optional<Timestamp> stableTimestamp = boost::none;
    if (recoveryTS || startupRecoveryForRestore) {
        if (startupRecoveryForRestore && !recoveryTS) {
            LOGV2_WARNING(5576601,
                          "Replication startup parameter 'startupRecoveryForRestore' is set and "
                          "recovering from an unstable checkpoint.  Assuming this is a resume of "
                          "an earlier attempt to recover for restore.");
        }

        // We pass in "none" for the stable timestamp so that recoverFromOplog asks storage
        // for the recoveryTimestamp just like on replica set recovery.
        stableTimestamp = recoverFromOplog(opCtx, boost::none);
    } else {
        if (gTakeUnstableCheckpointOnShutdown) {
            // Ensure 'recoverFromOplogAsStandalone' with 'takeUnstableCheckpointOnShutdown'
            // is safely idempotent when it succeeds.
            LOGV2(21537,
                  "Recovering from unstable checkpoint with 'takeUnstableCheckpointOnShutdown'. "
                  "Confirming that no oplog recovery is needed");
            _assertNoRecoveryNeededOnUnstableCheckpoint(opCtx);
            LOGV2(21538,
                  "Not doing any oplog recovery since there is an unstable checkpoint that is up "
                  "to date");
        } else {
            LOGV2_FATAL_NOTRACE(
                31229, "Cannot use 'recoverFromOplogAsStandalone' without a stable checkpoint");
        }
    }

    if (!_duringInitialSync) {
        // Initial sync will reconstruct prepared transactions when it is completely done.
        reconstructPreparedTransactions(opCtx,
                                        stableTimestamp
                                            ? OplogApplication::Mode::kStableRecovering
                                            : OplogApplication::Mode::kUnstableRecovering);
    }
}

void ReplicationRecoveryImpl::recoverFromOplogUpTo(OperationContext* opCtx, Timestamp endPoint) {
    uassert(
        ErrorCodes::InitialSyncActive,
        str::stream() << "Cannot recover from oplog while the node is performing an initial sync",
        !_consistencyMarkers->getInitialSyncFlag(opCtx));

    auto recoveryTS = recoverFromOplogPrecursor(opCtx, _storageInterface);
    if (!recoveryTS) {
        LOGV2_FATAL_NOTRACE(31399,
                            "Cannot use 'recoverToOplogTimestamp' without a stable checkpoint");
    }

    InReplicationRecovery inReplicationRecovery(opCtx->getServiceContext());

    // This may take an IS lock on the oplog collection.
    _truncateOplogIfNeededAndThenClearOplogTruncateAfterPoint(opCtx, &recoveryTS);

    boost::optional<Timestamp> startPoint =
        _storageInterface->getRecoveryTimestamp(opCtx->getServiceContext());
    fassert(31436, !!startPoint);  // No recovery timestamp, cannot recover from the oplog

    startPoint = _adjustStartPointIfNecessary(opCtx, startPoint.value());

    invariant(!endPoint.isNull());

    if (*startPoint == endPoint) {
        LOGV2(
            21540,
            "No oplog entries to apply for recovery. Start point is at the end point in the oplog",
            "startPoint"_attr = startPoint,
            "endPoint"_attr = endPoint);
        return;
    } else if (*startPoint > endPoint) {
        uasserted(ErrorCodes::BadValue,
                  str::stream() << "No oplog entries to apply for recovery. Start point '"
                                << startPoint->toString() << "' is beyond the end point '"
                                << endPoint.toString() << "' in the oplog.");
    }

    Timestamp appliedUpTo = _applyOplogOperations(
        opCtx, *startPoint, endPoint, RecoveryMode::kStartupFromStableTimestamp);
    if (appliedUpTo.isNull()) {
        LOGV2(21541,
              "No stored oplog entries to apply for recovery between startPoint (inclusive) and "
              "endPoint (inclusive)",
              "startPoint"_attr = startPoint->toString(),
              "endPoint"_attr = endPoint.toString());
    } else {
        invariant(appliedUpTo <= endPoint);
    }

    reconstructPreparedTransactions(opCtx, OplogApplication::Mode::kStableRecovering);
}

boost::optional<Timestamp> ReplicationRecoveryImpl::recoverFromOplog(
    OperationContext* opCtx, boost::optional<Timestamp> stableTimestamp) try {
    if (_consistencyMarkers->getInitialSyncFlag(opCtx)) {
        LOGV2(21542, "No recovery needed. Initial sync flag set");
        return stableTimestamp;  // Initial Sync will take over so no cleanup is needed.
    }

    InReplicationRecovery inReplicationRecovery(getGlobalServiceContext());

    // If we were passed in a stable timestamp, we are in rollback recovery and should recover from
    // that stable timestamp. Otherwise, we're recovering at startup. If this storage engine
    // supports recover to stable timestamp, we ask it for the recovery timestamp. If the storage
    // engine returns a timestamp, we recover from that point. However, if the storage engine
    // returns "none", the storage engine does not have a stable checkpoint and we must recover from
    // an unstable checkpoint instead.
    bool isRollbackRecovery = stableTimestamp != boost::none;
    const bool supportsRecoveryTimestamp =
        _storageInterface->supportsRecoveryTimestamp(opCtx->getServiceContext());
    if (!stableTimestamp && supportsRecoveryTimestamp) {
        stableTimestamp = _storageInterface->getRecoveryTimestamp(opCtx->getServiceContext());
    }

    // This may take an IS lock on the oplog collection.
    _truncateOplogIfNeededAndThenClearOplogTruncateAfterPoint(opCtx, &stableTimestamp);

    hangAfterOplogTruncationInRollback.pauseWhileSet();

    auto topOfOplogSW = _getTopOfOplog(opCtx);
    if (topOfOplogSW.getStatus() == ErrorCodes::CollectionIsEmpty ||
        topOfOplogSW.getStatus() == ErrorCodes::NamespaceNotFound) {
        // Oplog is empty. There are no oplog entries to apply, so we exit recovery and go into
        // initial sync.
        LOGV2(21543, "No oplog entries to apply for recovery. Oplog is empty");
        return stableTimestamp;
    }
    fassert(40290, topOfOplogSW);
    const auto topOfOplog = topOfOplogSW.getValue();

    if (stableTimestamp) {
        // For recovery from a stable timestamp, data is already consistent before oplog recovery.
        // For initial sync, we only mark data consistent when the entire initial sync process
        // completes.
        if (!_duringInitialSync) {
            ReplicationCoordinator::get(opCtx)->setConsistentDataAvailable(
                opCtx,
                /*isDataMajorityCommitted=*/true);
        }
        invariant(supportsRecoveryTimestamp);
        const auto recoveryMode = isRollbackRecovery ? RecoveryMode::kRollbackFromStableTimestamp
                                                     : RecoveryMode::kStartupFromStableTimestamp;
        _recoverFromStableTimestamp(opCtx, *stableTimestamp, topOfOplog, recoveryMode);
    } else {
        _recoverFromUnstableCheckpoint(
            opCtx, _consistencyMarkers->getAppliedThrough(opCtx), topOfOplog);
        // For recovery from an unstable timestamp, data is consistent after oplog recovery.
        // For initial sync, we only mark data consistent when the entire initial sync process
        // completes.
        if (!_duringInitialSync) {
            ReplicationCoordinator::get(opCtx)->setConsistentDataAvailable(
                opCtx,
                /*isDataMajorityCommitted=*/false);
        }
    }
    return stableTimestamp;
} catch (const ExceptionFor<ErrorCodes::DuplicateKey>& e) {
    auto info = e.extraInfo<DuplicateKeyErrorInfo>();
    LOGV2_FATAL_CONTINUE(5689601,
                         "Caught duplicate key exception during replication recovery",
                         "keyPattern"_attr = info->getKeyPattern(),
                         "keyValue"_attr = redact(info->getDuplicatedKeyValue()),
                         "error"_attr = redact(e.reason()));
    std::terminate();
} catch (...) {
    LOGV2_FATAL_CONTINUE(
        21570, "Caught exception during replication recovery", "error"_attr = exceptionToStatus());
    std::terminate();
}

void ReplicationRecoveryImpl::truncateOplogToTimestamp(OperationContext* opCtx,
                                                       Timestamp truncateAfterTimestamp) {
    _truncateOplogTo(
        opCtx, truncateAfterTimestamp, std::make_unique<boost::optional<Timestamp>>().get());
}


// TODO SERVER-87432: Once the replication recovery code is refactored, address code duplication in
// this function.
void ReplicationRecoveryImpl::applyOplogEntriesForRestore(OperationContext* opCtx,
                                                          Timestamp stableTimestamp) {
    invariant(storageGlobalParams.magicRestore);
    InReplicationRecovery inReplicationRecovery(getGlobalServiceContext());
    invariant(_storageInterface->supportsRecoveryTimestamp(opCtx->getServiceContext()));

    auto topOfOplogSW = _getTopOfOplog(opCtx);
    // The oplog must exist and contain entries.
    fassert(8290703, topOfOplogSW);
    const auto topOfOplog = topOfOplogSW.getValue();

    // Note that this function skips setting the initial data timestamp at the end of oplog
    // application, as we expect the restore process to do that once restore completes.
    _recoverFromStableTimestamp(
        opCtx, stableTimestamp, topOfOplog, RecoveryMode::kStartupFromStableTimestamp);
    reconstructPreparedTransactions(opCtx, OplogApplication::Mode::kStableRecovering);
}

void ReplicationRecoveryImpl::_recoverFromStableTimestamp(OperationContext* opCtx,
                                                          Timestamp stableTimestamp,
                                                          OpTime topOfOplog,
                                                          RecoveryMode recoveryMode) {
    invariant(!stableTimestamp.isNull());
    invariant(!topOfOplog.isNull());

    LOGV2(21544,
          "Recovering from stable timestamp",
          "stableTimestamp"_attr = stableTimestamp,
          "topOfOplog"_attr = topOfOplog,
          "appliedThrough"_attr = _consistencyMarkers->getAppliedThrough(opCtx));

    LOGV2(21545,
          "Starting recovery oplog application at the stable timestamp",
          "stableTimestamp"_attr = stableTimestamp);

    if (recoveryMode == RecoveryMode::kStartupFromStableTimestamp &&
        (startupRecoveryForRestore || _duringInitialSync)) {
        if (startupRecoveryForRestore) {
            LOGV2_WARNING(5576600,
                          "Replication startup parameter 'startupRecoveryForRestore' is set, "
                          "recovering without preserving history before top of oplog.");
        }
        // Take only unstable checkpoints during the recovery process.
        _storageInterface->setInitialDataTimestamp(opCtx->getServiceContext(),
                                                   Timestamp::kAllowUnstableCheckpointsSentinel);
    }
    auto startPoint = _adjustStartPointIfNecessary(opCtx, stableTimestamp);
    _applyToEndOfOplog(opCtx, startPoint, topOfOplog.getTimestamp(), recoveryMode);
    const bool inRestore = startupRecoveryForRestore || storageGlobalParams.magicRestore;
    if (recoveryMode == RecoveryMode::kStartupFromStableTimestamp &&
        (inRestore || _duringInitialSync)) {
        // For a magic restore, the initial data timestamp is set at the end of the restore process.
        if (!storageGlobalParams.magicRestore) {
            _storageInterface->setInitialDataTimestamp(opCtx->getServiceContext(),
                                                       topOfOplog.getTimestamp());
        }
        // Clear the appliedThrough so this reflects in the first stable checkpoint. See
        // _recoverFromUnstableCheckpoint for details.
        if (!gTakeUnstableCheckpointOnShutdown) {
            _consistencyMarkers->clearAppliedThrough(opCtx);
        }
    }
}

void ReplicationRecoveryImpl::_recoverFromUnstableCheckpoint(OperationContext* opCtx,
                                                             OpTime appliedThrough,
                                                             OpTime topOfOplog) {
    invariant(!topOfOplog.isNull());
    LOGV2(21546,
          "Recovering from an unstable checkpoint",
          "topOfOplog"_attr = topOfOplog,
          "appliedThrough"_attr = appliedThrough);

    if (appliedThrough.isNull()) {
        // The appliedThrough would be null if we shut down cleanly or crashed as a primary. Either
        // way we are consistent at the top of the oplog.
        LOGV2(21547, "No oplog entries to apply for recovery. appliedThrough is null");
    } else {
        // If the appliedThrough is not null, then we shut down uncleanly during oplog application
        // for restore and must apply from the appliedThrough to the top of the oplog.
        LOGV2(21548,
              "Starting recovery oplog application at the appliedThrough through the top of the "
              "oplog",
              "appliedThrough"_attr = appliedThrough,
              "topOfOplog"_attr = topOfOplog);

        // We advance both appliedThrough and the oldest timestamp together during oplog application
        // for restore after each batch, so the oldest timestamp cannot be behind appliedThrough.
        opCtx->getServiceContext()->getStorageEngine()->setOldestTimestamp(
            appliedThrough.getTimestamp(), false /*force*/);

        if (startupRecoveryForRestore) {
            // When we're recovering for a restore, we may be recovering a large number of oplog
            // entries, so we want to take unstable checkpoints to reduce cache pressure and allow
            // resumption in case of a crash.
            _storageInterface->setInitialDataTimestamp(
                opCtx->getServiceContext(), Timestamp::kAllowUnstableCheckpointsSentinel);
        }
        _applyToEndOfOplog(opCtx,
                           appliedThrough.getTimestamp(),
                           topOfOplog.getTimestamp(),
                           RecoveryMode::kStartupFromUnstableCheckpoint);
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
    opCtx->getServiceContext()->getStorageEngine()->waitUntilUnjournaledWritesDurable(
        opCtx,
        /*stableCheckpoint*/ true);

    // Now that we have set the initial data timestamp and taken an unstable checkpoint with the
    // appliedThrough being the topOfOplog, it is safe to clear the appliedThrough. This minValid
    // document write would never get into an unstable checkpoint because we will no longer take
    // unstable checkpoints from now on, except when gTakeUnstableCheckpointOnShutdown is true in
    // certain standalone restore cases. Additionally, future stable checkpoints are guaranteed to
    // be taken with the appliedThrough cleared. Therefore, if this node crashes before the first
    // stable checkpoint, it can safely recover from the last unstable checkpoint with a correct
    // appliedThrough value. Otherwise, if this node crashes after the first stable checkpoint, it
    // can safely recover from a stable checkpoint (with an empty appliedThrough).
    if (!gTakeUnstableCheckpointOnShutdown) {
        _consistencyMarkers->clearAppliedThrough(opCtx);
    }
}

void ReplicationRecoveryImpl::_applyToEndOfOplog(OperationContext* opCtx,
                                                 const Timestamp& oplogApplicationStartPoint,
                                                 const Timestamp& topOfOplog,
                                                 const RecoveryMode recoveryMode) {
    invariant(!oplogApplicationStartPoint.isNull());
    invariant(!topOfOplog.isNull());

    // Check if we have any unapplied ops in our oplog. It is important that this is done after
    // deleting the ragged end of the oplog.
    if (oplogApplicationStartPoint == topOfOplog) {
        LOGV2(21549,
              "No oplog entries to apply for recovery. Start point is at the top of the oplog");
        return;  // We've applied all the valid oplog we have.
    } else if (oplogApplicationStartPoint > topOfOplog) {
        LOGV2_FATAL_NOTRACE(40313,
                            "Applied op oplogApplicationStartPoint not found",
                            "oplogApplicationStartPoint"_attr = oplogApplicationStartPoint.toBSON(),
                            "topOfOplog"_attr = topOfOplog.toBSON());
    }

    Timestamp appliedUpTo =
        _applyOplogOperations(opCtx, oplogApplicationStartPoint, topOfOplog, recoveryMode);
    invariant(!appliedUpTo.isNull());
    invariant(appliedUpTo == topOfOplog,
              str::stream() << "Did not apply to top of oplog. Applied through: "
                            << appliedUpTo.toString()
                            << ". Top of oplog: " << topOfOplog.toString());
}

Timestamp ReplicationRecoveryImpl::_applyOplogOperations(OperationContext* opCtx,
                                                         const Timestamp& startPoint,
                                                         const Timestamp& endPoint,
                                                         RecoveryMode recoveryMode) {

    // Make sure we skip validation checks that are only intended for primaries while recovering.
    auto validateValue = serverGlobalParams.validateFeaturesAsPrimary.load();
    ON_BLOCK_EXIT([validateValue] {
        if (MONGO_unlikely(
                skipResettingValidateFeaturesAsPrimaryAfterRecoveryOplogApplication.shouldFail())) {
            LOGV2(5717600,
                  "Hit skipResettingValidateFeaturesAsPrimaryAfterRecoveryOplogApplication "
                  "failpoint");
            return;
        }
        serverGlobalParams.validateFeaturesAsPrimary.store(validateValue);
    });
    serverGlobalParams.validateFeaturesAsPrimary.store(false);

    // The oplog buffer will fetch all entries >= the startPoint timestamp, but it skips the first
    // op on startup, which is why the startPoint is described as "exclusive".
    LOGV2(21550,
          "Replaying stored operations from startPoint (exclusive) to endPoint (inclusive)",
          "startPoint"_attr = startPoint,
          "endPoint"_attr = endPoint);

    OplogBufferLocalOplog oplogBuffer(startPoint, endPoint);
    oplogBuffer.startup(opCtx);

    RecoveryOplogApplierStats stats;

    auto oplogApplicationMode = (recoveryMode == RecoveryMode::kStartupFromStableTimestamp ||
                                 recoveryMode == RecoveryMode::kRollbackFromStableTimestamp)
        ? OplogApplication::Mode::kStableRecovering
        : OplogApplication::Mode::kUnstableRecovering;
    auto workerPool = makeReplWorkerPool();
    auto* replCoord = ReplicationCoordinator::get(opCtx);
    OplogApplierImpl oplogApplier(nullptr,
                                  &oplogBuffer,
                                  &stats,
                                  replCoord,
                                  _consistencyMarkers,
                                  _storageInterface,
                                  OplogApplier::Options(oplogApplicationMode),
                                  workerPool.get());

    OplogApplier::BatchLimits batchLimits;
    batchLimits.bytes = getBatchLimitOplogBytes(opCtx, _storageInterface);
    batchLimits.ops = getBatchLimitOplogEntries();

    // If we're doing unstable checkpoints during the recovery process (as we do during a recovery
    // for restore), we need to advance the consistency marker for each batch so the next time we
    // recover we won't start all the way over. Further, we can advance the oldest timestamp to
    // avoid keeping too much history.
    //
    // If we're recovering from a stable checkpoint (except the special startupRecoveryForRestore
    // mode, which discards history before the top of oplog), we aren't doing new checkpoints during
    // recovery so there is no point in advancing the consistency marker and we cannot advance
    // "oldest" becaue it would be later than "stable".
    const bool inRestore = startupRecoveryForRestore || storageGlobalParams.magicRestore;
    const bool advanceTimestampsEachBatch = (inRestore || _duringInitialSync) &&
        (recoveryMode == RecoveryMode::kStartupFromStableTimestamp ||
         recoveryMode == RecoveryMode::kStartupFromUnstableCheckpoint);

    OpTime applyThroughOpTime;
    std::vector<OplogEntry> batch;
    while (
        !(batch =
              fassert(50763, oplogApplier.getNextApplierBatch(opCtx, batchLimits)).releaseBatch())
             .empty()) {
        if (advanceTimestampsEachBatch && applyThroughOpTime.isNull()) {
            // We must set appliedThrough before applying anything at all, so we know
            // any unstable checkpoints we take are "dirty".  A null appliedThrough indicates
            // a clean shutdown which may not be the case if we had started applying a batch.
            _consistencyMarkers->setAppliedThrough(opCtx, oplogBuffer.getOpTimeAtStartPoint());
        }
        applyThroughOpTime = uassertStatusOK(oplogApplier.applyOplogBatch(opCtx, std::move(batch)));
        if (advanceTimestampsEachBatch) {
            invariant(!applyThroughOpTime.isNull());
            _consistencyMarkers->setAppliedThrough(opCtx, applyThroughOpTime);
            replCoord->getServiceContext()->getStorageEngine()->setStableTimestamp(
                applyThroughOpTime.getTimestamp(), false /*force*/);
            replCoord->setOldestTimestamp(applyThroughOpTime.getTimestamp());
        }
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
    // It is safe to do startup recovery from an unstable checkpoint provided we recover to the
    // end of the oplog and discard history before it, as _recoverFromUnstableCheckpoint does.
    if (!advanceTimestampsEachBatch) {
        if (recoveryMode == RecoveryMode::kStartupFromStableTimestamp) {
            // Advance all_durable timestamp to the last applied timestamp. This is needed because
            // the last applied entry during recovery could be a no-op entry which doesn't do
            // timestamped writes or advance the all_durable timestamp. We may set the stable
            // timestamp to this last applied timestamp later and we require the stable timestamp to
            // be less than or equal to the all_durable timestamp.
            WriteUnitOfWork wunit(opCtx);
            uassertStatusOK(shard_role_details::getRecoveryUnit(opCtx)->setTimestamp(
                applyThroughOpTime.getTimestamp()));
            shard_role_details::getRecoveryUnit(opCtx)->setOrderedCommit(false);
            wunit.commit();
        } else {
            _consistencyMarkers->setAppliedThrough(opCtx, applyThroughOpTime);
        }
    }
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
                                               Timestamp truncateAfterTimestamp,
                                               boost::optional<Timestamp>* stableTimestamp) {
    Timer timer;

    // Fetch the oplog collection.
    const NamespaceString oplogNss(NamespaceString::kRsOplogNamespace);
    AutoGetDb autoDb(opCtx, oplogNss.dbName(), MODE_IX);
    Lock::CollectionLock oplogCollectionLoc(opCtx, oplogNss, MODE_X);
    auto oplogCollection =
        CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, oplogNss);
    if (!oplogCollection) {
        fassertFailedWithStatusNoTrace(
            34418,
            Status(ErrorCodes::NamespaceNotFound,
                   str::stream() << "Can't find "
                                 << NamespaceString::kRsOplogNamespace.toStringForErrorMsg()));
    }

    // Find an oplog entry optime <= truncateAfterTimestamp.
    // TODO(SERVER-103411): Investigate usage validity of CollectionPtr::CollectionPtr_UNSAFE
    auto truncateAfterOpTimeAndWallTime =
        _storageInterface->findOplogOpTimeLessThanOrEqualToTimestamp(
            opCtx, CollectionPtr::CollectionPtr_UNSAFE(oplogCollection), truncateAfterTimestamp);
    if (!truncateAfterOpTimeAndWallTime) {
        LOGV2_FATAL_NOTRACE(40296,
                            "Reached end of oplog looking for an oplog entry lte to "
                            "oplogTruncateAfterPoint but did not find one",
                            "oplogTruncateAfterPoint"_attr = truncateAfterTimestamp.toBSON());
    }

    auto truncateAfterOplogEntryTs = truncateAfterOpTimeAndWallTime->opTime.getTimestamp();
    auto truncateAfterRecordId = RecordId(truncateAfterOplogEntryTs.asULL());

    // Truncate the oplog AFTER the oplog entry found to be <= truncateAfterTimestamp.
    LOGV2(21553,
          "Truncating oplog from truncateAfterOplogEntryTimestamp (non-inclusive)",
          "truncateAfterOplogEntryTimestamp"_attr = truncateAfterOplogEntryTs,
          "oplogTruncateAfterPoint"_attr = truncateAfterTimestamp);

    if (*stableTimestamp && (**stableTimestamp) > truncateAfterOplogEntryTs) {
        // Truncating the oplog sets the storage engine's maximum durable timestamp to the new top
        // of the oplog.  It is illegal for this maximum durable timestamp to be before the oldest
        // timestamp, so if the oldest timestamp is ahead of that point, we need to move it back.
        // Since the stable timestamp is never behind the oldest and also must not be ahead of the
        // maximum durable timestamp, it has to be moved back as well.  This usually happens when
        // the truncateAfterTimestamp does not exist in the oplog because there was a hole open when
        // we crashed; in that case the oldest timestamp and the stable timestamp will be the
        // timestamp immediately prior to the hole.
        LOGV2_DEBUG(5104900,
                    0,
                    "Resetting stable and oldest timestamp to oplog entry we truncate after",
                    "stableTimestamp"_attr = *stableTimestamp,
                    "truncateAfterRecordTimestamp"_attr = truncateAfterOplogEntryTs);
        // We're moving the stable timestamp backwards, so we need to force it.
        const bool force = true;
        opCtx->getServiceContext()->getStorageEngine()->setStableTimestamp(
            truncateAfterOplogEntryTs, force);
        **stableTimestamp = truncateAfterOplogEntryTs;

        // The initialDataTimestamp may also be at the hole; move it back.
        auto initialDataTimestamp =
            opCtx->getServiceContext()->getStorageEngine()->getInitialDataTimestamp();
        if (initialDataTimestamp > truncateAfterOplogEntryTs) {
            _storageInterface->setInitialDataTimestamp(opCtx->getServiceContext(),
                                                       truncateAfterOplogEntryTs);
        }
    }

    WriteUnitOfWork wunit(opCtx);
    RecordStore::Capped::TruncateAfterResult result =
        oplogCollection->getRecordStore()->capped()->truncateAfter(
            opCtx,
            *shard_role_details::getRecoveryUnit(opCtx),
            truncateAfterRecordId,
            false /*inclusive*/);
    wunit.commit();
    if (result.recordsRemoved > 0) {
        if (auto truncateMarkers = LocalOplogInfo::get(opCtx)->getTruncateMarkers()) {
            truncateMarkers->updateMarkersAfterCappedTruncateAfter(
                result.recordsRemoved, result.bytesRemoved, result.firstRemovedId);
        }
    }

    LOGV2(21554,
          "Replication recovery oplog truncation finished",
          "durationMillis"_attr = timer.millis());
}

void ReplicationRecoveryImpl::_truncateOplogIfNeededAndThenClearOplogTruncateAfterPoint(
    OperationContext* opCtx, boost::optional<Timestamp>* stableTimestamp) {

    Timestamp truncatePoint = _consistencyMarkers->getOplogTruncateAfterPoint(opCtx);

    if (truncatePoint.isNull()) {
        // There are no holes in the oplog that necessitate truncation.
        return;
    }

    if (*stableTimestamp && !(*stableTimestamp)->isNull() && truncatePoint <= *stableTimestamp) {
        LOGV2(21556,
              "The oplog truncation point is equal to or earlier than the stable timestamp, so "
              "truncating after the stable timestamp instead",
              "truncatePoint"_attr = truncatePoint,
              "stableTimestamp"_attr = (*stableTimestamp).value());

        truncatePoint = (*stableTimestamp).value();
    }

    LOGV2(21557,
          "Removing unapplied oplog entries after oplogTruncateAfterPoint",
          "oplogTruncateAfterPoint"_attr = truncatePoint.toBSON());
    _truncateOplogTo(opCtx, truncatePoint, stableTimestamp);

    // Clear the oplogTruncateAfterPoint now that we have removed any holes that might exist in the
    // oplog -- and so that we do not truncate future entries erroneously.
    _consistencyMarkers->setOplogTruncateAfterPoint(opCtx, Timestamp());
    JournalFlusher::get(opCtx)->waitForJournalFlush();
}

Timestamp ReplicationRecoveryImpl::_adjustStartPointIfNecessary(OperationContext* opCtx,
                                                                Timestamp startPoint) {
    // Set up read on oplog collection.
    AutoGetOplogFastPath oplogRead(opCtx, OplogAccessMode::kRead);
    const auto& oplogCollection = oplogRead.getCollection();
    if (!oplogCollection) {
        LOGV2_FATAL_NOTRACE(
            5466600,
            "Cannot find oplog collection for recovery oplog application start point",
            "oplogNss"_attr = NamespaceString::kRsOplogNamespace);
    }

    auto adjustmentOpTimeAndWallTime = _storageInterface->findOplogOpTimeLessThanOrEqualToTimestamp(
        opCtx, oplogCollection, startPoint);

    if (!adjustmentOpTimeAndWallTime) {
        LOGV2_FATAL_NOTRACE(
            5466601,
            "Could not find LTE oplog entry for oplog application start point for recovery",
            "startPoint"_attr = startPoint);
    }

    auto adjustmentTimestamp = adjustmentOpTimeAndWallTime->opTime.getTimestamp();

    if (startPoint != adjustmentTimestamp) {
        LOGV2(5466603,
              "Start point for recovery oplog application not found in oplog. Adjusting start "
              "point to earlier entry",
              "oldStartPoint"_attr = startPoint,
              "newStartPoint"_attr = adjustmentTimestamp);
        invariant(adjustmentTimestamp < startPoint);
        return adjustmentTimestamp;
    }

    LOGV2(5466604,
          "Start point for recovery oplog application exists in oplog. No adjustment necessary",
          "startPoint"_attr = startPoint);
    return startPoint;
}

}  // namespace repl
}  // namespace mongo
