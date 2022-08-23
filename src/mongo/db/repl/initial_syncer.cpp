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


#include "mongo/platform/basic.h"

#include "initial_syncer.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "mongo/base/counter.h"
#include "mongo/base/status.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/fetcher.h"
#include "mongo/client/remote_command_retry_scheduler.h"
#include "mongo/db/commands/feature_compatibility_version_parser.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/all_database_cloner.h"
#include "mongo/db/repl/initial_sync_state.h"
#include "mongo/db/repl/initial_syncer_common_stats.h"
#include "mongo/db/repl/initial_syncer_factory.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/db/repl/oplog_fetcher.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/replication_consistency_markers.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/sync_source_selector.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/repl/transaction_oplog_application.h"
#include "mongo/db/session/session_txn_record_gen.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/destructor_guard.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/system_clock_source.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"
#include "mongo/util/version/releases.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplicationInitialSync


namespace mongo {
namespace repl {

// Failpoint for initial sync
MONGO_FAIL_POINT_DEFINE(failInitialSyncWithBadHost);

// Failpoint which fails initial sync and leaves an oplog entry in the buffer.
MONGO_FAIL_POINT_DEFINE(failInitSyncWithBufferedEntriesLeft);

// Failpoint which causes the initial sync function to hang after getting the oldest active
// transaction timestamp from the sync source.
MONGO_FAIL_POINT_DEFINE(initialSyncHangAfterGettingBeginFetchingTimestamp);

// Failpoint which causes the initial sync function to hang before creating shared data and
// splitting control flow between the oplog fetcher and the cloners.
MONGO_FAIL_POINT_DEFINE(initialSyncHangBeforeSplittingControlFlow);

// Failpoint which causes the initial sync function to hang before copying databases.
MONGO_FAIL_POINT_DEFINE(initialSyncHangBeforeCopyingDatabases);

// Failpoint which causes the initial sync function to hang before finishing.
MONGO_FAIL_POINT_DEFINE(initialSyncHangBeforeFinish);

// Failpoint which causes the initial sync function to hang before creating the oplog.
MONGO_FAIL_POINT_DEFINE(initialSyncHangBeforeCreatingOplog);

// Failpoint which stops the applier.
MONGO_FAIL_POINT_DEFINE(rsSyncApplyStop);

// Failpoint which causes the initial sync function to hang after cloning all databases.
MONGO_FAIL_POINT_DEFINE(initialSyncHangAfterDataCloning);

// Failpoint which skips clearing _initialSyncState after a successful initial sync attempt.
MONGO_FAIL_POINT_DEFINE(skipClearInitialSyncState);

// Failpoint which causes the initial sync function to fail and hang before starting a new attempt.
MONGO_FAIL_POINT_DEFINE(failAndHangInitialSync);

// Failpoint which fails initial sync before it applies the next batch of oplog entries.
MONGO_FAIL_POINT_DEFINE(failInitialSyncBeforeApplyingBatch);

// Failpoint which fasserts if applying a batch fails.
MONGO_FAIL_POINT_DEFINE(initialSyncFassertIfApplyingBatchFails);

// Failpoint which causes the initial sync function to hang before stopping the oplog fetcher.
MONGO_FAIL_POINT_DEFINE(initialSyncHangBeforeCompletingOplogFetching);

// Failpoint which causes the initial sync function to hang before choosing a sync source.
MONGO_FAIL_POINT_DEFINE(initialSyncHangBeforeChoosingSyncSource);

// Failpoint which causes the initial sync function to hang after finishing.
MONGO_FAIL_POINT_DEFINE(initialSyncHangAfterFinish);

// Failpoints for synchronization, shared with cloners.
extern FailPoint initialSyncFuzzerSynchronizationPoint1;
extern FailPoint initialSyncFuzzerSynchronizationPoint2;

namespace {
using namespace executor;
using CallbackArgs = executor::TaskExecutor::CallbackArgs;
using Event = executor::TaskExecutor::EventHandle;
using Handle = executor::TaskExecutor::CallbackHandle;
using QueryResponseStatus = StatusWith<Fetcher::QueryResponse>;
using UniqueLock = stdx::unique_lock<Latch>;
using LockGuard = stdx::lock_guard<Latch>;

// Used to reset the oldest timestamp during initial sync to a non-null timestamp.
const Timestamp kTimestampOne(0, 1);

ServiceContext::UniqueOperationContext makeOpCtx() {
    return cc().makeOperationContext();
}

StatusWith<OpTimeAndWallTime> parseOpTimeAndWallTime(const QueryResponseStatus& fetchResult) {
    if (!fetchResult.isOK()) {
        return fetchResult.getStatus();
    }
    const auto docs = fetchResult.getValue().documents;
    const auto hasDoc = docs.begin() != docs.end();
    if (!hasDoc) {
        return StatusWith<OpTimeAndWallTime>{ErrorCodes::NoMatchingDocument,
                                             "no oplog entry found"};
    }

    return OpTimeAndWallTime::parseOpTimeAndWallTimeFromOplogEntry(docs.front());
}

void pauseAtInitialSyncFuzzerSyncronizationPoints(std::string msg) {
    // Set and unset by the InitialSyncTest fixture to cause initial sync to pause so that the
    // Initial Sync Fuzzer can run commands on the sync source.
    if (MONGO_unlikely(initialSyncFuzzerSynchronizationPoint1.shouldFail())) {
        LOGV2(21158,
              "initialSyncFuzzerSynchronizationPoint1 fail point enabled",
              "failpointMessage"_attr = msg);
        initialSyncFuzzerSynchronizationPoint1.pauseWhileSet();
    }

    if (MONGO_unlikely(initialSyncFuzzerSynchronizationPoint2.shouldFail())) {
        LOGV2(21160, "initialSyncFuzzerSynchronizationPoint2 fail point enabled");
        initialSyncFuzzerSynchronizationPoint2.pauseWhileSet();
    }
}

}  // namespace

ServiceContext::ConstructorActionRegisterer initialSyncerRegisterer(
    "InitialSyncerRegisterer",
    {"InitialSyncerFactoryRegisterer"} /* dependency list */,
    [](ServiceContext* service) {
        InitialSyncerFactory::get(service)->registerInitialSyncer(
            "logical",
            [](InitialSyncerInterface::Options opts,
               std::unique_ptr<DataReplicatorExternalState> dataReplicatorExternalState,
               ThreadPool* writerPool,
               StorageInterface* storage,
               ReplicationProcess* replicationProcess,
               const InitialSyncerInterface::OnCompletionFn& onCompletion) {
                return std::make_shared<InitialSyncer>(opts,
                                                       std::move(dataReplicatorExternalState),
                                                       writerPool,
                                                       storage,
                                                       replicationProcess,
                                                       onCompletion);
            });
    });

InitialSyncer::InitialSyncer(
    InitialSyncerInterface::Options opts,
    std::unique_ptr<DataReplicatorExternalState> dataReplicatorExternalState,
    ThreadPool* writerPool,
    StorageInterface* storage,
    ReplicationProcess* replicationProcess,
    const OnCompletionFn& onCompletion)
    : _fetchCount(0),
      _opts(opts),
      _dataReplicatorExternalState(std::move(dataReplicatorExternalState)),
      _exec(_dataReplicatorExternalState->getSharedTaskExecutor()),
      _clonerExec(_exec),
      _writerPool(writerPool),
      _storage(storage),
      _replicationProcess(replicationProcess),
      _onCompletion(onCompletion),
      _createClientFn(
          [] { return std::make_unique<DBClientConnection>(true /* autoReconnect */); }),
      _createOplogFetcherFn(CreateOplogFetcherFn::get()) {
    uassert(ErrorCodes::BadValue, "task executor cannot be null", _exec);
    uassert(ErrorCodes::BadValue, "invalid storage interface", _storage);
    uassert(ErrorCodes::BadValue, "invalid replication process", _replicationProcess);
    uassert(ErrorCodes::BadValue, "invalid getMyLastOptime function", _opts.getMyLastOptime);
    uassert(ErrorCodes::BadValue, "invalid setMyLastOptime function", _opts.setMyLastOptime);
    uassert(ErrorCodes::BadValue, "invalid resetOptimes function", _opts.resetOptimes);
    uassert(ErrorCodes::BadValue, "invalid sync source selector", _opts.syncSourceSelector);
    uassert(ErrorCodes::BadValue, "callback function cannot be null", _onCompletion);
}

InitialSyncer::~InitialSyncer() {
    DESTRUCTOR_GUARD({
        shutdown().transitional_ignore();
        join();
    });
}

bool InitialSyncer::isActive() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _isActive_inlock();
}

bool InitialSyncer::_isActive_inlock() const {
    return State::kRunning == _state || State::kShuttingDown == _state;
}

std::string InitialSyncer::getInitialSyncMethod() const {
    return "logical";
}

Status InitialSyncer::startup(OperationContext* opCtx,
                              std::uint32_t initialSyncMaxAttempts) noexcept {
    invariant(opCtx);
    invariant(initialSyncMaxAttempts >= 1U);

    stdx::lock_guard<Latch> lock(_mutex);
    switch (_state) {
        case State::kPreStart:
            _state = State::kRunning;
            break;
        case State::kRunning:
            return Status(ErrorCodes::IllegalOperation, "initial syncer already started");
        case State::kShuttingDown:
            return Status(ErrorCodes::ShutdownInProgress, "initial syncer shutting down");
        case State::kComplete:
            return Status(ErrorCodes::ShutdownInProgress, "initial syncer completed");
    }

    _setUp_inlock(opCtx, initialSyncMaxAttempts);

    // Start first initial sync attempt.
    std::uint32_t initialSyncAttempt = 0;
    _attemptExec = std::make_unique<executor::ScopedTaskExecutor>(
        _exec, Status(ErrorCodes::CallbackCanceled, "Initial Sync Attempt Canceled"));
    _clonerAttemptExec = std::make_unique<executor::ScopedTaskExecutor>(
        _clonerExec, Status(ErrorCodes::CallbackCanceled, "Initial Sync Attempt Canceled"));
    auto status = _scheduleWorkAndSaveHandle_inlock(
        [=](const executor::TaskExecutor::CallbackArgs& args) {
            _startInitialSyncAttemptCallback(args, initialSyncAttempt, initialSyncMaxAttempts);
        },
        &_startInitialSyncAttemptHandle,
        str::stream() << "_startInitialSyncAttemptCallback-" << initialSyncAttempt);

    if (!status.isOK()) {
        _state = State::kComplete;
        return status;
    }

    return Status::OK();
}

Status InitialSyncer::shutdown() {
    stdx::lock_guard<Latch> lock(_mutex);
    switch (_state) {
        case State::kPreStart:
            // Transition directly from PreStart to Complete if not started yet.
            _state = State::kComplete;
            return Status::OK();
        case State::kRunning:
            _state = State::kShuttingDown;
            break;
        case State::kShuttingDown:
        case State::kComplete:
            // Nothing to do if we are already in ShuttingDown or Complete state.
            return Status::OK();
    }

    _cancelRemainingWork_inlock();

    return Status::OK();
}

void InitialSyncer::cancelCurrentAttempt() {
    stdx::lock_guard lk(_mutex);
    if (_isActive_inlock()) {
        LOGV2_DEBUG(4427201,
                    1,
                    "Cancelling the current initial sync attempt.",
                    "currentAttempt"_attr = _stats.failedInitialSyncAttempts + 1);
        _cancelRemainingWork_inlock();
    } else {
        LOGV2_DEBUG(4427202,
                    1,
                    "There is no initial sync attempt to cancel because the initial syncer is not "
                    "currently active.");
    }
}

void InitialSyncer::_cancelRemainingWork_inlock() {
    _cancelHandle_inlock(_startInitialSyncAttemptHandle);
    _cancelHandle_inlock(_chooseSyncSourceHandle);
    _cancelHandle_inlock(_getBaseRollbackIdHandle);
    _cancelHandle_inlock(_getLastRollbackIdHandle);
    _cancelHandle_inlock(_getNextApplierBatchHandle);

    _shutdownComponent_inlock(_oplogFetcher);
    if (_sharedData) {
        // We actually hold the required lock, but the lock object itself is not passed through.
        _clearRetriableError(WithLock::withoutLock());
        stdx::lock_guard<InitialSyncSharedData> lock(*_sharedData);
        _sharedData->setStatusIfOK(
            lock, Status{ErrorCodes::CallbackCanceled, "Initial sync attempt canceled"});
    }
    if (_client) {
        _client->shutdownAndDisallowReconnect();
    }
    _shutdownComponent_inlock(_applier);
    _shutdownComponent_inlock(_fCVFetcher);
    _shutdownComponent_inlock(_lastOplogEntryFetcher);
    _shutdownComponent_inlock(_beginFetchingOpTimeFetcher);
    (*_attemptExec)->shutdown();
    (*_clonerAttemptExec)->shutdown();
    _attemptCanceled = true;
}

void InitialSyncer::join() {
    stdx::unique_lock<Latch> lk(_mutex);
    _stateCondition.wait(lk, [this]() { return !_isActive_inlock(); });
}

InitialSyncer::State InitialSyncer::getState_forTest() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _state;
}

