/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "initial_syncer.h"

#include <algorithm>
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
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/databases_cloner.h"
#include "mongo/db/repl/initial_sync_state.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/db/repl/oplog_fetcher.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_consistency_markers.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/sync_source_selector.h"
#include "mongo/db/server_parameters.h"
#include "mongo/executor/task_executor.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/destructor_guard.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace repl {

// Failpoint for initial sync
MONGO_FAIL_POINT_DEFINE(failInitialSyncWithBadHost);

// Failpoint which fails initial sync and leaves an oplog entry in the buffer.
MONGO_FAIL_POINT_DEFINE(failInitSyncWithBufferedEntriesLeft);

// Failpoint which causes the initial sync function to hang before copying databases.
MONGO_FAIL_POINT_DEFINE(initialSyncHangBeforeCopyingDatabases);

// Failpoint which causes the initial sync function to hang before finishing.
MONGO_FAIL_POINT_DEFINE(initialSyncHangBeforeFinish);

// Failpoint which causes the initial sync function to hang before calling shouldRetry on a failed
// operation.
MONGO_FAIL_POINT_DEFINE(initialSyncHangBeforeGettingMissingDocument);

// Failpoint which stops the applier.
MONGO_FAIL_POINT_DEFINE(rsSyncApplyStop);

namespace {
using namespace executor;
using CallbackArgs = executor::TaskExecutor::CallbackArgs;
using Event = executor::TaskExecutor::EventHandle;
using Handle = executor::TaskExecutor::CallbackHandle;
using Operations = MultiApplier::Operations;
using QueryResponseStatus = StatusWith<Fetcher::QueryResponse>;
using UniqueLock = stdx::unique_lock<stdx::mutex>;
using LockGuard = stdx::lock_guard<stdx::mutex>;

// 16MB max batch size / 12 byte min doc size * 10 (for good measure) = defaultBatchSize to use.
const auto defaultBatchSize = (16 * 1024 * 1024) / 12 * 10;

// The number of attempts to connect to a sync source.
MONGO_EXPORT_SERVER_PARAMETER(numInitialSyncConnectAttempts, int, 10);

// The number of attempts to call find on the remote oplog.
MONGO_EXPORT_SERVER_PARAMETER(numInitialSyncOplogFindAttempts, int, 3);

// The batchSize to use for the find/getMore queries called by the OplogFetcher
MONGO_EXPORT_STARTUP_SERVER_PARAMETER(initialSyncOplogFetcherBatchSize, int, defaultBatchSize);

// The number of initial sync attempts that have failed since server startup. Each instance of
// InitialSyncer may run multiple attempts to fulfill an initial sync request that is triggered
// when InitialSyncer::startup() is called.
Counter64 initialSyncFailedAttempts;

// The number of initial sync requests that have been requested and failed. Each instance of
// InitialSyncer (upon successful startup()) corresponds to a single initial sync request.
// This value does not include the number of times where a InitialSyncer is created successfully
// but failed in startup().
Counter64 initialSyncFailures;

// The number of initial sync requests that have been requested and completed successfully. Each
// instance of InitialSyncer corresponds to a single initial sync request.
Counter64 initialSyncCompletes;

ServerStatusMetricField<Counter64> displaySSInitialSyncFailedAttempts(
    "repl.initialSync.failedAttempts", &initialSyncFailedAttempts);
ServerStatusMetricField<Counter64> displaySSInitialSyncFailures("repl.initialSync.failures",
                                                                &initialSyncFailures);
ServerStatusMetricField<Counter64> displaySSInitialSyncCompleted("repl.initialSync.completed",
                                                                 &initialSyncCompletes);

ServiceContext::UniqueOperationContext makeOpCtx() {
    return cc().makeOperationContext();
}

StatusWith<Timestamp> parseTimestampStatus(const QueryResponseStatus& fetchResult) {
    if (!fetchResult.isOK()) {
        return fetchResult.getStatus();
    } else {
        const auto docs = fetchResult.getValue().documents;
        const auto hasDoc = docs.begin() != docs.end();
        if (!hasDoc || !docs.begin()->hasField("ts")) {
            return {ErrorCodes::FailedToParse, "Could not find an oplog entry with 'ts' field."};
        } else {
            return {docs.begin()->getField("ts").timestamp()};
        }
    }
}

StatusWith<OpTimeWithHash> parseOpTimeWithHash(const QueryResponseStatus& fetchResult) {
    if (!fetchResult.isOK()) {
        return fetchResult.getStatus();
    }
    const auto docs = fetchResult.getValue().documents;
    const auto hasDoc = docs.begin() != docs.end();
    return hasDoc
        ? AbstractOplogFetcher::parseOpTimeWithHash(docs.front())
        : StatusWith<OpTimeWithHash>{ErrorCodes::NoMatchingDocument, "no oplog entry found"};
}

/**
 * OplogApplier observer that updates 'fetchCount' when applying operations for each writer thread.
 */
class InitialSyncApplyObserver : public OplogApplier::Observer {
public:
    explicit InitialSyncApplyObserver(AtomicUInt32* fetchCount) : _fetchCount(fetchCount) {}

    // OplogApplier::Observer functions
    void onBatchBegin(const OplogApplier::Operations&) final {}
    void onBatchEnd(const StatusWith<OpTime>&, const OplogApplier::Operations&) final {}
    void onMissingDocumentsFetchedAndInserted(const std::vector<FetchInfo>& docs) final {
        _fetchCount->fetchAndAdd(docs.size());
    }

private:
    AtomicUInt32* const _fetchCount;
};

}  // namespace