Date_t InitialSyncer::getWallClockTime_forTest() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _lastApplied.wallTime;
}

void InitialSyncer::setAllowedOutageDuration_forTest(Milliseconds allowedOutageDuration) {
    stdx::lock_guard<Latch> lk(_mutex);
    _allowedOutageDuration = allowedOutageDuration;
    if (_sharedData) {
        stdx::lock_guard<InitialSyncSharedData> lk(*_sharedData);
        _sharedData->setAllowedOutageDuration_forTest(lk, allowedOutageDuration);
    }
}

bool InitialSyncer::_isShuttingDown() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _isShuttingDown_inlock();
}

bool InitialSyncer::_isShuttingDown_inlock() const {
    return State::kShuttingDown == _state;
}

std::string InitialSyncer::getDiagnosticString() const {
    LockGuard lk(_mutex);
    str::stream out;
    out << "InitialSyncer -"
        << " oplogFetcher: " << _oplogFetcher->toString()
        << " opsBuffered: " << _oplogBuffer->getSize() << " active: " << _isActive_inlock()
        << " shutting down: " << _isShuttingDown_inlock();
    if (_initialSyncState) {
        out << " opsAppied: " << _initialSyncState->appliedOps;
    }

    return out;
}

BSONObj InitialSyncer::getInitialSyncProgress() const {
    LockGuard lk(_mutex);

    // We return an empty BSON object after an initial sync attempt has been successfully
    // completed. When an initial sync attempt completes successfully, initialSyncCompletes is
    // incremented and then _initialSyncState is cleared. We check that _initialSyncState has been
    // cleared because an initial sync attempt can fail even after initialSyncCompletes is
    // incremented, and we also check that initialSyncCompletes is positive because an initial sync
    // attempt can also fail before _initialSyncState is initialized.
    if (!_initialSyncState && initial_sync_common_stats::initialSyncCompletes.get() > 0) {
        return BSONObj();
    }
    return _getInitialSyncProgress_inlock();
}

void InitialSyncer::_appendInitialSyncProgressMinimal_inlock(BSONObjBuilder* bob) const {
    bob->append("method", "logical");
    _stats.append(bob);
    if (!_initialSyncState) {
        return;
    }
    if (_initialSyncState->allDatabaseCloner) {
        const auto allDbClonerStats = _initialSyncState->allDatabaseCloner->getStats();
        const auto approxTotalDataSize = allDbClonerStats.dataSize;
        bob->appendNumber("approxTotalDataSize", approxTotalDataSize);
        long long approxTotalBytesCopied = 0;
        for (auto&& dbClonerStats : allDbClonerStats.databaseStats) {
            for (auto&& collClonerStats : dbClonerStats.collectionStats) {
                approxTotalBytesCopied += collClonerStats.approxBytesCopied;
            }
        }
        bob->appendNumber("approxTotalBytesCopied", approxTotalBytesCopied);
        if (approxTotalBytesCopied > 0) {
            const auto statsObj = bob->asTempObj();
            auto totalInitialSyncElapsedMillis =
                statsObj.getField("totalInitialSyncElapsedMillis").safeNumberLong();
            const auto downloadRate =
                (double)totalInitialSyncElapsedMillis / (double)approxTotalBytesCopied;
            const auto remainingInitialSyncEstimatedMillis =
                downloadRate * (double)(approxTotalDataSize - approxTotalBytesCopied);
            bob->appendNumber("remainingInitialSyncEstimatedMillis",
                              (long long)remainingInitialSyncEstimatedMillis);
        }
    }
    bob->appendNumber("appliedOps", static_cast<long long>(_initialSyncState->appliedOps));
    if (!_initialSyncState->beginApplyingTimestamp.isNull()) {
        bob->append("initialSyncOplogStart", _initialSyncState->beginApplyingTimestamp);
    }
    // Only include the beginFetchingTimestamp if it's different from the beginApplyingTimestamp.
    if (!_initialSyncState->beginFetchingTimestamp.isNull() &&
        _initialSyncState->beginFetchingTimestamp != _initialSyncState->beginApplyingTimestamp) {
        bob->append("initialSyncOplogFetchingStart", _initialSyncState->beginFetchingTimestamp);
    }
    if (!_initialSyncState->stopTimestamp.isNull()) {
        bob->append("initialSyncOplogEnd", _initialSyncState->stopTimestamp);
    }
    if (_sharedData) {
        stdx::lock_guard<InitialSyncSharedData> sdLock(*_sharedData);
        auto unreachableSince = _sharedData->getSyncSourceUnreachableSince(sdLock);
        if (unreachableSince != Date_t()) {
            bob->append("syncSourceUnreachableSince", unreachableSince);
            bob->append("currentOutageDurationMillis",
                        durationCount<Milliseconds>(_sharedData->getCurrentOutageDuration(sdLock)));
        }
        bob->append("totalTimeUnreachableMillis",
                    durationCount<Milliseconds>(_sharedData->getTotalTimeUnreachable(sdLock)));
    }
}

BSONObj InitialSyncer::_getInitialSyncProgress_inlock() const {
    try {
        BSONObjBuilder bob;
        _appendInitialSyncProgressMinimal_inlock(&bob);
        if (_initialSyncState) {
            if (_initialSyncState->allDatabaseCloner) {
                BSONObjBuilder dbsBuilder(bob.subobjStart("databases"));
                _initialSyncState->allDatabaseCloner->getStats().append(&dbsBuilder);
                dbsBuilder.doneFast();
            }
        }
        return bob.obj();
    } catch (const DBException& e) {
        LOGV2(21161,
              "Error creating initial sync progress object: {error}",
              "Error creating initial sync progress object",
              "error"_attr = e.toString());
    }
    BSONObjBuilder bob;
    _appendInitialSyncProgressMinimal_inlock(&bob);
    return bob.obj();
}

void InitialSyncer::setCreateClientFn_forTest(const CreateClientFn& createClientFn) {
    LockGuard lk(_mutex);
    _createClientFn = createClientFn;
}

void InitialSyncer::setCreateOplogFetcherFn_forTest(
    std::unique_ptr<OplogFetcherFactory> createOplogFetcherFn) {
    LockGuard lk(_mutex);
    _createOplogFetcherFn = std::move(createOplogFetcherFn);
}

OplogFetcher* InitialSyncer::getOplogFetcher_forTest() const {
    // Wait up to 10 seconds.
    for (auto i = 0; i < 100; i++) {
        {
            LockGuard lk(_mutex);
            if (_oplogFetcher) {
                return _oplogFetcher.get();
            }
        }
        sleepmillis(100);
    }
    invariant(false, "Timed out getting OplogFetcher pointer for test");
    return nullptr;
}

void InitialSyncer::setClonerExecutor_forTest(std::shared_ptr<executor::TaskExecutor> clonerExec) {
    _clonerExec = clonerExec;
}

void InitialSyncer::waitForCloner_forTest() {
    _initialSyncState->allDatabaseClonerFuture.wait();
}

void InitialSyncer::_setUp_inlock(OperationContext* opCtx, std::uint32_t initialSyncMaxAttempts) {
    // 'opCtx' is passed through from startup().
    _replicationProcess->getConsistencyMarkers()->setInitialSyncFlag(opCtx);
    _replicationProcess->getConsistencyMarkers()->clearInitialSyncId(opCtx);

    auto serviceCtx = opCtx->getServiceContext();
    _storage->setInitialDataTimestamp(serviceCtx, Timestamp::kAllowUnstableCheckpointsSentinel);
    _storage->setStableTimestamp(serviceCtx, Timestamp::min());

    LOGV2_DEBUG(21162, 1, "Creating oplogBuffer");
    _oplogBuffer = _dataReplicatorExternalState->makeInitialSyncOplogBuffer(opCtx);
    _oplogBuffer->startup(opCtx);

    _stats.initialSyncStart = _exec->now();
    _stats.maxFailedInitialSyncAttempts = initialSyncMaxAttempts;
    _stats.failedInitialSyncAttempts = 0;
    _stats.exec = std::weak_ptr<executor::TaskExecutor>(_exec);

    _allowedOutageDuration = Seconds(initialSyncTransientErrorRetryPeriodSeconds.load());
}

void InitialSyncer::_tearDown_inlock(OperationContext* opCtx,
                                     const StatusWith<OpTimeAndWallTime>& lastApplied) {
    _stats.initialSyncEnd = _exec->now();

    // This might not be necessary if we failed initial sync.
    invariant(_oplogBuffer);
    _oplogBuffer->shutdown(opCtx);

    if (!lastApplied.isOK()) {
        return;
    }
    const auto lastAppliedOpTime = lastApplied.getValue().opTime;
    auto initialDataTimestamp = lastAppliedOpTime.getTimestamp();

    // A node coming out of initial sync must guarantee at least one oplog document is visible
    // such that others can sync from this node. Oplog visibility is only advanced when applying
    // oplog entries during initial sync. Correct the visibility to match the initial sync time
    // before transitioning to steady state replication.
    const bool orderedCommit = true;
    _storage->oplogDiskLocRegister(opCtx, initialDataTimestamp, orderedCommit);

    tenant_migration_access_blocker::recoverTenantMigrationAccessBlockers(opCtx);
    reconstructPreparedTransactions(opCtx, repl::OplogApplication::Mode::kInitialSync);

    _replicationProcess->getConsistencyMarkers()->setInitialSyncIdIfNotSet(opCtx);

    // We set the initial data timestamp before clearing the initial sync flag. See comments in
    // clearInitialSyncFlag.
    _storage->setInitialDataTimestamp(opCtx->getServiceContext(), initialDataTimestamp);

    _replicationProcess->getConsistencyMarkers()->clearInitialSyncFlag(opCtx);

    auto currentLastAppliedOpTime = _opts.getMyLastOptime();
    if (currentLastAppliedOpTime.isNull()) {
        _opts.setMyLastOptime(lastApplied.getValue());
    } else {
        invariant(currentLastAppliedOpTime == lastAppliedOpTime);
    }

    LOGV2(21163,
          "initial sync done; took "
          "{duration}.",
          "Initial sync done",
          "duration"_attr =
              duration_cast<Seconds>(_stats.initialSyncEnd - _stats.initialSyncStart));
    initial_sync_common_stats::initialSyncCompletes.increment();
}

void InitialSyncer::_startInitialSyncAttemptCallback(
    const executor::TaskExecutor::CallbackArgs& callbackArgs,
    std::uint32_t initialSyncAttempt,
    std::uint32_t initialSyncMaxAttempts) noexcept {
    auto status = [&] {
        stdx::lock_guard<Latch> lock(_mutex);
        return _checkForShutdownAndConvertStatus_inlock(
            callbackArgs,
            str::stream() << "error while starting initial sync attempt "
                          << (initialSyncAttempt + 1) << " of " << initialSyncMaxAttempts);
    }();

    if (!status.isOK()) {
        _finishInitialSyncAttempt(status);
        return;
    }

    LOGV2(21164,
          "Starting initial sync (attempt {initialSyncAttempt} of {initialSyncMaxAttempts})",
          "Starting initial sync attempt",
          "initialSyncAttempt"_attr = (initialSyncAttempt + 1),
          "initialSyncMaxAttempts"_attr = initialSyncMaxAttempts);

    // This completion guard invokes _finishInitialSyncAttempt on destruction.
    auto cancelRemainingWorkInLock = [this]() { _cancelRemainingWork_inlock(); };
    auto finishInitialSyncAttemptFn = [this](const StatusWith<OpTimeAndWallTime>& lastApplied) {
        _finishInitialSyncAttempt(lastApplied);
    };
    auto onCompletionGuard =
        std::make_shared<OnCompletionGuard>(cancelRemainingWorkInLock, finishInitialSyncAttemptFn);

    // Lock guard must be declared after completion guard because completion guard destructor
    // has to run outside lock.
    stdx::lock_guard<Latch> lock(_mutex);

    _oplogApplier = {};

    LOGV2_DEBUG(
        21165, 2, "Resetting sync source so a new one can be chosen for this initial sync attempt");
    _syncSource = HostAndPort();

    LOGV2_DEBUG(21166, 2, "Resetting all optimes before starting this initial sync attempt");
    _opts.resetOptimes();
    _lastApplied = {OpTime(), Date_t()};
    _lastFetched = {};

    LOGV2_DEBUG(
        21167, 2, "Resetting the oldest timestamp before starting this initial sync attempt");
    auto storageEngine = getGlobalServiceContext()->getStorageEngine();
    if (storageEngine) {
        // Set the oldestTimestamp to one because WiredTiger does not allow us to set it to zero
        // since that would also set the all_durable point to zero. We specifically don't set
        // the stable timestamp here because that will trigger taking a first stable checkpoint even
        // though the initialDataTimestamp is still set to kAllowUnstableCheckpointsSentinel.
        storageEngine->setOldestTimestamp(kTimestampOne);
    }

    LOGV2_DEBUG(21168,
                2,
                "Resetting feature compatibility version to last-lts. If the sync source is in "
                "latest feature compatibility version, we will find out when we clone the "
                "server configuration collection (admin.system.version)");
    serverGlobalParams.mutableFeatureCompatibility.reset();

    // Clear the oplog buffer.
    _oplogBuffer->clear(makeOpCtx().get());

    // Get sync source.
    std::uint32_t chooseSyncSourceAttempt = 0;
    std::uint32_t chooseSyncSourceMaxAttempts =
        static_cast<std::uint32_t>(numInitialSyncConnectAttempts.load());

    // _scheduleWorkAndSaveHandle_inlock() is shutdown-aware.
    status = _scheduleWorkAndSaveHandle_inlock(
        [=](const executor::TaskExecutor::CallbackArgs& args) {
            _chooseSyncSourceCallback(
                args, chooseSyncSourceAttempt, chooseSyncSourceMaxAttempts, onCompletionGuard);
        },
        &_chooseSyncSourceHandle,
        str::stream() << "_chooseSyncSourceCallback-" << chooseSyncSourceAttempt);
    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }
}

void InitialSyncer::_chooseSyncSourceCallback(
    const executor::TaskExecutor::CallbackArgs& callbackArgs,
    std::uint32_t chooseSyncSourceAttempt,
    std::uint32_t chooseSyncSourceMaxAttempts,
    std::shared_ptr<OnCompletionGuard> onCompletionGuard) noexcept try {
    if (MONGO_unlikely(initialSyncHangBeforeChoosingSyncSource.shouldFail())) {
        LOGV2(5284800, "initialSyncHangBeforeChoosingSyncSource fail point enabled");
        initialSyncHangBeforeChoosingSyncSource.pauseWhileSet();
    }

    stdx::unique_lock<Latch> lock(_mutex);
    // Cancellation should be treated the same as other errors. In this case, the most likely cause
    // of a failed _chooseSyncSourceCallback() task is a cancellation triggered by
    // InitialSyncer::shutdown() or the task executor shutting down.
    auto status =
        _checkForShutdownAndConvertStatus_inlock(callbackArgs, "error while choosing sync source");
    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }

    if (MONGO_unlikely(failInitialSyncWithBadHost.shouldFail())) {
        status = Status(ErrorCodes::InvalidSyncSource,
                        "initial sync failed - failInitialSyncWithBadHost failpoint is set.");
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }

    auto syncSource = _chooseSyncSource_inlock();
    if (!syncSource.isOK()) {
        if (chooseSyncSourceAttempt + 1 >= chooseSyncSourceMaxAttempts) {
            onCompletionGuard->setResultAndCancelRemainingWork_inlock(
                lock,
                Status(ErrorCodes::InitialSyncOplogSourceMissing,
                       "No valid sync source found in current replica set to do an initial sync."));
            return;
        }

        auto when = (*_attemptExec)->now() + _opts.syncSourceRetryWait;
        LOGV2_DEBUG(21169,
                    1,
                    "Error getting sync source: '{error}', trying again in "
                    "{syncSourceRetryWait} at {retryTime}. Attempt {chooseSyncSourceAttempt} of "
                    "{numInitialSyncConnectAttempts}",
                    "Error getting sync source. Waiting to retry",
                    "error"_attr = syncSource.getStatus(),
                    "syncSourceRetryWait"_attr = _opts.syncSourceRetryWait,
                    "retryTime"_attr = when.toString(),
                    "chooseSyncSourceAttempt"_attr = (chooseSyncSourceAttempt + 1),
                    "numInitialSyncConnectAttempts"_attr = numInitialSyncConnectAttempts.load());
        auto status = _scheduleWorkAtAndSaveHandle_inlock(
            when,
            [=](const executor::TaskExecutor::CallbackArgs& args) {
                _chooseSyncSourceCallback(args,
                                          chooseSyncSourceAttempt + 1,
                                          chooseSyncSourceMaxAttempts,
                                          onCompletionGuard);
            },
            &_chooseSyncSourceHandle,
            str::stream() << "_chooseSyncSourceCallback-" << (chooseSyncSourceAttempt + 1));
        if (!status.isOK()) {
            onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
            return;
        }
        return;
    }

    if (MONGO_unlikely(initialSyncHangBeforeCreatingOplog.shouldFail())) {
        // This log output is used in js tests so please leave it.
        LOGV2(21170,
              "initial sync - initialSyncHangBeforeCreatingOplog fail point "
              "enabled. Blocking until fail point is disabled.");
        lock.unlock();
        while (MONGO_unlikely(initialSyncHangBeforeCreatingOplog.shouldFail()) &&
               !_isShuttingDown()) {
            mongo::sleepsecs(1);
        }
        lock.lock();
    }

    // There is no need to schedule separate task to create oplog collection since we are already in
    // a callback and we are certain there's no existing operation context (required for creating
    // collections and dropping user databases) attached to the current thread.
    status = _truncateOplogAndDropReplicatedDatabases();
    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }

    _syncSource = syncSource.getValue();

    // Schedule rollback ID checker.
    _rollbackChecker = std::make_unique<RollbackChecker>(*_attemptExec, _syncSource);
    auto scheduleResult = _rollbackChecker->reset([=](const RollbackChecker::Result& result) {
        return _rollbackCheckerResetCallback(result, onCompletionGuard);
    });
    status = scheduleResult.getStatus();
    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }
    _getBaseRollbackIdHandle = scheduleResult.getValue();
} catch (const DBException&) {
    // Report exception as an initial syncer failure.
    stdx::unique_lock<Latch> lock(_mutex);
    onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, exceptionToStatus());
}

Status InitialSyncer::_truncateOplogAndDropReplicatedDatabases() {
    // truncate oplog; drop user databases.
    LOGV2_DEBUG(4540700,
                1,
                "About to truncate the oplog, if it exists, ns:{namespace}, and drop all "
                "user databases (so that we can clone them).",
                "About to truncate the oplog, if it exists, and drop all user databases (so that "
                "we can clone them)",
                "namespace"_attr = NamespaceString::kRsOplogNamespace);

    auto opCtx = makeOpCtx();
    // This code can make untimestamped writes (deletes) to the _mdb_catalog on top of existing
    // timestamped updates.
    opCtx->recoveryUnit()->allowUntimestampedWrite();

    // We are not replicating nor validating these writes.
    UnreplicatedWritesBlock unreplicatedWritesBlock(opCtx.get());

    // 1.) Truncate the oplog.
    LOGV2_DEBUG(4540701,
                2,
                "Truncating the existing oplog: {namespace}",
                "Truncating the existing oplog",
                "namespace"_attr = NamespaceString::kRsOplogNamespace);
    Timer timer;
    auto status = _storage->truncateCollection(opCtx.get(), NamespaceString::kRsOplogNamespace);
    LOGV2(21173,
          "Initial syncer oplog truncation finished in: {durationMillis}ms",
          "Initial syncer oplog truncation finished",
          "durationMillis"_attr = timer.millis());
    if (!status.isOK()) {
        // 1a.) Create the oplog.
        LOGV2_DEBUG(4540702,
                    2,
                    "Creating the oplog: {namespace}",
                    "Creating the oplog",
                    "namespace"_attr = NamespaceString::kRsOplogNamespace);
        status = _storage->createOplog(opCtx.get(), NamespaceString::kRsOplogNamespace);
        if (!status.isOK()) {
            return status;
        }
    }

    // 2a.) Abort any index builds started during initial sync.
    IndexBuildsCoordinator::get(opCtx.get())
        ->abortAllIndexBuildsForInitialSync(opCtx.get(), "Aborting index builds for initial sync");

    // 2b.) Drop user databases.
    LOGV2_DEBUG(21175, 2, "Dropping user databases");
    return _storage->dropReplicatedDatabases(opCtx.get());
}

void InitialSyncer::_rollbackCheckerResetCallback(
    const RollbackChecker::Result& result, std::shared_ptr<OnCompletionGuard> onCompletionGuard) {
    stdx::lock_guard<Latch> lock(_mutex);
    auto status = _checkForShutdownAndConvertStatus_inlock(result.getStatus(),
                                                           "error while getting base rollback ID");
    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }

    // Since the beginFetchingOpTime is retrieved before significant work is done copying
    // data from the sync source, we allow the OplogEntryFetcher to use its default retry strategy
    // which retries up to 'numInitialSyncOplogFindAttempts' times'.  This will fail relatively
    // quickly in the presence of network errors, allowing us to choose a different sync source.
    status = _scheduleLastOplogEntryFetcher_inlock(
        [=](const StatusWith<mongo::Fetcher::QueryResponse>& response,
            mongo::Fetcher::NextAction*,
            mongo::BSONObjBuilder*) mutable {
            _lastOplogEntryFetcherCallbackForDefaultBeginFetchingOpTime(response,
                                                                        onCompletionGuard);
        },
        kFetcherHandlesRetries);
    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }
}