InitialSyncer::InitialSyncer(
    InitialSyncerOptions opts,
    std::unique_ptr<DataReplicatorExternalState> dataReplicatorExternalState,
    ThreadPool* writerPool,
    StorageInterface* storage,
    ReplicationProcess* replicationProcess,
    const OnCompletionFn& onCompletion)
    : _fetchCount(0),
      _opts(opts),
      _dataReplicatorExternalState(std::move(dataReplicatorExternalState)),
      _exec(_dataReplicatorExternalState->getTaskExecutor()),
      _writerPool(writerPool),
      _storage(storage),
      _replicationProcess(replicationProcess),
      _onCompletion(onCompletion),
      _observer(std::make_unique<InitialSyncApplyObserver>(&_fetchCount)) {
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
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _isActive_inlock();
}

bool InitialSyncer::_isActive_inlock() const {
    return State::kRunning == _state || State::kShuttingDown == _state;
}

Status InitialSyncer::startup(OperationContext* opCtx,
                              std::uint32_t initialSyncMaxAttempts) noexcept {
    invariant(opCtx);
    invariant(initialSyncMaxAttempts >= 1U);

    stdx::lock_guard<stdx::mutex> lock(_mutex);
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
    stdx::lock_guard<stdx::mutex> lock(_mutex);
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

void InitialSyncer::_cancelRemainingWork_inlock() {
    _cancelHandle_inlock(_startInitialSyncAttemptHandle);
    _cancelHandle_inlock(_chooseSyncSourceHandle);
    _cancelHandle_inlock(_getBaseRollbackIdHandle);
    _cancelHandle_inlock(_getLastRollbackIdHandle);
    _cancelHandle_inlock(_getNextApplierBatchHandle);

    _shutdownComponent_inlock(_oplogFetcher);
    if (_initialSyncState) {
        _shutdownComponent_inlock(_initialSyncState->dbsCloner);
    }
    _shutdownComponent_inlock(_applier);
    _shutdownComponent_inlock(_fCVFetcher);
    _shutdownComponent_inlock(_lastOplogEntryFetcher);
}

void InitialSyncer::join() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _stateCondition.wait(lk, [this]() { return !_isActive_inlock(); });
}

InitialSyncer::State InitialSyncer::getState_forTest() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _state;
}

bool InitialSyncer::_isShuttingDown() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _isShuttingDown_inlock();
}

bool InitialSyncer::_isShuttingDown_inlock() const {
    return State::kShuttingDown == _state;
}

std::string InitialSyncer::getDiagnosticString() const {
    LockGuard lk(_mutex);
    str::stream out;
    out << "InitialSyncer -"
        << " opts: " << _opts.toString() << " oplogFetcher: " << _oplogFetcher->toString()
        << " opsBuffered: " << _oplogBuffer->getSize() << " active: " << _isActive_inlock()
        << " shutting down: " << _isShuttingDown_inlock();
    if (_initialSyncState) {
        out << " opsAppied: " << _initialSyncState->appliedOps;
    }

    return out;
}

BSONObj InitialSyncer::getInitialSyncProgress() const {
    LockGuard lk(_mutex);
    return _getInitialSyncProgress_inlock();
}

void InitialSyncer::_appendInitialSyncProgressMinimal_inlock(BSONObjBuilder* bob) const {
    _stats.append(bob);
    if (!_initialSyncState) {
        return;
    }
    bob->appendNumber("fetchedMissingDocs", _initialSyncState->fetchedMissingDocs);
    bob->appendNumber("appliedOps", _initialSyncState->appliedOps);
    if (!_initialSyncState->beginTimestamp.isNull()) {
        bob->append("initialSyncOplogStart", _initialSyncState->beginTimestamp);
    }
    if (!_initialSyncState->stopTimestamp.isNull()) {
        bob->append("initialSyncOplogEnd", _initialSyncState->stopTimestamp);
    }
}

BSONObj InitialSyncer::_getInitialSyncProgress_inlock() const {
    try {
        BSONObjBuilder bob;
        _appendInitialSyncProgressMinimal_inlock(&bob);
        if (_initialSyncState) {
            if (_initialSyncState->dbsCloner) {
                BSONObjBuilder dbsBuilder(bob.subobjStart("databases"));
                _initialSyncState->dbsCloner->getStats().append(&dbsBuilder);
                dbsBuilder.doneFast();
            }
        }
        return bob.obj();
    } catch (const DBException& e) {
        log() << "Error creating initial sync progress object: " << e.toString();
    }
    BSONObjBuilder bob;
    _appendInitialSyncProgressMinimal_inlock(&bob);
    return bob.obj();
}

void InitialSyncer::setScheduleDbWorkFn_forTest(const CollectionCloner::ScheduleDbWorkFn& work) {
    LockGuard lk(_mutex);
    _scheduleDbWorkFn = work;
}

void InitialSyncer::_setUp_inlock(OperationContext* opCtx, std::uint32_t initialSyncMaxAttempts) {
    // 'opCtx' is passed through from startup().
    _replicationProcess->getConsistencyMarkers()->setInitialSyncFlag(opCtx);

    auto serviceCtx = opCtx->getServiceContext();
    _storage->setInitialDataTimestamp(serviceCtx, Timestamp::kAllowUnstableCheckpointsSentinel);
    _storage->setStableTimestamp(serviceCtx, Timestamp::min());

    LOG(1) << "Creating oplogBuffer.";
    _oplogBuffer = _dataReplicatorExternalState->makeInitialSyncOplogBuffer(opCtx);
    _oplogBuffer->startup(opCtx);

    _stats.initialSyncStart = _exec->now();
    _stats.maxFailedInitialSyncAttempts = initialSyncMaxAttempts;
    _stats.failedInitialSyncAttempts = 0;
}