void InitialSyncer::_lastOplogEntryFetcherCallbackForDefaultBeginFetchingOpTime(
    const StatusWith<Fetcher::QueryResponse>& result,
    std::shared_ptr<OnCompletionGuard> onCompletionGuard) {

    stdx::unique_lock<Latch> lock(_mutex);
    auto status = _checkForShutdownAndConvertStatus_inlock(
        result.getStatus(), "error while getting last oplog entry for begin timestamp");
    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }

    const auto opTimeResult = parseOpTimeAndWallTime(result);
    status = opTimeResult.getStatus();
    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }

    // This is the top of the oplog before we query for the oldest active transaction timestamp. If
    // that query returns that there are no active transactions, we will use this as the
    // beginFetchingTimestamp.
    const auto& defaultBeginFetchingOpTime = opTimeResult.getValue().opTime;

    std::string logMsg = str::stream() << "Initial Syncer got the defaultBeginFetchingTimestamp: "
                                       << defaultBeginFetchingOpTime.toString();
    pauseAtInitialSyncFuzzerSyncronizationPoints(logMsg);
    LOGV2_DEBUG(6608900,
                1,
                "Initial Syncer got the defaultBeginFetchingOpTime",
                "defaultBeginFetchingOpTime"_attr = defaultBeginFetchingOpTime);

    status = _scheduleGetBeginFetchingOpTime_inlock(onCompletionGuard, defaultBeginFetchingOpTime);
    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }
}

Status InitialSyncer::_scheduleGetBeginFetchingOpTime_inlock(
    std::shared_ptr<OnCompletionGuard> onCompletionGuard,
    const OpTime& defaultBeginFetchingOpTime) {

    const auto preparedState = DurableTxnState_serializer(DurableTxnStateEnum::kPrepared);
    const auto inProgressState = DurableTxnState_serializer(DurableTxnStateEnum::kInProgress);

    // Obtain the oldest active transaction timestamp from the remote by querying their transactions
    // table. To prevent oplog holes (primary) or a stale lastAppliedSnapshot (secondary) from
    // causing this query to return an inaccurate timestamp, we specify an afterClusterTime of the
    // defaultBeginFetchingOpTime so that we wait for all previous writes to be visible.
    BSONObjBuilder cmd;
    cmd.append("find", NamespaceString::kSessionTransactionsTableNamespace.coll().toString());
    cmd.append("filter",
               BSON("state" << BSON("$in" << BSON_ARRAY(preparedState << inProgressState))));
    cmd.append("sort", BSON(SessionTxnRecord::kStartOpTimeFieldName << 1));
    cmd.append("readConcern",
               BSON("level"
                    << "local"
                    << "afterClusterTime" << defaultBeginFetchingOpTime.getTimestamp()));
    cmd.append("limit", 1);

    _beginFetchingOpTimeFetcher = std::make_unique<Fetcher>(
        *_attemptExec,
        _syncSource,
        NamespaceString::kSessionTransactionsTableNamespace.db().toString(),
        cmd.obj(),
        [=](const StatusWith<mongo::Fetcher::QueryResponse>& response,
            mongo::Fetcher::NextAction*,
            mongo::BSONObjBuilder*) mutable {
            _getBeginFetchingOpTimeCallback(
                response, onCompletionGuard, defaultBeginFetchingOpTime);
        },
        ReadPreferenceSetting::secondaryPreferredMetadata(),
        RemoteCommandRequest::kNoTimeout /* find network timeout */,
        RemoteCommandRequest::kNoTimeout /* getMore network timeout */,
        RemoteCommandRetryScheduler::makeRetryPolicy<ErrorCategory::RetriableError>(
            numInitialSyncOplogFindAttempts.load(), executor::RemoteCommandRequest::kNoTimeout));
    Status scheduleStatus = _beginFetchingOpTimeFetcher->schedule();
    if (!scheduleStatus.isOK()) {
        _beginFetchingOpTimeFetcher.reset();
    }
    return scheduleStatus;
}

void InitialSyncer::_getBeginFetchingOpTimeCallback(
    const StatusWith<Fetcher::QueryResponse>& result,
    std::shared_ptr<OnCompletionGuard> onCompletionGuard,
    const OpTime& defaultBeginFetchingOpTime) {
    stdx::unique_lock<Latch> lock(_mutex);
    auto status = _checkForShutdownAndConvertStatus_inlock(
        result.getStatus(),
        "error while getting oldest active transaction timestamp for begin fetching timestamp");
    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }

    const auto docs = result.getValue().documents;
    if (docs.size() > 1) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(
            lock,
            Status(ErrorCodes::TooManyMatchingDocuments,
                   str::stream() << "Expected to receive one document for the oldest active "
                                    "transaction entry, but received: "
                                 << docs.size() << ". First: " << redact(docs.front())
                                 << ". Last: " << redact(docs.back())));
        return;
    }

    // Set beginFetchingOpTime if the oldest active transaction timestamp actually exists. Otherwise
    // use the sync source's top of the oplog from before querying for the oldest active transaction
    // timestamp. This will mean that even if a transaction is started on the sync source after
    // querying for the oldest active transaction timestamp, the node will still fetch its oplog
    // entries.
    OpTime beginFetchingOpTime = defaultBeginFetchingOpTime;
    if (docs.size() != 0) {
        auto entry = SessionTxnRecord::parse(
            IDLParserContext("oldest active transaction optime for initial sync"), docs.front());
        auto optime = entry.getStartOpTime();
        if (optime) {
            beginFetchingOpTime = optime.value();
        }
    }

    std::string logMsg = str::stream()
        << "Initial Syncer got the beginFetchingTimestamp: " << beginFetchingOpTime.toString();
    pauseAtInitialSyncFuzzerSyncronizationPoints(logMsg);

    if (MONGO_unlikely(initialSyncHangAfterGettingBeginFetchingTimestamp.shouldFail())) {
        LOGV2(21176, "initialSyncHangAfterGettingBeginFetchingTimestamp fail point enabled");
        initialSyncHangAfterGettingBeginFetchingTimestamp.pauseWhileSet();
    }

    // Since the beginFetchingOpTime is retrieved before significant work is done copying
    // data from the sync source, we allow the OplogEntryFetcher to use its default retry strategy
    // which retries up to 'numInitialSyncOplogFindAttempts' times'.  This will fail relatively
    // quickly in the presence of network errors, allowing us to choose a different sync source.
    status = _scheduleLastOplogEntryFetcher_inlock(
        [=](const StatusWith<mongo::Fetcher::QueryResponse>& response,
            mongo::Fetcher::NextAction*,
            mongo::BSONObjBuilder*) mutable {
            _lastOplogEntryFetcherCallbackForBeginApplyingTimestamp(
                response, onCompletionGuard, beginFetchingOpTime);
        },
        kFetcherHandlesRetries);
    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }
}

void InitialSyncer::_lastOplogEntryFetcherCallbackForBeginApplyingTimestamp(
    const StatusWith<Fetcher::QueryResponse>& result,
    std::shared_ptr<OnCompletionGuard> onCompletionGuard,
    OpTime& beginFetchingOpTime) {
    stdx::unique_lock<Latch> lock(_mutex);
    auto status = _checkForShutdownAndConvertStatus_inlock(
        result.getStatus(), "error while getting last oplog entry for begin timestamp");
    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }

    const auto opTimeResult = parseOpTimeAndWallTime(result);
    status = opTimeResult.getStatus();
    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }

    const auto& lastOpTime = opTimeResult.getValue().opTime;

    std::string logMsg = str::stream()
        << "Initial Syncer got the beginApplyingTimestamp: " << lastOpTime.toString();
    pauseAtInitialSyncFuzzerSyncronizationPoints(logMsg);

    BSONObjBuilder queryBob;
    queryBob.append("find", NamespaceString::kServerConfigurationNamespace.coll());
    auto filterBob = BSONObjBuilder(queryBob.subobjStart("filter"));
    filterBob.append("_id", multiversion::kParameterName);
    filterBob.done();
    // As part of reading the FCV, we ensure the source node's all_durable timestamp has advanced
    // to at least the timestamp of the last optime that we found in the lastOplogEntryFetcher.
    // When document locking is used, there could be oplog "holes" which would result in
    // inconsistent initial sync data if we didn't do this.
    auto readConcernBob = BSONObjBuilder(queryBob.subobjStart("readConcern"));
    readConcernBob.append("afterClusterTime", lastOpTime.getTimestamp());
    readConcernBob.done();

    _fCVFetcher = std::make_unique<Fetcher>(
        *_attemptExec,
        _syncSource,
        NamespaceString::kServerConfigurationNamespace.db().toString(),
        queryBob.obj(),
        [=](const StatusWith<mongo::Fetcher::QueryResponse>& response,
            mongo::Fetcher::NextAction*,
            mongo::BSONObjBuilder*) mutable {
            _fcvFetcherCallback(response, onCompletionGuard, lastOpTime, beginFetchingOpTime);
        },
        ReadPreferenceSetting::secondaryPreferredMetadata(),
        RemoteCommandRequest::kNoTimeout /* find network timeout */,
        RemoteCommandRequest::kNoTimeout /* getMore network timeout */,
        RemoteCommandRetryScheduler::makeRetryPolicy<ErrorCategory::RetriableError>(
            numInitialSyncOplogFindAttempts.load(), executor::RemoteCommandRequest::kNoTimeout));
    Status scheduleStatus = _fCVFetcher->schedule();
    if (!scheduleStatus.isOK()) {
        _fCVFetcher.reset();
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, scheduleStatus);
        return;
    }
}