void InitialSyncer::_tearDown_inlock(OperationContext* opCtx,
                                     const StatusWith<OpTimeWithHash>& lastApplied) {
    _stats.initialSyncEnd = _exec->now();

    // This might not be necessary if we failed initial sync.
    invariant(_oplogBuffer);
    _oplogBuffer->shutdown(opCtx);

    if (!lastApplied.isOK()) {
        return;
    }

    // This is necessary to ensure that the oplog contains at least one visible document prior to
    // setting an externally visible lastApplied.  That way if any other node attempts to read from
    // this node's oplog, it won't appear empty.
    _storage->waitForAllEarlierOplogWritesToBeVisible(opCtx);

    _replicationProcess->getConsistencyMarkers()->clearInitialSyncFlag(opCtx);

    // All updates that represent initial sync must be completed before setting the initial data
    // timestamp.
    _storage->setInitialDataTimestamp(opCtx->getServiceContext(),
                                      lastApplied.getValue().opTime.getTimestamp());

    auto currentLastAppliedOpTime = _opts.getMyLastOptime();
    if (currentLastAppliedOpTime.isNull()) {
        _opts.setMyLastOptime(lastApplied.getValue().opTime,
                              ReplicationCoordinator::DataConsistency::Consistent);
    } else {
        invariant(currentLastAppliedOpTime == lastApplied.getValue().opTime);
    }

    log() << "initial sync done; took "
          << duration_cast<Seconds>(_stats.initialSyncEnd - _stats.initialSyncStart) << ".";
    initialSyncCompletes.increment();
}

void InitialSyncer::_startInitialSyncAttemptCallback(
    const executor::TaskExecutor::CallbackArgs& callbackArgs,
    std::uint32_t initialSyncAttempt,
    std::uint32_t initialSyncMaxAttempts) {
    auto status = _checkForShutdownAndConvertStatus_inlock(
        callbackArgs,
        str::stream() << "error while starting initial sync attempt " << (initialSyncAttempt + 1)
                      << " of "
                      << initialSyncMaxAttempts);
    if (!status.isOK()) {
        _finishInitialSyncAttempt(status);
        return;
    }

    log() << "Starting initial sync (attempt " << (initialSyncAttempt + 1) << " of "
          << initialSyncMaxAttempts << ")";

    // This completion guard invokes _finishInitialSyncAttempt on destruction.
    auto cancelRemainingWorkInLock = [this]() { _cancelRemainingWork_inlock(); };
    auto finishInitialSyncAttemptFn = [this](const StatusWith<OpTimeWithHash>& lastApplied) {
        _finishInitialSyncAttempt(lastApplied);
    };
    auto onCompletionGuard =
        std::make_shared<OnCompletionGuard>(cancelRemainingWorkInLock, finishInitialSyncAttemptFn);

    // Lock guard must be declared after completion guard because completion guard destructor
    // has to run outside lock.
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    _oplogApplier = {};

    LOG(2) << "Resetting sync source so a new one can be chosen for this initial sync attempt.";
    _syncSource = HostAndPort();

    LOG(2) << "Resetting all optimes before starting this initial sync attempt.";
    _opts.resetOptimes();
    _lastApplied = {};
    _lastFetched = {};

    LOG(2) << "Resetting feature compatibility version to last-stable. If the sync source is in "
              "latest feature compatibility version, we will find out when we clone the "
              "server configuration collection (admin.system.version).";
    serverGlobalParams.featureCompatibility.reset();

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
    std::shared_ptr<OnCompletionGuard> onCompletionGuard) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    // Cancellation should be treated the same as other errors. In this case, the most likely cause
    // of a failed _chooseSyncSourceCallback() task is a cancellation triggered by
    // InitialSyncer::shutdown() or the task executor shutting down.
    auto status =
        _checkForShutdownAndConvertStatus_inlock(callbackArgs, "error while choosing sync source");
    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }

    if (MONGO_FAIL_POINT(failInitialSyncWithBadHost)) {
        status = Status(ErrorCodes::InvalidSyncSource,
                        "no sync source avail(failInitialSyncWithBadHost failpoint is set).");
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

        auto when = _exec->now() + _opts.syncSourceRetryWait;
        LOG(1) << "Error getting sync source: '" << syncSource.getStatus() << "', trying again in "
               << _opts.syncSourceRetryWait << " at " << when.toString() << ". Attempt "
               << (chooseSyncSourceAttempt + 1) << " of " << numInitialSyncConnectAttempts.load();
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

    // There is no need to schedule separate task to create oplog collection since we are already in
    // a callback and we are certain there's no existing operation context (required for creating
    // collections and dropping user databases) attached to the current thread.
    status = _truncateOplogAndDropReplicatedDatabases();
    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }

    _syncSource = syncSource.getValue();

    // Create oplog applier.
    auto consistencyMarkers = _replicationProcess->getConsistencyMarkers();
    OplogApplier::Options options;
    options.allowNamespaceNotFoundErrorsOnCrudOps = true;
    options.missingDocumentSourceForInitialSync = _syncSource;
    _oplogApplier = _dataReplicatorExternalState->makeOplogApplier(
        _oplogBuffer.get(), _observer.get(), consistencyMarkers, _storage, options, _writerPool);

    // Schedule rollback ID checker.
    _rollbackChecker = stdx::make_unique<RollbackChecker>(_exec, _syncSource);
    auto scheduleResult = _rollbackChecker->reset([=](const RollbackChecker::Result& result) {
        return _rollbackCheckerResetCallback(result, onCompletionGuard);
    });
    status = scheduleResult.getStatus();
    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }
    _getBaseRollbackIdHandle = scheduleResult.getValue();
}