void InitialSyncer::_fcvFetcherCallback(const StatusWith<Fetcher::QueryResponse>& result,
                                        std::shared_ptr<OnCompletionGuard> onCompletionGuard,
                                        const OpTime& lastOpTime,
                                        OpTime& beginFetchingOpTime) {
    stdx::unique_lock<Latch> lock(_mutex);
    auto status = _checkForShutdownAndConvertStatus_inlock(
        result.getStatus(), "error while getting the remote feature compatibility version");
    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }

    const auto docs = result.getValue().documents;
    if (docs.size() > 1) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(
            lock,
            Status(ErrorCodes::TooManyMatchingDocuments,
                   str::stream() << "Expected to receive one feature compatibility version "
                                    "document, but received: "
                                 << docs.size() << ". First: " << redact(docs.front())
                                 << ". Last: " << redact(docs.back())));
        return;
    }
    const auto hasDoc = docs.begin() != docs.end();
    if (!hasDoc) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(
            lock,
            Status(ErrorCodes::IncompatibleServerVersion,
                   "Sync source had no feature compatibility version document"));
        return;
    }

    auto fCVParseSW = FeatureCompatibilityVersionParser::parse(docs.front());
    if (!fCVParseSW.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, fCVParseSW.getStatus());
        return;
    }

    auto version = fCVParseSW.getValue();

    // Changing the featureCompatibilityVersion during initial sync is unsafe.
    // (Generic FCV reference): This FCV check should exist across LTS binary versions.
    if (serverGlobalParams.featureCompatibility.isUpgradingOrDowngrading(version)) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(
            lock,
            Status(ErrorCodes::IncompatibleServerVersion,
                   str::stream() << "Sync source had unsafe feature compatibility version: "
                                 << multiversion::toString(version)));
        return;
    } else {
        // Since we don't guarantee that we always clone the "admin.system.version" collection first
        // and collection/index creation can depend on FCV, we set the in-memory FCV value to match
        // the version on the sync source. We won't persist the FCV on disk nor will we update our
        // minWireVersion until we clone the actual document.
        serverGlobalParams.mutableFeatureCompatibility.setVersion(version);
    }

    if (MONGO_unlikely(initialSyncHangBeforeSplittingControlFlow.shouldFail())) {
        lock.unlock();
        LOGV2(5032000,
              "initial sync - initialSyncHangBeforeSplittingControlFlow fail point "
              "enabled. Blocking until fail point is disabled.");
        while (MONGO_unlikely(initialSyncHangBeforeSplittingControlFlow.shouldFail()) &&
               !_isShuttingDown()) {
            mongo::sleepsecs(1);
        }
        lock.lock();
    }

    // This is where the flow of control starts to split into two parallel tracks:
    // - oplog fetcher
    // - data cloning and applier
    _sharedData =
        std::make_unique<InitialSyncSharedData>(_rollbackChecker->getBaseRBID(),
                                                _allowedOutageDuration,
                                                getGlobalServiceContext()->getFastClockSource());
    _client = _createClientFn();
    _initialSyncState = std::make_unique<InitialSyncState>(std::make_unique<AllDatabaseCloner>(
        _sharedData.get(), _syncSource, _client.get(), _storage, _writerPool));

    // Create oplog applier.
    auto consistencyMarkers = _replicationProcess->getConsistencyMarkers();
    OplogApplier::Options options(OplogApplication::Mode::kInitialSync);
    options.beginApplyingOpTime = lastOpTime;
    _oplogApplier = _dataReplicatorExternalState->makeOplogApplier(_oplogBuffer.get(),
                                                                   &noopOplogApplierObserver,
                                                                   consistencyMarkers,
                                                                   _storage,
                                                                   options,
                                                                   _writerPool);

    _initialSyncState->beginApplyingTimestamp = lastOpTime.getTimestamp();
    _initialSyncState->beginFetchingTimestamp = beginFetchingOpTime.getTimestamp();

    invariant(_initialSyncState->beginApplyingTimestamp >=
                  _initialSyncState->beginFetchingTimestamp,
              str::stream() << "beginApplyingTimestamp was less than beginFetchingTimestamp. "
                               "beginApplyingTimestamp: "
                            << _initialSyncState->beginApplyingTimestamp.toBSON()
                            << " beginFetchingTimestamp: "
                            << _initialSyncState->beginFetchingTimestamp.toBSON());

    invariant(!result.getValue().documents.empty());
    LOGV2_DEBUG(4431600,
                2,
                "Setting begin applying timestamp to {beginApplyingTimestamp}, ns: "
                "{namespace} and the begin fetching timestamp to {beginFetchingTimestamp}",
                "Setting begin applying timestamp and begin fetching timestamp",
                "beginApplyingTimestamp"_attr = _initialSyncState->beginApplyingTimestamp,
                "namespace"_attr = NamespaceString::kRsOplogNamespace,
                "beginFetchingTimestamp"_attr = _initialSyncState->beginFetchingTimestamp);

    const auto configResult = _dataReplicatorExternalState->getCurrentConfig();
    status = configResult.getStatus();
    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        _initialSyncState.reset();
        return;
    }

    const auto& config = configResult.getValue();
    OplogFetcher::Config oplogFetcherConfig(
        beginFetchingOpTime,
        _syncSource,
        config,
        _rollbackChecker->getBaseRBID(),
        initialSyncOplogFetcherBatchSize,
        OplogFetcher::RequireFresherSyncSource::kDontRequireFresherSyncSource);
    oplogFetcherConfig.startingPoint = OplogFetcher::StartingPoint::kEnqueueFirstDoc;
    _oplogFetcher = (*_createOplogFetcherFn)(
        *_attemptExec,
        std::make_unique<OplogFetcherRestartDecisionInitialSyncer>(
            _sharedData.get(), _opts.oplogFetcherMaxFetcherRestarts),
        _dataReplicatorExternalState.get(),
        [=](OplogFetcher::Documents::const_iterator first,
            OplogFetcher::Documents::const_iterator last,
            const OplogFetcher::DocumentsInfo& info) {
            return _enqueueDocuments(first, last, info);
        },
        [=](const Status& s, int rbid) { _oplogFetcherCallback(s, onCompletionGuard); },
        std::move(oplogFetcherConfig));

    LOGV2_DEBUG(21178,
                2,
                "Starting OplogFetcher: {oplogFetcher}",
                "Starting OplogFetcher",
                "oplogFetcher"_attr = _oplogFetcher->toString());

    // _startupComponent_inlock is shutdown-aware.
    status = _startupComponent_inlock(_oplogFetcher);
    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        _initialSyncState->allDatabaseCloner.reset();
        return;
    }

    if (MONGO_unlikely(initialSyncHangBeforeCopyingDatabases.shouldFail())) {
        lock.unlock();
        // This could have been done with a scheduleWorkAt but this is used only by JS tests where
        // we run with multiple threads so it's fine to spin on this thread.
        // This log output is used in js tests so please leave it.
        LOGV2(21179,
              "initial sync - initialSyncHangBeforeCopyingDatabases fail point "
              "enabled. Blocking until fail point is disabled.");
        while (MONGO_unlikely(initialSyncHangBeforeCopyingDatabases.shouldFail()) &&
               !_isShuttingDown()) {
            mongo::sleepsecs(1);
        }
        lock.lock();
    }

    LOGV2_DEBUG(21180,
                2,
                "Starting AllDatabaseCloner: {allDatabaseCloner}",
                "Starting AllDatabaseCloner",
                "allDatabaseCloner"_attr = _initialSyncState->allDatabaseCloner->toString());

    auto [startClonerFuture, startCloner] =
        _initialSyncState->allDatabaseCloner->runOnExecutorEvent(*_clonerAttemptExec);
    // runOnExecutorEvent ensures the future is not ready unless an error has occurred.
    if (startClonerFuture.isReady()) {
        status = startClonerFuture.getNoThrow();
        invariant(!status.isOK());
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }
    _initialSyncState->allDatabaseClonerFuture =
        std::move(startClonerFuture).onCompletion([this, onCompletionGuard](Status status) mutable {
            // The completion guard must run on the main executor, and never inline.  In unit tests,
            // without the executor call, it would run on the wrong executor.  In both production
            // and in unit tests, if the cloner finishes very quickly, the callback could run
            // in-line and result in self-deadlock.
            stdx::unique_lock<Latch> lock(_mutex);
            auto exec_status = (*_attemptExec)
                                   ->scheduleWork([this, status, onCompletionGuard](
                                                      executor::TaskExecutor::CallbackArgs args) {
                                       _allDatabaseClonerCallback(status, onCompletionGuard);
                                   });
            if (!exec_status.isOK()) {
                onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock,
                                                                          exec_status.getStatus());
                // In the shutdown case, it is possible the completion guard will be run
                // from this thread (since the lambda holding another copy didn't schedule).
                // If it does, we will self-deadlock if we're holding the lock, so release it.
                lock.unlock();
            }
            // In unit tests, this reset ensures the completion guard does not run during the
            // destruction of the lambda (which occurs on the wrong executor), except in the
            // shutdown case.
            onCompletionGuard.reset();
        });
    lock.unlock();
    // Start (and therefore finish) the cloners outside the lock.  This ensures onCompletion
    // is not run with the mutex held, which would result in self-deadlock.
    (*_clonerAttemptExec)->signalEvent(startCloner);
}

void InitialSyncer::_oplogFetcherCallback(const Status& oplogFetcherFinishStatus,
                                          std::shared_ptr<OnCompletionGuard> onCompletionGuard) {
    stdx::lock_guard<Latch> lock(_mutex);
    LOGV2(21181,
          "Finished fetching oplog during initial sync: {oplogFetcherFinishStatus}. Last fetched "
          "optime: {lastFetched}",
          "Finished fetching oplog during initial sync",
          "oplogFetcherFinishStatus"_attr = redact(oplogFetcherFinishStatus),
          "lastFetched"_attr = _lastFetched.toString());

    auto status = _checkForShutdownAndConvertStatus_inlock(
        oplogFetcherFinishStatus, "error fetching oplog during initial sync");

    // When the OplogFetcher completes early (instead of being canceled at shutdown), we log and let
    // our reference to 'onCompletionGuard' go out of scope. Since we know the
    // DatabasesCloner/MultiApplier will still have a reference to it, the actual function within
    // the guard won't be fired yet.
    // It is up to the DatabasesCloner and MultiApplier to determine if they can proceed without any
    // additional data going into the oplog buffer.
    // It is not common for the OplogFetcher to return with an OK status. The only time it returns
    // an OK status is when the 'stopReplProducer' fail point is enabled, which causes the
    // OplogFetcher to ignore the current sync source response and return early.
    if (status.isOK()) {
        LOGV2(21182,
              "Finished fetching oplog fetching early. Last fetched optime: {lastFetched}",
              "Finished fetching oplog fetching early",
              "lastFetched"_attr = _lastFetched.toString());
        return;
    }

    // During normal operation, this call to onCompletion->setResultAndCancelRemainingWork_inlock
    // is a no-op because the other thread running the DatabasesCloner or MultiApplier will already
    // have called it with the success/failed status.
    // The OplogFetcher does not finish on its own because of the oplog tailing query it runs on the
    // sync source. The most common OplogFetcher completion status is CallbackCanceled due to either
    // a shutdown request or completion of the data cloning and oplog application phases.
    onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
}

void InitialSyncer::_allDatabaseClonerCallback(
    const Status& databaseClonerFinishStatus,
    std::shared_ptr<OnCompletionGuard> onCompletionGuard) {
    LOGV2(21183,
          "Finished cloning data: {databaseClonerFinishStatus}. Beginning oplog replay.",
          "Finished cloning data. Beginning oplog replay",
          "databaseClonerFinishStatus"_attr = redact(databaseClonerFinishStatus));
    _client->shutdownAndDisallowReconnect();

    if (MONGO_unlikely(initialSyncHangAfterDataCloning.shouldFail())) {
        // This could have been done with a scheduleWorkAt but this is used only by JS tests where
        // we run with multiple threads so it's fine to spin on this thread.
        // This log output is used in js tests so please leave it.
        LOGV2(21184,
              "initial sync - initialSyncHangAfterDataCloning fail point "
              "enabled. Blocking until fail point is disabled.");
        while (MONGO_unlikely(initialSyncHangAfterDataCloning.shouldFail()) && !_isShuttingDown()) {
            mongo::sleepsecs(1);
        }
    }

    stdx::lock_guard<Latch> lock(_mutex);
    _client.reset();
    auto status = _checkForShutdownAndConvertStatus_inlock(databaseClonerFinishStatus,
                                                           "error cloning databases");
    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }

    // Since the stopTimestamp is retrieved after we have done all the work of retrieving collection
    // data, we handle retries within this class by retrying for
    // 'initialSyncTransientErrorRetryPeriodSeconds' (default 24 hours).  This is the same retry
    // strategy used when retrieving collection data, and avoids retrieving all the data and then
    // throwing it away due to a transient network outage.
    status = _scheduleLastOplogEntryFetcher_inlock(
        [=](const StatusWith<mongo::Fetcher::QueryResponse>& status,
            mongo::Fetcher::NextAction*,
            mongo::BSONObjBuilder*) {
            _lastOplogEntryFetcherCallbackForStopTimestamp(status, onCompletionGuard);
        },
        kInitialSyncerHandlesRetries);
    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }
}