Status InitialSyncer::_truncateOplogAndDropReplicatedDatabases() {
    // truncate oplog; drop user databases.
    LOG(1) << "About to truncate the oplog, if it exists, ns:" << _opts.localOplogNS
           << ", and drop all user databases (so that we can clone them).";

    auto opCtx = makeOpCtx();

    // We are not replicating nor validating these writes.
    UnreplicatedWritesBlock unreplicatedWritesBlock(opCtx.get());

    // 1.) Truncate the oplog.
    LOG(2) << "Truncating the existing oplog: " << _opts.localOplogNS;
    Timer timer;
    auto status = _storage->truncateCollection(opCtx.get(), _opts.localOplogNS);
    log() << "Initial syncer oplog truncation finished in: " << timer.millis() << "ms";
    if (!status.isOK()) {
        // 1a.) Create the oplog.
        LOG(2) << "Creating the oplog: " << _opts.localOplogNS;
        status = _storage->createOplog(opCtx.get(), _opts.localOplogNS);
        if (!status.isOK()) {
            return status;
        }
    }

    // 2.) Drop user databases.
    LOG(2) << "Dropping user databases";
    return _storage->dropReplicatedDatabases(opCtx.get());
}

void InitialSyncer::_rollbackCheckerResetCallback(
    const RollbackChecker::Result& result, std::shared_ptr<OnCompletionGuard> onCompletionGuard) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    auto status = _checkForShutdownAndConvertStatus_inlock(result.getStatus(),
                                                           "error while getting base rollback ID");
    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }

    status = _scheduleLastOplogEntryFetcher_inlock(
        [=](const StatusWith<mongo::Fetcher::QueryResponse>& response,
            mongo::Fetcher::NextAction*,
            mongo::BSONObjBuilder*) {
            _lastOplogEntryFetcherCallbackForBeginTimestamp(response, onCompletionGuard);
        });
    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }
}

void InitialSyncer::_lastOplogEntryFetcherCallbackForBeginTimestamp(
    const StatusWith<Fetcher::QueryResponse>& result,
    std::shared_ptr<OnCompletionGuard> onCompletionGuard) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    auto status = _checkForShutdownAndConvertStatus_inlock(
        result.getStatus(), "error while getting last oplog entry for begin timestamp");
    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }

    const auto opTimeWithHashResult = parseOpTimeWithHash(result);
    status = opTimeWithHashResult.getStatus();
    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }

    const auto& lastOpTimeWithHash = opTimeWithHashResult.getValue();

    BSONObjBuilder queryBob;
    queryBob.append("find", NamespaceString::kServerConfigurationNamespace.coll());
    auto filterBob = BSONObjBuilder(queryBob.subobjStart("filter"));
    filterBob.append("_id", FeatureCompatibilityVersionParser::kParameterName);
    filterBob.done();

    _fCVFetcher = stdx::make_unique<Fetcher>(
        _exec,
        _syncSource,
        NamespaceString::kServerConfigurationNamespace.db().toString(),
        queryBob.obj(),
        [=](const StatusWith<mongo::Fetcher::QueryResponse>& response,
            mongo::Fetcher::NextAction*,
            mongo::BSONObjBuilder*) {
            _fcvFetcherCallback(response, onCompletionGuard, lastOpTimeWithHash);
        },
        ReadPreferenceSetting::secondaryPreferredMetadata(),
        RemoteCommandRequest::kNoTimeout /* find network timeout */,
        RemoteCommandRequest::kNoTimeout /* getMore network timeout */,
        RemoteCommandRetryScheduler::makeRetryPolicy(
            numInitialSyncOplogFindAttempts.load(),
            executor::RemoteCommandRequest::kNoTimeout,
            RemoteCommandRetryScheduler::kAllRetriableErrors));
    Status scheduleStatus = _fCVFetcher->schedule();
    if (!scheduleStatus.isOK()) {
        _fCVFetcher.reset();
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, scheduleStatus);
        return;
    }
}