void InitialSyncer::_lastOplogEntryFetcherCallbackForStopTimestamp(
    const StatusWith<Fetcher::QueryResponse>& result,
    std::shared_ptr<OnCompletionGuard> onCompletionGuard) {
    OpTimeAndWallTime resultOpTimeAndWallTime = {OpTime(), Date_t()};
    {
        stdx::lock_guard<Latch> lock(_mutex);
        auto status = _checkForShutdownAndConvertStatus_inlock(
            result.getStatus(), "error fetching last oplog entry for stop timestamp");
        if (_shouldRetryError(lock, status)) {
            auto scheduleStatus =
                (*_attemptExec)
                    ->scheduleWork([this,
                                    onCompletionGuard](executor::TaskExecutor::CallbackArgs args) {
                        // It is not valid to schedule the retry from within this callback,
                        // hence we schedule a lambda to schedule the retry.
                        stdx::lock_guard<Latch> lock(_mutex);
                        // Since the stopTimestamp is retrieved after we have done all the work of
                        // retrieving collection data, we handle retries within this class by
                        // retrying for 'initialSyncTransientErrorRetryPeriodSeconds' (default 24
                        // hours).  This is the same retry strategy used when retrieving collection
                        // data, and avoids retrieving all the data and then throwing it away due to
                        // a transient network outage.
                        auto status = _scheduleLastOplogEntryFetcher_inlock(
                            [=](const StatusWith<mongo::Fetcher::QueryResponse>& status,
                                mongo::Fetcher::NextAction*,
                                mongo::BSONObjBuilder*) {
                                _lastOplogEntryFetcherCallbackForStopTimestamp(status,
                                                                               onCompletionGuard);
                            },
                            kInitialSyncerHandlesRetries);
                        if (!status.isOK()) {
                            onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
                        }
                    });
            if (scheduleStatus.isOK())
                return;
            // If scheduling failed, we're shutting down and cannot retry.
            // So just continue with the original failed status.
        }
        if (!status.isOK()) {
            onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
            return;
        }

        auto&& optimeStatus = parseOpTimeAndWallTime(result);
        if (!optimeStatus.isOK()) {
            onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock,
                                                                      optimeStatus.getStatus());
            return;
        }
        resultOpTimeAndWallTime = optimeStatus.getValue();

        _initialSyncState->stopTimestamp = resultOpTimeAndWallTime.opTime.getTimestamp();

        // If the beginFetchingTimestamp is different from the stopTimestamp, it indicates that
        // there are oplog entries fetched by the oplog fetcher that need to be written to the oplog
        // and/or there are operations that need to be applied.
        if (_initialSyncState->beginFetchingTimestamp != _initialSyncState->stopTimestamp) {
            invariant(_lastApplied.opTime.isNull());
            _checkApplierProgressAndScheduleGetNextApplierBatch_inlock(lock, onCompletionGuard);
            return;
        }
    }

    // Oplog at sync source has not advanced since we started cloning databases, so we use the last
    // oplog entry to seed the oplog before checking the rollback ID.
    {
        const auto& documents = result.getValue().documents;
        invariant(!documents.empty());
        const BSONObj oplogSeedDoc = documents.front();
        LOGV2_DEBUG(21185,
                    2,
                    "Inserting oplog seed document: {oplogSeedDocument}",
                    "Inserting oplog seed document",
                    "oplogSeedDocument"_attr = oplogSeedDoc);

        auto opCtx = makeOpCtx();
        // StorageInterface::insertDocument() has to be called outside the lock because we may
        // override its behavior in tests. See InitialSyncerReturnsCallbackCanceledAndDoesNot-
        // ScheduleRollbackCheckerIfShutdownAfterInsertingInsertOplogSeedDocument in
        // initial_syncer_test.cpp
        //
        // Note that the initial seed oplog insertion is not timestamped, this is safe to do as the
        // logic for navigating the oplog is reliant on the timestamp value of the oplog document
        // itself. Additionally, this also prevents confusion in the storage engine as the last
        // insertion can be produced at precisely the stable timestamp, which could lead to invalid
        // data consistency due to the stable timestamp signalling that no operations before or at
        // that point will be rolled back. So transactions shouldn't happen at precisely that point.
        auto status = _storage->insertDocument(opCtx.get(),
                                               NamespaceString::kRsOplogNamespace,
                                               TimestampedBSONObj{oplogSeedDoc},
                                               resultOpTimeAndWallTime.opTime.getTerm());
        if (!status.isOK()) {
            stdx::lock_guard<Latch> lock(_mutex);
            onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
            return;
        }
        const bool orderedCommit = true;
        _storage->oplogDiskLocRegister(
            opCtx.get(), resultOpTimeAndWallTime.opTime.getTimestamp(), orderedCommit);
    }

    stdx::lock_guard<Latch> lock(_mutex);
    _lastApplied = resultOpTimeAndWallTime;
    LOGV2(21186,
          "No need to apply operations. (currently at {stopTimestamp})",
          "No need to apply operations",
          "stopTimestamp"_attr = _initialSyncState->stopTimestamp.toBSON());

    // This sets the error in 'onCompletionGuard' and shuts down the OplogFetcher on error.
    _scheduleRollbackCheckerCheckForRollback_inlock(lock, onCompletionGuard);
}

void InitialSyncer::_getNextApplierBatchCallback(
    const executor::TaskExecutor::CallbackArgs& callbackArgs,
    std::shared_ptr<OnCompletionGuard> onCompletionGuard) noexcept try {
    stdx::lock_guard<Latch> lock(_mutex);
    auto status =
        _checkForShutdownAndConvertStatus_inlock(callbackArgs, "error getting next applier batch");
    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }

    auto batchResult = _getNextApplierBatch_inlock();
    if (!batchResult.isOK()) {
        LOGV2_WARNING(21196,
                      "Failure creating next apply batch: {error}",
                      "Failure creating next apply batch",
                      "error"_attr = redact(batchResult.getStatus()));
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, batchResult.getStatus());
        return;
    }

    std::string logMsg = str::stream()
        << "Initial Syncer is about to apply the next oplog batch of size: "
        << batchResult.getValue().size();
    pauseAtInitialSyncFuzzerSyncronizationPoints(logMsg);

    if (MONGO_unlikely(failInitialSyncBeforeApplyingBatch.shouldFail())) {
        LOGV2(21187,
              "initial sync - failInitialSyncBeforeApplyingBatch fail point enabled. Pausing until "
              "fail point is disabled, then will fail initial sync");
        failInitialSyncBeforeApplyingBatch.pauseWhileSet();
        status = Status(ErrorCodes::CallbackCanceled,
                        "failInitialSyncBeforeApplyingBatch fail point enabled");
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }

    // Schedule MultiApplier if we have operations to apply.
    const auto& ops = batchResult.getValue();
    if (!ops.empty()) {
        _fetchCount.store(0);
        MultiApplier::MultiApplyFn applyBatchOfOperationsFn = [this](OperationContext* opCtx,
                                                                     std::vector<OplogEntry> ops) {
            return _oplogApplier->applyOplogBatch(opCtx, std::move(ops));
        };
        OpTime lastApplied = ops.back().getOpTime();
        Date_t lastAppliedWall = ops.back().getWallClockTime();

        auto numApplied = ops.size();
        MultiApplier::CallbackFn onCompletionFn = [=](const Status& s) {
            return _multiApplierCallback(
                s, {lastApplied, lastAppliedWall}, numApplied, onCompletionGuard);
        };

        _applier = std::make_unique<MultiApplier>(
            *_attemptExec, ops, std::move(applyBatchOfOperationsFn), std::move(onCompletionFn));
        status = _startupComponent_inlock(_applier);
        if (!status.isOK()) {
            onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
            return;
        }
        return;
    }

    // If the oplog fetcher is no longer running (completed successfully) and the oplog buffer is
    // empty, we are not going to make any more progress with this initial sync. Report progress so
    // far and return a RemoteResultsUnavailable error.
    if (!_oplogFetcher->isActive()) {
        static constexpr char msg[] =
            "The oplog fetcher is no longer running and we have applied all the oplog entries "
            "in the oplog buffer. Aborting this initial sync attempt";
        LOGV2(21188,
              msg,
              "lastApplied"_attr = _lastApplied.opTime,
              "lastFetched"_attr = _lastFetched,
              "operationsApplied"_attr = _initialSyncState->appliedOps);
        status = Status(ErrorCodes::RemoteResultsUnavailable,
                        str::stream()
                            << msg << ". Last applied: " << _lastApplied.opTime.toString()
                            << ". Last fetched: " << _lastFetched.toString()
                            << ". Number of operations applied: " << _initialSyncState->appliedOps);
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }

    // If there are no operations at the moment to apply and the oplog fetcher is still waiting on
    // the sync source, we'll check the oplog buffer again in
    // '_opts.getApplierBatchCallbackRetryWait' ms.
    auto when = (*_attemptExec)->now() + _opts.getApplierBatchCallbackRetryWait;
    status = _scheduleWorkAtAndSaveHandle_inlock(
        when,
        [=](const CallbackArgs& args) { _getNextApplierBatchCallback(args, onCompletionGuard); },
        &_getNextApplierBatchHandle,
        "_getNextApplierBatchCallback");
    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }
} catch (const DBException&) {
    // Report exception as an initial syncer failure.
    stdx::unique_lock<Latch> lock(_mutex);
    onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, exceptionToStatus());
}

void InitialSyncer::_multiApplierCallback(const Status& multiApplierStatus,
                                          OpTimeAndWallTime lastApplied,
                                          std::uint32_t numApplied,
                                          std::shared_ptr<OnCompletionGuard> onCompletionGuard) {
    stdx::lock_guard<Latch> lock(_mutex);
    auto status =
        _checkForShutdownAndConvertStatus_inlock(multiApplierStatus, "error applying batch");

    // Set to cause initial sync to fassert instead of restart if applying a batch fails, so that
    // tests can be robust to network errors but not oplog idempotency errors.
    if (MONGO_unlikely(initialSyncFassertIfApplyingBatchFails.shouldFail())) {
        LOGV2(21189, "initialSyncFassertIfApplyingBatchFails fail point enabled");
        fassert(31210, status);
    }

    if (!status.isOK()) {
        LOGV2_ERROR(21199,
                    "Failed to apply batch due to '{error}'",
                    "Failed to apply batch",
                    "error"_attr = redact(status));
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }

    _initialSyncState->appliedOps += numApplied;
    _lastApplied = lastApplied;
    const auto lastAppliedOpTime = _lastApplied.opTime;
    _opts.setMyLastOptime(_lastApplied);

    // Update oplog visibility after applying a batch so that while applying transaction oplog
    // entries, the TransactionHistoryIterator can get earlier oplog entries associated with the
    // transaction. Note that setting the oplog visibility timestamp here will be safe even if
    // initial sync was restarted because until initial sync ends, no one else will try to read our
    // oplog. It is also safe even if we tried to read from our own oplog because we never try to
    // read from the oplog before applying at least one batch and therefore setting a value for the
    // oplog visibility timestamp.
    auto opCtx = makeOpCtx();
    const bool orderedCommit = true;
    _storage->oplogDiskLocRegister(opCtx.get(), lastAppliedOpTime.getTimestamp(), orderedCommit);
    _checkApplierProgressAndScheduleGetNextApplierBatch_inlock(lock, onCompletionGuard);
}

void InitialSyncer::_rollbackCheckerCheckForRollbackCallback(
    const RollbackChecker::Result& result, std::shared_ptr<OnCompletionGuard> onCompletionGuard) {
    stdx::lock_guard<Latch> lock(_mutex);
    auto status = _checkForShutdownAndConvertStatus_inlock(result.getStatus(),
                                                           "error while getting last rollback ID");
    if (_shouldRetryError(lock, status)) {
        LOGV2_DEBUG(21190,
                    1,
                    "Retrying rollback checker because of network error {error}",
                    "Retrying rollback checker because of network error",
                    "error"_attr = status);
        _scheduleRollbackCheckerCheckForRollback_inlock(lock, onCompletionGuard);
        return;
    }

    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }

    auto hasHadRollback = result.getValue();
    if (hasHadRollback) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(
            lock,
            Status(ErrorCodes::UnrecoverableRollbackError,
                   str::stream() << "Rollback occurred on our sync source " << _syncSource
                                 << " during initial sync"));
        return;
    }

    if (MONGO_unlikely(initialSyncHangBeforeCompletingOplogFetching.shouldFail())) {
        LOGV2(4599500, "initialSyncHangBeforeCompletingOplogFetching fail point enabled");
        initialSyncHangBeforeCompletingOplogFetching.pauseWhileSet();
    }

    // Success!
    onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, _lastApplied);
}