void InitialSyncer::_fcvFetcherCallback(const StatusWith<Fetcher::QueryResponse>& result,
                                        std::shared_ptr<OnCompletionGuard> onCompletionGuard,
                                        const OpTimeWithHash& lastOpTimeWithHash) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
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
                   str::stream() << "Expected to receive one document, but received: "
                                 << docs.size()
                                 << ". First: "
                                 << redact(docs.front())
                                 << ". Last: "
                                 << redact(docs.back())));
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
    if (version > ServerGlobalParams::FeatureCompatibility::Version::kFullyDowngradedTo36 &&
        version < ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo40) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(
            lock,
            Status(ErrorCodes::IncompatibleServerVersion,
                   str::stream() << "Sync source had unsafe feature compatibility version: "
                                 << FeatureCompatibilityVersionParser::toString(version)));
        return;
    }

    // This is where the flow of control starts to split into two parallel tracks:
    // - oplog fetcher
    // - data cloning and applier
    auto listDatabasesFilter = [](BSONObj dbInfo) {
        std::string name;
        auto status = mongo::bsonExtractStringField(dbInfo, "name", &name);
        if (!status.isOK()) {
            error() << "listDatabases filter failed to parse database name from " << redact(dbInfo)
                    << ": " << redact(status);
            return false;
        }
        return (name != "local");
    };
    _initialSyncState = stdx::make_unique<InitialSyncState>(stdx::make_unique<DatabasesCloner>(
        _storage, _exec, _writerPool, _syncSource, listDatabasesFilter, [=](const Status& status) {
            _databasesClonerCallback(status, onCompletionGuard);
        }));

    _initialSyncState->beginTimestamp = lastOpTimeWithHash.opTime.getTimestamp();

    invariant(!result.getValue().documents.empty());
    LOG(2) << "Setting begin timestamp to " << _initialSyncState->beginTimestamp
           << " using last oplog entry: " << redact(result.getValue().documents.front())
           << ", ns: " << _opts.localOplogNS;


    const auto configResult = _dataReplicatorExternalState->getCurrentConfig();
    status = configResult.getStatus();
    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        _initialSyncState.reset();
        return;
    }

    const auto& config = configResult.getValue();
    _oplogFetcher = stdx::make_unique<OplogFetcher>(
        _exec,
        lastOpTimeWithHash,
        _syncSource,
        _opts.remoteOplogNS,
        config,
        _opts.oplogFetcherMaxFetcherRestarts,
        _rollbackChecker->getBaseRBID(),
        false /* requireFresherSyncSource */,
        _dataReplicatorExternalState.get(),
        [=](Fetcher::Documents::const_iterator first,
            Fetcher::Documents::const_iterator last,
            const OplogFetcher::DocumentsInfo& info) {
            return _enqueueDocuments(first, last, info);
        },
        [=](const Status& s) { _oplogFetcherCallback(s, onCompletionGuard); },
        initialSyncOplogFetcherBatchSize);

    LOG(2) << "Starting OplogFetcher: " << _oplogFetcher->toString();

    // _startupComponent_inlock is shutdown-aware.
    status = _startupComponent_inlock(_oplogFetcher);
    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        _initialSyncState->dbsCloner.reset();
        return;
    }

    if (MONGO_FAIL_POINT(initialSyncHangBeforeCopyingDatabases)) {
        lock.unlock();
        // This could have been done with a scheduleWorkAt but this is used only by JS tests where
        // we run with multiple threads so it's fine to spin on this thread.
        // This log output is used in js tests so please leave it.
        log() << "initial sync - initialSyncHangBeforeCopyingDatabases fail point "
                 "enabled. Blocking until fail point is disabled.";
        while (MONGO_FAIL_POINT(initialSyncHangBeforeCopyingDatabases) && !_isShuttingDown()) {
            mongo::sleepsecs(1);
        }
        lock.lock();
    }

    if (_scheduleDbWorkFn) {
        // '_scheduleDbWorkFn' is passed through (DatabasesCloner->DatabaseCloner->CollectionCloner)
        // to the CollectionCloner so that CollectionCloner's default TaskRunner can be disabled to
        // facilitate testing.
        _initialSyncState->dbsCloner->setScheduleDbWorkFn_forTest(_scheduleDbWorkFn);
    }

    LOG(2) << "Starting DatabasesCloner: " << _initialSyncState->dbsCloner->toString();

    // _startupComponent_inlock() is shutdown-aware. Additionally, if the component fails to
    // startup, _startupComponent_inlock() resets the unique_ptr to the component (in this case,
    // DatabasesCloner).
    status = _startupComponent_inlock(_initialSyncState->dbsCloner);
    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }
}

void InitialSyncer::_oplogFetcherCallback(const Status& oplogFetcherFinishStatus,
                                          std::shared_ptr<OnCompletionGuard> onCompletionGuard) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    log() << "Finished fetching oplog during initial sync: " << redact(oplogFetcherFinishStatus)
          << ". Last fetched optime and hash: " << _lastFetched.toString();

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
        log() << "Finished fetching oplog fetching early. Last fetched optime and hash: "
              << _lastFetched.toString();
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

void InitialSyncer::_databasesClonerCallback(const Status& databaseClonerFinishStatus,
                                             std::shared_ptr<OnCompletionGuard> onCompletionGuard) {
    log() << "Finished cloning data: " << redact(databaseClonerFinishStatus)
          << ". Beginning oplog replay.";

    stdx::lock_guard<stdx::mutex> lock(_mutex);
    auto status = _checkForShutdownAndConvertStatus_inlock(databaseClonerFinishStatus,
                                                           "error cloning databases");
    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }

    status = _scheduleLastOplogEntryFetcher_inlock(
        [=](const StatusWith<mongo::Fetcher::QueryResponse>& status,
            mongo::Fetcher::NextAction*,
            mongo::BSONObjBuilder*) {
            _lastOplogEntryFetcherCallbackForStopTimestamp(status, onCompletionGuard);
        });
    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }
}

void InitialSyncer::_lastOplogEntryFetcherCallbackForStopTimestamp(
    const StatusWith<Fetcher::QueryResponse>& result,
    std::shared_ptr<OnCompletionGuard> onCompletionGuard) {
    OpTimeWithHash optimeWithHash;
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        auto status = _checkForShutdownAndConvertStatus_inlock(
            result.getStatus(), "error fetching last oplog entry for stop timestamp");
        if (!status.isOK()) {
            onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
            return;
        }

        auto&& optimeWithHashStatus = parseOpTimeWithHash(result);
        if (!optimeWithHashStatus.isOK()) {
            onCompletionGuard->setResultAndCancelRemainingWork_inlock(
                lock, optimeWithHashStatus.getStatus());
            return;
        }
        optimeWithHash = optimeWithHashStatus.getValue();
        _initialSyncState->stopTimestamp = optimeWithHash.opTime.getTimestamp();

        if (_initialSyncState->beginTimestamp != _initialSyncState->stopTimestamp) {
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
        LOG(2) << "Inserting oplog seed document: " << oplogSeedDoc;

        auto opCtx = makeOpCtx();
        // StorageInterface::insertDocument() has to be called outside the lock because we may
        // override its behavior in tests. See InitialSyncerReturnsCallbackCanceledAndDoesNot-
        // ScheduleRollbackCheckerIfShutdownAfterInsertingInsertOplogSeedDocument in
        // initial_syncer_test.cpp
        auto status = _storage->insertDocument(
            opCtx.get(),
            _opts.localOplogNS,
            TimestampedBSONObj{oplogSeedDoc, optimeWithHash.opTime.getTimestamp()},
            optimeWithHash.opTime.getTerm());
        if (!status.isOK()) {
            stdx::lock_guard<stdx::mutex> lock(_mutex);
            onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
            return;
        }
        const bool orderedCommit = true;
        _storage->oplogDiskLocRegister(
            opCtx.get(), optimeWithHash.opTime.getTimestamp(), orderedCommit);
    }

    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _lastApplied = optimeWithHash;
    log() << "No need to apply operations. (currently at "
          << _initialSyncState->stopTimestamp.toBSON() << ")";

    // This sets the error in 'onCompletionGuard' and shuts down the OplogFetcher on error.
    _scheduleRollbackCheckerCheckForRollback_inlock(lock, onCompletionGuard);
}

void InitialSyncer::_getNextApplierBatchCallback(
    const executor::TaskExecutor::CallbackArgs& callbackArgs,
    std::shared_ptr<OnCompletionGuard> onCompletionGuard) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    auto status =
        _checkForShutdownAndConvertStatus_inlock(callbackArgs, "error getting next applier batch");
    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }

    auto batchResult = _getNextApplierBatch_inlock();
    if (!batchResult.isOK()) {
        warning() << "Failure creating next apply batch: " << redact(batchResult.getStatus());
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, batchResult.getStatus());
        return;
    }

    // Schedule MultiApplier if we have operations to apply.
    const auto& ops = batchResult.getValue();
    if (!ops.empty()) {
        _fetchCount.store(0);
        MultiApplier::MultiApplyFn applyBatchOfOperationsFn = [this](OperationContext* opCtx,
                                                                     MultiApplier::Operations ops) {
            return _oplogApplier->multiApply(opCtx, std::move(ops));
        };
        const auto& lastEntry = ops.back();
        OpTimeWithHash lastApplied(lastEntry.getHash(), lastEntry.getOpTime());
        auto numApplied = ops.size();
        MultiApplier::CallbackFn onCompletionFn = [=](const Status& s) {
            return _multiApplierCallback(s, lastApplied, numApplied, onCompletionGuard);
        };

        _applier = stdx::make_unique<MultiApplier>(
            _exec, ops, std::move(applyBatchOfOperationsFn), std::move(onCompletionFn));
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
        std::string msg = str::stream()
            << "The oplog fetcher is no longer running and we have applied all the oplog entries "
               "in the oplog buffer. Aborting this initial sync attempt. Last applied: "
            << _lastApplied.toString() << ". Last fetched: " << _lastFetched.toString()
            << ". Number of operations applied: " << _initialSyncState->appliedOps;
        log() << msg;
        status = Status(ErrorCodes::RemoteResultsUnavailable, msg);
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }

    // If there are no operations at the moment to apply and the oplog fetcher is still waiting on
    // the sync source, we'll check the oplog buffer again in
    // '_opts.getApplierBatchCallbackRetryWait' ms.
    auto when = _exec->now() + _opts.getApplierBatchCallbackRetryWait;
    status = _scheduleWorkAtAndSaveHandle_inlock(
        when,
        [=](const CallbackArgs& args) { _getNextApplierBatchCallback(args, onCompletionGuard); },
        &_getNextApplierBatchHandle,
        "_getNextApplierBatchCallback");
    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }
}

void InitialSyncer::_multiApplierCallback(const Status& multiApplierStatus,
                                          OpTimeWithHash lastApplied,
                                          std::uint32_t numApplied,
                                          std::shared_ptr<OnCompletionGuard> onCompletionGuard) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    auto status =
        _checkForShutdownAndConvertStatus_inlock(multiApplierStatus, "error applying batch");
    if (!status.isOK()) {
        error() << "Failed to apply batch due to '" << redact(status) << "'";
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }

    _initialSyncState->appliedOps += numApplied;
    _lastApplied = lastApplied;
    _opts.setMyLastOptime(_lastApplied.opTime,
                          ReplicationCoordinator::DataConsistency::Inconsistent);

    auto fetchCount = _fetchCount.load();
    if (fetchCount > 0) {
        _initialSyncState->fetchedMissingDocs += fetchCount;
        _fetchCount.store(0);
        status = _scheduleLastOplogEntryFetcher_inlock(
            [=](const StatusWith<mongo::Fetcher::QueryResponse>& response,
                mongo::Fetcher::NextAction*,
                mongo::BSONObjBuilder*) {
                return _lastOplogEntryFetcherCallbackAfterFetchingMissingDocuments(
                    response, onCompletionGuard);
            });
        if (!status.isOK()) {
            onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
            return;
        }
        return;
    }

    _checkApplierProgressAndScheduleGetNextApplierBatch_inlock(lock, onCompletionGuard);
}

void InitialSyncer::_lastOplogEntryFetcherCallbackAfterFetchingMissingDocuments(
    const StatusWith<Fetcher::QueryResponse>& result,
    std::shared_ptr<OnCompletionGuard> onCompletionGuard) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    auto status = _checkForShutdownAndConvertStatus_inlock(
        result.getStatus(), "error getting last oplog entry after fetching missing documents");
    if (!status.isOK()) {
        error() << "Failed to get new minValid from source " << _syncSource << " due to '"
                << redact(status) << "'";
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    }

    auto&& optimeWithHashStatus = parseOpTimeWithHash(result);
    if (!optimeWithHashStatus.isOK()) {
        error() << "Failed to parse new minValid from source " << _syncSource << " due to '"
                << redact(optimeWithHashStatus.getStatus()) << "'";
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock,
                                                                  optimeWithHashStatus.getStatus());
        return;
    }
    auto&& optimeWithHash = optimeWithHashStatus.getValue();

    const auto newOplogEnd = optimeWithHash.opTime.getTimestamp();
    LOG(2) << "Pushing back minValid from " << _initialSyncState->stopTimestamp << " to "
           << newOplogEnd;
    _initialSyncState->stopTimestamp = newOplogEnd;

    // Get another batch to apply.
    _checkApplierProgressAndScheduleGetNextApplierBatch_inlock(lock, onCompletionGuard);
}