void InitialSyncer::_finishInitialSyncAttempt(const StatusWith<OpTimeAndWallTime>& lastApplied) {
    // Since _finishInitialSyncAttempt can be called from any component's callback function or
    // scheduled task, it is possible that we may not be in a TaskExecutor-managed thread when this
    // function is invoked.
    // For example, if CollectionCloner fails while inserting documents into the
    // CollectionBulkLoader, we will get here via one of CollectionCloner's TaskRunner callbacks
    // which has an active OperationContext bound to the current Client. This would lead to an
    // invariant when we attempt to create a new OperationContext for _tearDown(opCtx).
    // To avoid this, we schedule _finishCallback against the TaskExecutor rather than calling it
    // here synchronously.

    // Unless dismissed, a scope guard will schedule _finishCallback() upon exiting this function.
    // Since it is a requirement that _finishCallback be called outside the lock (which is possible
    // if the task scheduling fails and we have to invoke _finishCallback() synchronously), we
    // declare the scope guard before the lock guard.
    auto result = lastApplied;
    ScopeGuard finishCallbackGuard([this, &result] {
        auto scheduleResult = _exec->scheduleWork(
            [=](const mongo::executor::TaskExecutor::CallbackArgs&) { _finishCallback(result); });
        if (!scheduleResult.isOK()) {
            LOGV2_WARNING(21197,
                          "Unable to schedule initial syncer completion task due to "
                          "{error}. Running callback on current thread.",
                          "Unable to schedule initial syncer completion task. Running callback on "
                          "current thread",
                          "error"_attr = redact(scheduleResult.getStatus()));
            _finishCallback(result);
        }
    });

    LOGV2(21191, "Initial sync attempt finishing up");

    stdx::lock_guard<Latch> lock(_mutex);

    auto runTime = _initialSyncState ? _initialSyncState->timer.millis() : 0;
    int rollBackId = -1;
    int operationsRetried = 0;
    int totalTimeUnreachableMillis = 0;
    if (_sharedData) {
        stdx::lock_guard<InitialSyncSharedData> sdLock(*_sharedData);
        rollBackId = _sharedData->getRollBackId();
        operationsRetried = _sharedData->getTotalRetries(sdLock);
        totalTimeUnreachableMillis =
            durationCount<Milliseconds>(_sharedData->getTotalTimeUnreachable(sdLock));
    }

    if (MONGO_unlikely(failAndHangInitialSync.shouldFail())) {
        LOGV2(21193, "failAndHangInitialSync fail point enabled");
        failAndHangInitialSync.pauseWhileSet();
        result = Status(ErrorCodes::InternalError, "failAndHangInitialSync fail point enabled");
    }

    _stats.initialSyncAttemptInfos.emplace_back(
        InitialSyncer::InitialSyncAttemptInfo{runTime,
                                              result.getStatus(),
                                              _syncSource,
                                              rollBackId,
                                              operationsRetried,
                                              totalTimeUnreachableMillis});

    if (!result.isOK()) {
        // This increments the number of failed attempts for the current initial sync request.
        ++_stats.failedInitialSyncAttempts;
        // This increments the number of failed attempts across all initial sync attempts since
        // process startup.
        initial_sync_common_stats::initialSyncFailedAttempts.increment();
    }

    bool hasRetries = _stats.failedInitialSyncAttempts < _stats.maxFailedInitialSyncAttempts;

    initial_sync_common_stats::LogInitialSyncAttemptStats(
        result, hasRetries, _getInitialSyncProgress_inlock());

    if (result.isOK()) {
        // Scope guard will invoke _finishCallback().
        return;
    }

    LOGV2_ERROR(21200,
                "Initial sync attempt failed -- attempts left: "
                "{attemptsLeft} cause: "
                "{error}",
                "Initial sync attempt failed",
                "attemptsLeft"_attr =
                    (_stats.maxFailedInitialSyncAttempts - _stats.failedInitialSyncAttempts),
                "error"_attr = redact(result.getStatus()));

    // Check if need to do more retries.
    if (!hasRetries) {
        LOGV2_FATAL_CONTINUE(21202,
                             "The maximum number of retries have been exhausted for initial sync");

        initial_sync_common_stats::initialSyncFailures.increment();

        // Scope guard will invoke _finishCallback().
        return;
    }

    _attemptExec = std::make_unique<executor::ScopedTaskExecutor>(
        _exec, Status(ErrorCodes::CallbackCanceled, "Initial Sync Attempt Canceled"));
    _clonerAttemptExec = std::make_unique<executor::ScopedTaskExecutor>(
        _clonerExec, Status(ErrorCodes::CallbackCanceled, "Initial Sync Attempt Canceled"));
    _attemptCanceled = false;
    auto when = (*_attemptExec)->now() + _opts.initialSyncRetryWait;
    auto status = _scheduleWorkAtAndSaveHandle_inlock(
        when,
        [=](const executor::TaskExecutor::CallbackArgs& args) {
            _startInitialSyncAttemptCallback(
                args, _stats.failedInitialSyncAttempts, _stats.maxFailedInitialSyncAttempts);
        },
        &_startInitialSyncAttemptHandle,
        str::stream() << "_startInitialSyncAttemptCallback-" << _stats.failedInitialSyncAttempts);

    if (!status.isOK()) {
        result = status;
        // Scope guard will invoke _finishCallback().
        return;
    }

    // Next initial sync attempt scheduled successfully and we do not need to call _finishCallback()
    // until the next initial sync attempt finishes.
    finishCallbackGuard.dismiss();
}

void InitialSyncer::_finishCallback(StatusWith<OpTimeAndWallTime> lastApplied) {
    // After running callback function, clear '_onCompletion' to release any resources that might be
    // held by this function object.
    // '_onCompletion' must be moved to a temporary copy and destroyed outside the lock in case
    // there is any logic that's invoked at the function object's destruction that might call into
    // this InitialSyncer. 'onCompletion' must be destroyed outside the lock and this should happen
    // before we transition the state to Complete.
    decltype(_onCompletion) onCompletion;
    {
        stdx::lock_guard<Latch> lock(_mutex);
        auto opCtx = makeOpCtx();
        _tearDown_inlock(opCtx.get(), lastApplied);
        invariant(_onCompletion);
        std::swap(_onCompletion, onCompletion);
    }

    if (MONGO_unlikely(initialSyncHangBeforeFinish.shouldFail())) {
        // This log output is used in js tests so please leave it.
        LOGV2(21194,
              "initial sync - initialSyncHangBeforeFinish fail point "
              "enabled. Blocking until fail point is disabled.");
        while (MONGO_unlikely(initialSyncHangBeforeFinish.shouldFail()) && !_isShuttingDown()) {
            mongo::sleepsecs(1);
        }
    }

    // Any _retryingOperation is no longer active.  This must be done before signalling state
    // Complete.
    _retryingOperation = boost::none;

    // Completion callback must be invoked outside mutex.
    try {
        onCompletion(lastApplied);
    } catch (...) {
        LOGV2_WARNING(21198,
                      "initial syncer finish callback threw exception: {error}",
                      "Initial syncer finish callback threw exception",
                      "error"_attr = redact(exceptionToStatus()));
    }

    // Destroy the remaining reference to the completion callback before we transition the state to
    // Complete so that callers can expect any resources bound to '_onCompletion' to be released
    // before InitialSyncer::join() returns.
    onCompletion = {};

    {
        stdx::lock_guard<Latch> lock(_mutex);
        invariant(_state != State::kComplete);
        _state = State::kComplete;
        _stateCondition.notify_all();

        // Clear the initial sync progress after an initial sync attempt has been successfully
        // completed.
        if (lastApplied.isOK() && !MONGO_unlikely(skipClearInitialSyncState.shouldFail())) {
            _initialSyncState.reset();
        }

        // Destroy shared references to executors.
        _attemptExec = nullptr;
        _clonerAttemptExec = nullptr;
        _clonerExec = nullptr;
        _exec = nullptr;
    }

    if (MONGO_unlikely(initialSyncHangAfterFinish.shouldFail())) {
        LOGV2(5825800,
              "initial sync finished - initialSyncHangAfterFinish fail point "
              "enabled. Blocking until fail point is disabled.");
        while (MONGO_unlikely(initialSyncHangAfterFinish.shouldFail()) && !_isShuttingDown()) {
            mongo::sleepsecs(1);
        }
    }
}

Status InitialSyncer::_scheduleLastOplogEntryFetcher_inlock(
    Fetcher::CallbackFn callback, LastOplogEntryFetcherRetryStrategy retryStrategy) {
    BSONObj query =
        BSON("find" << NamespaceString::kRsOplogNamespace.coll() << "sort" << BSON("$natural" << -1)
                    << "limit" << 1 << ReadConcernArgs::kReadConcernFieldName
                    << ReadConcernArgs::kLocal);

    _lastOplogEntryFetcher = std::make_unique<Fetcher>(
        *_attemptExec,
        _syncSource,
        NamespaceString::kRsOplogNamespace.db().toString(),
        query,
        callback,
        ReadPreferenceSetting::secondaryPreferredMetadata(),
        RemoteCommandRequest::kNoTimeout /* find network timeout */,
        RemoteCommandRequest::kNoTimeout /* getMore network timeout */,
        (retryStrategy == kFetcherHandlesRetries)
            ? RemoteCommandRetryScheduler::makeRetryPolicy<ErrorCategory::RetriableError>(
                  numInitialSyncOplogFindAttempts.load(),
                  executor::RemoteCommandRequest::kNoTimeout)
            : RemoteCommandRetryScheduler::makeNoRetryPolicy());
    Status scheduleStatus = _lastOplogEntryFetcher->schedule();
    if (!scheduleStatus.isOK()) {
        _lastOplogEntryFetcher.reset();
    }

    return scheduleStatus;
}