void InitialSyncer::_rollbackCheckerCheckForRollbackCallback(
    const RollbackChecker::Result& result, std::shared_ptr<OnCompletionGuard> onCompletionGuard) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    auto status = _checkForShutdownAndConvertStatus_inlock(result.getStatus(),
                                                           "error while getting last rollback ID");
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

    // Update all unique indexes belonging to non-replicated collections on secondaries. See comment
    // in ReplicationCoordinatorExternalStateImpl::initializeReplSetStorage() for the explanation of
    // why we do this.
    // TODO: SERVER-34489 should add a check for latest FCV before making the upgrade call when
    // upgrade downgrade is ready.
    if (createTimestampSafeUniqueIndex) {
        auto opCtx = makeOpCtx();
        auto updateStatus = _storage->upgradeNonReplicatedUniqueIndexes(opCtx.get());
        if (!updateStatus.isOK()) {
            onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, updateStatus);
            return;
        }
    }

    // Success!
    onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, _lastApplied);
}

void InitialSyncer::_finishInitialSyncAttempt(const StatusWith<OpTimeWithHash>& lastApplied) {
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
    auto finishCallbackGuard = MakeGuard([this, &result] {
        auto scheduleResult = _exec->scheduleWork(
            [=](const mongo::executor::TaskExecutor::CallbackArgs&) { _finishCallback(result); });
        if (!scheduleResult.isOK()) {
            warning() << "Unable to schedule initial syncer completion task due to "
                      << redact(scheduleResult.getStatus())
                      << ". Running callback on current thread.";
            _finishCallback(result);
        }
    });

    log() << "Initial sync attempt finishing up.";

    stdx::lock_guard<stdx::mutex> lock(_mutex);
    log() << "Initial Sync Attempt Statistics: " << redact(_getInitialSyncProgress_inlock());

    auto runTime = _initialSyncState ? _initialSyncState->timer.millis() : 0;
    _stats.initialSyncAttemptInfos.emplace_back(
        InitialSyncer::InitialSyncAttemptInfo{runTime, result.getStatus(), _syncSource});

    if (result.isOK()) {
        // Scope guard will invoke _finishCallback().
        return;
    }


    // This increments the number of failed attempts for the current initial sync request.
    ++_stats.failedInitialSyncAttempts;

    // This increments the number of failed attempts across all initial sync attempts since process
    // startup.
    initialSyncFailedAttempts.increment();

    error() << "Initial sync attempt failed -- attempts left: "
            << (_stats.maxFailedInitialSyncAttempts - _stats.failedInitialSyncAttempts)
            << " cause: " << redact(result.getStatus());

    // Check if need to do more retries.
    if (_stats.failedInitialSyncAttempts >= _stats.maxFailedInitialSyncAttempts) {
        const std::string err =
            "The maximum number of retries have been exhausted for initial sync.";
        severe() << err;

        initialSyncFailures.increment();

        // Scope guard will invoke _finishCallback().
        return;
    }

    auto when = _exec->now() + _opts.initialSyncRetryWait;
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
    finishCallbackGuard.Dismiss();
}

void InitialSyncer::_finishCallback(StatusWith<OpTimeWithHash> lastApplied) {
    // After running callback function, clear '_onCompletion' to release any resources that might be
    // held by this function object.
    // '_onCompletion' must be moved to a temporary copy and destroyed outside the lock in case
    // there is any logic that's invoked at the function object's destruction that might call into
    // this InitialSyncer. 'onCompletion' must be destroyed outside the lock and this should happen
    // before we transition the state to Complete.
    decltype(_onCompletion) onCompletion;
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        auto opCtx = makeOpCtx();
        _tearDown_inlock(opCtx.get(), lastApplied);

        invariant(_onCompletion);
        std::swap(_onCompletion, onCompletion);
    }

    if (MONGO_FAIL_POINT(initialSyncHangBeforeFinish)) {
        // This log output is used in js tests so please leave it.
        log() << "initial sync - initialSyncHangBeforeFinish fail point "
                 "enabled. Blocking until fail point is disabled.";
        while (MONGO_FAIL_POINT(initialSyncHangBeforeFinish) && !_isShuttingDown()) {
            mongo::sleepsecs(1);
        }
    }

    // Completion callback must be invoked outside mutex.
    try {
        onCompletion(lastApplied);
    } catch (...) {
        warning() << "initial syncer finish callback threw exception: "
                  << redact(exceptionToStatus());
    }

    // Destroy the remaining reference to the completion callback before we transition the state to
    // Complete so that callers can expect any resources bound to '_onCompletion' to be released
    // before InitialSyncer::join() returns.
    onCompletion = {};

    stdx::lock_guard<stdx::mutex> lock(_mutex);
    invariant(_state != State::kComplete);
    _state = State::kComplete;
    _stateCondition.notify_all();
}

Status InitialSyncer::_scheduleLastOplogEntryFetcher_inlock(Fetcher::CallbackFn callback) {
    BSONObj query = BSON(
        "find" << _opts.remoteOplogNS.coll() << "sort" << BSON("$natural" << -1) << "limit" << 1);

    _lastOplogEntryFetcher =
        stdx::make_unique<Fetcher>(_exec,
                                   _syncSource,
                                   _opts.remoteOplogNS.db().toString(),
                                   query,
                                   callback,
                                   ReadPreferenceSetting::secondaryPreferredMetadata(),
                                   RemoteCommandRequest::kNoTimeout /* find network timeout */,
                                   RemoteCommandRequest::kNoTimeout /* getMore network timeout */,
                                   RemoteCommandRetryScheduler::makeRetryPolicy(
                                       numInitialSyncOplogFindAttempts.load(),
                                       executor::RemoteCommandRequest::kNoTimeout,
                                       RemoteCommandRetryScheduler::kAllRetriableErrors));
    Status scheduleStatus = _lastOplogEntryFetcher->schedule();
    if (!scheduleStatus.isOK()) {
        _lastOplogEntryFetcher.reset();
    }

    return scheduleStatus;
}

void InitialSyncer::_checkApplierProgressAndScheduleGetNextApplierBatch_inlock(
    const stdx::lock_guard<stdx::mutex>& lock,
    std::shared_ptr<OnCompletionGuard> onCompletionGuard) {
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
    if (_initialSyncState->beginTimestamp > _initialSyncState->stopTimestamp) {
        std::string msg = str::stream()
            << "Possible rollback on sync source " << _syncSource.toString() << ". Currently at "
            << _initialSyncState->stopTimestamp.toBSON() << ". Started at "
            << _initialSyncState->beginTimestamp.toBSON();
        error() << msg;
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(
            lock, Status(ErrorCodes::OplogOutOfOrder, msg));
        return;
    }

    if (_lastApplied.opTime.isNull()) {
        // Check if any ops occurred while cloning.
        invariant(_initialSyncState->beginTimestamp < _initialSyncState->stopTimestamp);
        log() << "Applying operations until " << _initialSyncState->stopTimestamp.toBSON()
              << " before initial sync can complete. (starting at "
              << _initialSyncState->beginTimestamp.toBSON() << ")";
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
    const stdx::lock_guard<stdx::mutex>& lock,
    std::shared_ptr<OnCompletionGuard> onCompletionGuard) {
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
    const executor::TaskExecutor::CallbackFn& work,
    executor::TaskExecutor::CallbackHandle* handle,
    const std::string& name) {
    invariant(handle);
    if (_isShuttingDown_inlock()) {
        return Status(ErrorCodes::CallbackCanceled,
                      str::stream() << "failed to schedule work " << name
                                    << ": initial syncer is shutting down");
    }
    auto result = _exec->scheduleWork(work);
    if (!result.isOK()) {
        return result.getStatus().withContext(str::stream() << "failed to schedule work " << name);
    }
    *handle = result.getValue();
    return Status::OK();
}

Status InitialSyncer::_scheduleWorkAtAndSaveHandle_inlock(
    Date_t when,
    const executor::TaskExecutor::CallbackFn& work,
    executor::TaskExecutor::CallbackHandle* handle,
    const std::string& name) {
    invariant(handle);
    if (_isShuttingDown_inlock()) {
        return Status(ErrorCodes::CallbackCanceled,
                      str::stream() << "failed to schedule work " << name << " at "
                                    << when.toString()
                                    << ": initial syncer is shutting down");
    }
    auto result = _exec->scheduleWorkAt(when, work);
    if (!result.isOK()) {
        return result.getStatus().withContext(
            str::stream() << "failed to schedule work " << name << " at " << when.toString());
    }
    *handle = result.getValue();
    return Status::OK();
}

void InitialSyncer::_cancelHandle_inlock(executor::TaskExecutor::CallbackHandle handle) {
    if (!handle) {
        return;
    }
    _exec->cancel(handle);
}

template <typename Component>
Status InitialSyncer::_startupComponent_inlock(Component& component) {
    if (_isShuttingDown_inlock()) {
        component.reset();
        return Status(ErrorCodes::CallbackCanceled,
                      "initial syncer shutdown while trying to call startup() on component");
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

StatusWith<Operations> InitialSyncer::_getNextApplierBatch_inlock() {
    // If the fail-point is active, delay the apply batch by returning an empty batch so that
    // _getNextApplierBatchCallback() will reschedule itself at a later time.
    // See InitialSyncerOptions::getApplierBatchCallbackRetryWait.
    if (MONGO_FAIL_POINT(rsSyncApplyStop)) {
        return Operations();
    }

    // Obtain next batch of operations from OplogApplier.
    auto opCtx = makeOpCtx();
    OplogApplier::BatchLimits batchLimits;
    batchLimits.bytes = OplogApplier::replBatchLimitBytes;
    batchLimits.ops = OplogApplier::getBatchLimitOperations();
    return _oplogApplier->getNextApplierBatch(opCtx.get(), batchLimits);
}

StatusWith<HostAndPort> InitialSyncer::_chooseSyncSource_inlock() {
    auto syncSource = _opts.syncSourceSelector->chooseNewSyncSource(_lastFetched.opTime);
    if (syncSource.empty()) {
        return Status{ErrorCodes::InvalidSyncSource,
                      str::stream() << "No valid sync source available. Our last fetched optime: "
                                    << _lastFetched.opTime.toString()};
    }
    return syncSource;
}

Status InitialSyncer::_enqueueDocuments(Fetcher::Documents::const_iterator begin,
                                        Fetcher::Documents::const_iterator end,
                                        const OplogFetcher::DocumentsInfo& info) {
    if (info.toApplyDocumentCount == 0) {
        return Status::OK();
    }

    if (_isShuttingDown()) {
        return Status::OK();
    }

    invariant(_oplogBuffer);

    // Wait for enough space.
    // Gets unblocked on shutdown.
    _oplogBuffer->waitForSpace(makeOpCtx().get(), info.toApplyDocumentBytes);

    OCCASIONALLY {
        LOG(2) << "bgsync buffer has " << _oplogBuffer->getSize() << " bytes";
    }

    // Buffer docs for later application.
    _oplogBuffer->pushAllNonBlocking(makeOpCtx().get(), begin, end);

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
    if (initialSyncStart != Date_t()) {
        builder->appendDate("initialSyncStart", initialSyncStart);
        if (initialSyncEnd != Date_t()) {
            builder->appendDate("initialSyncEnd", initialSyncEnd);
            auto elapsed = initialSyncEnd - initialSyncStart;
            long long elapsedMillis = duration_cast<Milliseconds>(elapsed).count();
            builder->appendNumber("initialSyncElapsedMillis", elapsedMillis);
        }
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
}

}  // namespace repl
}  // namespace mongo