void InitialSyncer::_checkApplierProgressAndScheduleGetNextApplierBatch_inlock(
    const stdx::lock_guard<Latch>& lock, std::shared_ptr<OnCompletionGuard> onCompletionGuard) {
    // We should check our current state because shutdown() could have been called before
    // we re-acquired the lock.
    if (_isShuttingDown_inlock()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(
            lock,
            Status(ErrorCodes::CallbackCanceled,
                   "failed to schedule applier to check for "
                   "rollback: initial syncer is shutting down"));
        return;
    }

    // Basic sanity check on begin/stop timestamps.
    if (_initialSyncState->beginApplyingTimestamp > _initialSyncState->stopTimestamp) {
        static constexpr char msg[] = "Possible rollback on sync source";
        LOGV2_ERROR(21201,
                    msg,
                    "syncSource"_attr = _syncSource,
                    "stopTimestamp"_attr = _initialSyncState->stopTimestamp.toBSON(),
                    "beginApplyingTimestamp"_attr =
                        _initialSyncState->beginApplyingTimestamp.toBSON());
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(
            lock,
            Status(ErrorCodes::OplogOutOfOrder,
                   str::stream() << msg << " " << _syncSource.toString() << ". Currently at "
                                 << _initialSyncState->stopTimestamp.toBSON() << ". Started at "
                                 << _initialSyncState->beginApplyingTimestamp.toBSON()));
        return;
    }

    if (_lastApplied.opTime.isNull()) {
        // Check if any ops occurred while cloning or any ops need to be fetched.
        invariant(_initialSyncState->beginFetchingTimestamp < _initialSyncState->stopTimestamp);
        LOGV2(21195,
              "Writing to the oplog and applying operations until {stopTimestamp} "
              "before initial sync can complete. (started fetching at "
              "{beginFetchingTimestamp} and applying at "
              "{beginApplyingTimestamp})",
              "Writing to the oplog and applying operations until stopTimestamp before initial "
              "sync can complete",
              "stopTimestamp"_attr = _initialSyncState->stopTimestamp.toBSON(),
              "beginFetchingTimestamp"_attr = _initialSyncState->beginFetchingTimestamp.toBSON(),
              "beginApplyingTimestamp"_attr = _initialSyncState->beginApplyingTimestamp.toBSON());
        // Fall through to scheduling _getNextApplierBatchCallback().
    } else if (_lastApplied.opTime.getTimestamp() >= _initialSyncState->stopTimestamp) {
        // Check for rollback if we have applied far enough to be consistent.
        invariant(!_lastApplied.opTime.getTimestamp().isNull());
        _scheduleRollbackCheckerCheckForRollback_inlock(lock, onCompletionGuard);
        return;
    }

    // Get another batch to apply.
    // _scheduleWorkAndSaveHandle_inlock() is shutdown-aware.
    auto status = _scheduleWorkAndSaveHandle_inlock(
        [=](const executor::TaskExecutor::CallbackArgs& args) {
            return _getNextApplierBatchCallback(args, onCompletionGuard);
        },
        &_getNextApplierBatchHandle,
        "_getNextApplierBatchCallback");
    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }
}

void InitialSyncer::_scheduleRollbackCheckerCheckForRollback_inlock(
    const stdx::lock_guard<Latch>& lock, std::shared_ptr<OnCompletionGuard> onCompletionGuard) {
    // We should check our current state because shutdown() could have been called before
    // we re-acquired the lock.
    if (_isShuttingDown_inlock()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(
            lock,
            Status(ErrorCodes::CallbackCanceled,
                   "failed to schedule rollback checker to check "
                   "for rollback: initial syncer is shutting "
                   "down"));
        return;
    }

    auto scheduleResult =
        _rollbackChecker->checkForRollback([=](const RollbackChecker::Result& result) {
            _rollbackCheckerCheckForRollbackCallback(result, onCompletionGuard);
        });

    auto status = scheduleResult.getStatus();
    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }

    _getLastRollbackIdHandle = scheduleResult.getValue();
    return;
}

bool InitialSyncer::_shouldRetryError(WithLock lk, Status status) {
    if (ErrorCodes::isRetriableError(status)) {
        stdx::lock_guard<InitialSyncSharedData> sharedDataLock(*_sharedData);
        return _sharedData->shouldRetryOperation(sharedDataLock, &_retryingOperation);
    }
    // The status was OK or some error other than a retriable error, so clear the retriable error
    // state and indicate that we should not retry.
    _clearRetriableError(lk);
    return false;
}

void InitialSyncer::_clearRetriableError(WithLock lk) {
    _retryingOperation = boost::none;
}

Status InitialSyncer::_checkForShutdownAndConvertStatus_inlock(
    const executor::TaskExecutor::CallbackArgs& callbackArgs, const std::string& message) {
    return _checkForShutdownAndConvertStatus_inlock(callbackArgs.status, message);
}

Status InitialSyncer::_checkForShutdownAndConvertStatus_inlock(const Status& status,
                                                               const std::string& message) {

    if (_isShuttingDown_inlock()) {
        return Status(ErrorCodes::CallbackCanceled, message + ": initial syncer is shutting down");
    }

    return status.withContext(message);
}

Status InitialSyncer::_scheduleWorkAndSaveHandle_inlock(
    executor::TaskExecutor::CallbackFn work,
    executor::TaskExecutor::CallbackHandle* handle,
    const std::string& name) {
    invariant(handle);
    if (_isShuttingDown_inlock()) {
        return Status(ErrorCodes::CallbackCanceled,
                      str::stream() << "failed to schedule work " << name
                                    << ": initial syncer is shutting down");
    }
    auto result = (*_attemptExec)->scheduleWork(std::move(work));
    if (!result.isOK()) {
        return result.getStatus().withContext(str::stream() << "failed to schedule work " << name);
    }
    *handle = result.getValue();
    return Status::OK();
}

Status InitialSyncer::_scheduleWorkAtAndSaveHandle_inlock(
    Date_t when,
    executor::TaskExecutor::CallbackFn work,
    executor::TaskExecutor::CallbackHandle* handle,
    const std::string& name) {
    invariant(handle);
    if (_isShuttingDown_inlock()) {
        return Status(ErrorCodes::CallbackCanceled,
                      str::stream() << "failed to schedule work " << name << " at "
                                    << when.toString() << ": initial syncer is shutting down");
    }
    auto result = (*_attemptExec)->scheduleWorkAt(when, std::move(work));
    if (!result.isOK()) {
        return result.getStatus().withContext(str::stream() << "failed to schedule work " << name
                                                            << " at " << when.toString());
    }
    *handle = result.getValue();
    return Status::OK();
}

void InitialSyncer::_cancelHandle_inlock(executor::TaskExecutor::CallbackHandle handle) {
    if (!handle) {
        return;
    }
    (*_attemptExec)->cancel(handle);
}

template <typename Component>
Status InitialSyncer::_startupComponent_inlock(Component& component) {
    // It is necessary to check if shutdown or attempt cancelling happens before starting a
    // component; otherwise the component may call a callback function in line which will
    // cause a deadlock when the callback attempts to obtain the initial syncer mutex.
    if (_isShuttingDown_inlock() || _attemptCanceled) {
        component.reset();
        if (_isShuttingDown_inlock()) {
            return Status(ErrorCodes::CallbackCanceled,
                          "initial syncer shutdown while trying to call startup() on component");
        } else {
            return Status(
                ErrorCodes::CallbackCanceled,
                "initial sync attempt canceled while trying to call startup() on component");
        }
    }
    auto status = component->startup();
    if (!status.isOK()) {
        component.reset();
    }
    return status;
}

template <typename Component>
void InitialSyncer::_shutdownComponent_inlock(Component& component) {
    if (!component) {
        return;
    }
    component->shutdown();
}

StatusWith<std::vector<OplogEntry>> InitialSyncer::_getNextApplierBatch_inlock() {
    // If the fail-point is active, delay the apply batch by returning an empty batch so that
    // _getNextApplierBatchCallback() will reschedule itself at a later time.
    // See InitialSyncerInterface::Options::getApplierBatchCallbackRetryWait.
    if (MONGO_unlikely(rsSyncApplyStop.shouldFail())) {
        return std::vector<OplogEntry>();
    }

    // Obtain next batch of operations from OplogApplier.
    auto opCtx = makeOpCtx();
    OplogApplier::BatchLimits batchLimits;
    batchLimits.bytes = replBatchLimitBytes.load();
    batchLimits.ops = getBatchLimitOplogEntries();
    // We want a batch boundary after the beginApplyingTimestamp, to make sure all oplog entries
    // that are part of a transaction before that timestamp are written out before we start applying
    // entries after them.  This is because later entries may be commit or prepare and thus
    // expect to read the partial entries from the oplog.
    batchLimits.forceBatchBoundaryAfter = _initialSyncState->beginApplyingTimestamp;
    return _oplogApplier->getNextApplierBatch(opCtx.get(), batchLimits);
}

StatusWith<HostAndPort> InitialSyncer::_chooseSyncSource_inlock() {
    auto syncSource = _opts.syncSourceSelector->chooseNewSyncSource(_lastFetched);
    if (syncSource.empty()) {
        return Status{ErrorCodes::InvalidSyncSource,
                      str::stream() << "No valid sync source available. Our last fetched optime: "
                                    << _lastFetched.toString()};
    }
    return syncSource;
}

Status InitialSyncer::_enqueueDocuments(OplogFetcher::Documents::const_iterator begin,
                                        OplogFetcher::Documents::const_iterator end,
                                        const OplogFetcher::DocumentsInfo& info) {
    if (info.toApplyDocumentCount == 0) {
        return Status::OK();
    }

    if (_isShuttingDown()) {
        return Status::OK();
    }

    invariant(_oplogBuffer);

    // Wait for enough space.
    _oplogApplier->waitForSpace(makeOpCtx().get(), info.toApplyDocumentBytes);

    // Buffer docs for later application.
    _oplogApplier->enqueue(makeOpCtx().get(), begin, end);

    _lastFetched = info.lastDocument;

    // TODO: updates metrics with "info".
    return Status::OK();
}

std::string InitialSyncer::Stats::toString() const {
    return toBSON().toString();
}

BSONObj InitialSyncer::Stats::toBSON() const {
    BSONObjBuilder bob;
    append(&bob);
    return bob.obj();
}

void InitialSyncer::Stats::append(BSONObjBuilder* builder) const {
    builder->appendNumber("failedInitialSyncAttempts",
                          static_cast<long long>(failedInitialSyncAttempts));
    builder->appendNumber("maxFailedInitialSyncAttempts",
                          static_cast<long long>(maxFailedInitialSyncAttempts));

    auto e = exec.lock();
    if (initialSyncStart != Date_t()) {
        builder->appendDate("initialSyncStart", initialSyncStart);
        auto elapsedDurationEnd = e ? e->now() : Date_t::now();
        if (initialSyncEnd != Date_t()) {
            builder->appendDate("initialSyncEnd", initialSyncEnd);
            elapsedDurationEnd = initialSyncEnd;
        }
        long long elapsedMillis =
            duration_cast<Milliseconds>(elapsedDurationEnd - initialSyncStart).count();
        builder->appendNumber("totalInitialSyncElapsedMillis", elapsedMillis);
    }

    BSONArrayBuilder arrBuilder(builder->subarrayStart("initialSyncAttempts"));
    for (unsigned int i = 0; i < initialSyncAttemptInfos.size(); ++i) {
        arrBuilder.append(initialSyncAttemptInfos[i].toBSON());
    }
    arrBuilder.doneFast();
}

std::string InitialSyncer::InitialSyncAttemptInfo::toString() const {
    return toBSON().toString();
}

BSONObj InitialSyncer::InitialSyncAttemptInfo::toBSON() const {
    BSONObjBuilder bob;
    append(&bob);
    return bob.obj();
}

void InitialSyncer::InitialSyncAttemptInfo::append(BSONObjBuilder* builder) const {
    builder->appendNumber("durationMillis", durationMillis);
    builder->append("status", status.toString());
    builder->append("syncSource", syncSource.toString());
    if (rollBackId >= 0) {
        builder->append("rollBackId", rollBackId);
    }
    builder->append("operationsRetried", operationsRetried);
    builder->append("totalTimeUnreachableMillis", totalTimeUnreachableMillis);
}

bool InitialSyncer::OplogFetcherRestartDecisionInitialSyncer::shouldContinue(OplogFetcher* fetcher,
                                                                             Status status) {
    if (ErrorCodes::isRetriableError(status)) {
        stdx::lock_guard<InitialSyncSharedData> lk(*_sharedData);
        return _sharedData->shouldRetryOperation(lk, &_retryingOperation);
    }
    // A non-network error occured, so clear any network error and use the default restart
    // strategy.
    _retryingOperation = boost::none;
    return _defaultDecision.shouldContinue(fetcher, status);
}

void InitialSyncer::OplogFetcherRestartDecisionInitialSyncer::fetchSuccessful(
    OplogFetcher* fetcher) {
    _retryingOperation = boost::none;
    _defaultDecision.fetchSuccessful(fetcher);
}

}  // namespace repl
}  // namespace mongo
