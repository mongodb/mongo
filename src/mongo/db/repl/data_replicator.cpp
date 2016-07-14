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

#include "data_replicator.h"

#include <algorithm>

#include "mongo/base/status.h"
#include "mongo/client/fetcher.h"
#include "mongo/client/remote_command_retry_scheduler.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/databases_cloner.h"
#include "mongo/db/repl/initial_sync_state.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/db/repl/oplog_fetcher.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/rollback_checker.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/sync_source_selector.h"
#include "mongo/executor/task_executor.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
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
const int kInitialSyncMaxRetries = 9;

// Failpoint for initial sync
MONGO_FP_DECLARE(failInitialSyncWithBadHost);

// Failpoint which fails initial sync and leaves an oplog entry in the buffer.
MONGO_FP_DECLARE(failInitSyncWithBufferedEntriesLeft);

// Failpoint which causes the initial sync function to hang before copying databases.
MONGO_FP_DECLARE(initialSyncHangBeforeCopyingDatabases);

// Failpoint which causes the initial sync function to hang before calling shouldRetry on a failed
// operation.
MONGO_FP_DECLARE(initialSyncHangBeforeGettingMissingDocument);

// Failpoint which stops the applier.
MONGO_FP_DECLARE(rsSyncApplyStop);

namespace {
using namespace executor;
using CallbackArgs = executor::TaskExecutor::CallbackArgs;
using Event = executor::TaskExecutor::EventHandle;
using Handle = executor::TaskExecutor::CallbackHandle;
using Operations = MultiApplier::Operations;
using QueryResponseStatus = StatusWith<Fetcher::QueryResponse>;
using UniqueLock = stdx::unique_lock<stdx::mutex>;
using LockGuard = stdx::lock_guard<stdx::mutex>;

// The number of retries for the find command.
const size_t numFindRetries = 3;

ServiceContext::UniqueOperationContext makeOpCtx() {
    return cc().makeOperationContext();
}

StatusWith<TaskExecutor::CallbackHandle> scheduleWork(
    TaskExecutor* exec,
    stdx::function<void(OperationContext* txn, const CallbackArgs& cbData)> func) {

    // Wrap 'func' with a lambda that checks for cancallation and creates an OperationContext*.
    return exec->scheduleWork([func](const CallbackArgs& cbData) {
        if (cbData.status == ErrorCodes::CallbackCanceled) {
            return;
        }
        auto txn = makeOpCtx();
        func(txn.get(), cbData);
    });
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

StatusWith<BSONObj> getLatestOplogEntry(executor::TaskExecutor* exec,
                                        HostAndPort source,
                                        const NamespaceString& oplogNS) {
    BSONObj query =
        BSON("find" << oplogNS.coll() << "sort" << BSON("$natural" << -1) << "limit" << 1);

    BSONObj entry;
    Status statusToReturn(Status::OK());
    Fetcher fetcher(
        exec,
        source,
        oplogNS.db().toString(),
        query,
        [&entry, &statusToReturn](const QueryResponseStatus& fetchResult,
                                  Fetcher::NextAction* nextAction,
                                  BSONObjBuilder*) {
            if (!fetchResult.isOK()) {
                statusToReturn = fetchResult.getStatus();
            } else {
                const auto docs = fetchResult.getValue().documents;
                invariant(docs.size() < 2);
                if (docs.size() == 0) {
                    statusToReturn = {ErrorCodes::OplogStartMissing, "no oplog entry found."};
                } else {
                    entry = docs.back().getOwned();
                }
            }
        });
    Status scheduleStatus = fetcher.schedule();
    if (!scheduleStatus.isOK()) {
        return scheduleStatus;
    }

    // wait for fetcher to get the oplog position.
    fetcher.wait();
    if (statusToReturn.isOK()) {
        LOG(2) << "returning last oplog entry: " << entry << ", from: " << source
               << ", ns: " << oplogNS;
        return entry;
    }
    return statusToReturn;
}

StatusWith<OpTimeWithHash> parseOpTimeWithHash(const BSONObj& oplogEntry) {
    auto oplogEntryHash = oplogEntry["h"].Long();
    const auto lastOpTime = OpTime::parseFromOplogEntry(oplogEntry);
    if (!lastOpTime.isOK()) {
        return lastOpTime.getStatus();
    }

    return OpTimeWithHash{oplogEntryHash, lastOpTime.getValue()};
}

StatusWith<OpTimeWithHash> parseOpTimeWithHash(const QueryResponseStatus& fetchResult) {
    if (!fetchResult.isOK()) {
        return fetchResult.getStatus();
    }
    const auto docs = fetchResult.getValue().documents;
    const auto hasDoc = docs.begin() != docs.end();
    return hasDoc
        ? parseOpTimeWithHash(docs.front())
        : StatusWith<OpTimeWithHash>{ErrorCodes::NoMatchingDocument, "No document in batch."};
}

Timestamp findCommonPoint(HostAndPort host, Timestamp start) {
    // TODO: walk back in the oplog looking for a known/shared optime.
    return Timestamp();
}

}  // namespace

std::string toString(DataReplicatorState s) {
    switch (s) {
        case DataReplicatorState::InitialSync:
            return "InitialSync";
        case DataReplicatorState::Rollback:
            return "Rollback";
        case DataReplicatorState::Steady:
            return "Steady Replication";
        case DataReplicatorState::Uninitialized:
            return "Uninitialized";
    }
    MONGO_UNREACHABLE;
}

// Data Replicator
DataReplicator::DataReplicator(
    DataReplicatorOptions opts,
    std::unique_ptr<DataReplicatorExternalState> dataReplicatorExternalState,
    StorageInterface* storage)
    : _opts(opts),
      _dataReplicatorExternalState(std::move(dataReplicatorExternalState)),
      _exec(_dataReplicatorExternalState->getTaskExecutor()),
      _state(DataReplicatorState::Uninitialized),
      _storage(storage) {
    uassert(ErrorCodes::BadValue, "invalid storage interface", _storage);
    uassert(ErrorCodes::BadValue, "invalid rollback function", _opts.rollbackFn);
    uassert(ErrorCodes::BadValue,
            "invalid replSetUpdatePosition command object creation function",
            _opts.prepareReplSetUpdatePositionCommandFn);
    uassert(ErrorCodes::BadValue, "invalid getMyLastOptime function", _opts.getMyLastOptime);
    uassert(ErrorCodes::BadValue, "invalid setMyLastOptime function", _opts.setMyLastOptime);
    uassert(ErrorCodes::BadValue, "invalid setFollowerMode function", _opts.setFollowerMode);
    uassert(ErrorCodes::BadValue, "invalid getSlaveDelay function", _opts.getSlaveDelay);
    uassert(ErrorCodes::BadValue, "invalid sync source selector", _opts.syncSourceSelector);
}

DataReplicator::~DataReplicator() {
    DESTRUCTOR_GUARD({
        UniqueLock lk(_mutex);
        _cancelAllHandles_inlock();
        _waitOnAndResetAll(lk);
    });
}

Status DataReplicator::start(OperationContext* txn) {
    UniqueLock lk(_mutex);
    if (_state != DataReplicatorState::Uninitialized) {
        return Status(ErrorCodes::IllegalOperation,
                      str::stream() << "Already started in another state: " << toString(_state));
    }

    _setState_inlock(DataReplicatorState::Steady);
    _applierPaused = false;
    _fetcherPaused = false;
    _reporterPaused = false;
    _oplogBuffer = _dataReplicatorExternalState->makeSteadyStateOplogBuffer(txn);
    _oplogBuffer->startup(txn);
    _doNextActions_Steady_inlock();
    return Status::OK();
}

Status DataReplicator::shutdown(OperationContext* txn) {
    return _shutdown(txn);
}

Status DataReplicator::pause() {
    _pauseApplier();
    return Status::OK();
}

DataReplicatorState DataReplicator::getState() const {
    LockGuard lk(_mutex);
    return _state;
}

void DataReplicator::waitForState(const DataReplicatorState& state) {
    UniqueLock lk(_mutex);
    while (_state != state) {
        _stateCondition.wait(lk);
    }
}

HostAndPort DataReplicator::getSyncSource() const {
    LockGuard lk(_mutex);
    return _syncSource;
}

OpTimeWithHash DataReplicator::getLastFetched() const {
    LockGuard lk(_mutex);
    return _lastFetched;
}

OpTimeWithHash DataReplicator::getLastApplied() const {
    LockGuard lk(_mutex);
    return _lastApplied;
}

size_t DataReplicator::getOplogBufferCount() const {
    // Oplog buffer is internally synchronized.
    return _oplogBuffer->getCount();
}

std::string DataReplicator::getDiagnosticString() const {
    LockGuard lk(_mutex);
    str::stream out;
    out << "DataReplicator -"
        << " opts: " << _opts.toString() << " oplogFetcher: " << _oplogFetcher->toString()
        << " opsBuffered: " << _oplogBuffer->getSize() << " state: " << toString(_state);
    switch (_state) {
        case DataReplicatorState::InitialSync:
            out << " opsAppied: " << _initialSyncState->appliedOps
                << " status: " << _initialSyncState->status.toString();
            break;
        case DataReplicatorState::Steady:
            // TODO: add more here
            break;
        case DataReplicatorState::Rollback:
            // TODO: add more here
            break;
        default:
            break;
    }

    return out;
}

Status DataReplicator::resume(bool wait) {
    CBHStatus handle = _exec->scheduleWork(
        stdx::bind(&DataReplicator::_resumeFinish, this, stdx::placeholders::_1));
    const Status status = handle.getStatus();
    if (wait && status.isOK()) {
        _exec->wait(handle.getValue());
    }
    return status;
}

void DataReplicator::_resumeFinish(CallbackArgs cbData) {
    UniqueLock lk(_mutex);
    _fetcherPaused = _applierPaused = false;
    lk.unlock();

    _doNextActions();
}

void DataReplicator::_pauseApplier() {
    LockGuard lk(_mutex);
    if (_applier)
        _applier->wait();
    _applierPaused = true;
    _applier.reset();
}

Timestamp DataReplicator::_applyUntil(Timestamp untilTimestamp) {
    // TODO: block until all oplog buffer application is done to the given optime
    return Timestamp();
}

Timestamp DataReplicator::_applyUntilAndPause(Timestamp untilTimestamp) {
    //_run(&_pauseApplier);
    return _applyUntil(untilTimestamp);
}

StatusWith<Timestamp> DataReplicator::flushAndPause() {
    //_run(&_pauseApplier);
    UniqueLock lk(_mutex);
    if (_applierActive) {
        _applierPaused = true;
        lk.unlock();
        _applier->wait();
        lk.lock();
    }
    return StatusWith<Timestamp>(_lastApplied.opTime.getTimestamp());
}

void DataReplicator::_resetState_inlock(OperationContext* txn, OpTimeWithHash lastAppliedOpTime) {
    invariant(!_anyActiveHandles_inlock());
    _lastApplied = _lastFetched = lastAppliedOpTime;
    _oplogBuffer->clear(txn);
}

void DataReplicator::slavesHaveProgressed() {
    if (_reporter) {
        _reporter->trigger();
    }
}

StatusWith<Timestamp> DataReplicator::resync(OperationContext* txn) {
    _shutdown(txn);
    // Drop databases and do initialSync();
    CBHStatus cbh = scheduleWork(_exec, [this](OperationContext* txn, const CallbackArgs& cbData) {
        _storage->dropReplicatedDatabases(txn);
    });

    if (!cbh.isOK()) {
        return cbh.getStatus();
    }

    _exec->wait(cbh.getValue());

    auto status = doInitialSync(txn);
    if (status.isOK()) {
        return status.getValue().opTime.getTimestamp();
    } else {
        return status.getStatus();
    }
}

Status DataReplicator::_runInitialSyncAttempt_inlock(OperationContext* txn,
                                                     UniqueLock& lk,
                                                     const HostAndPort& syncSource,
                                                     RollbackChecker& rollbackChecker) {
    invariant(lk.owns_lock());
    Status statusFromWrites(ErrorCodes::NotYetInitialized, "About to run Initial Sync Attempt.");

    // drop/create oplog; drop user databases.
    LOG(1) << "About to drop+create the oplog, if it exists, ns:" << _opts.localOplogNS
           << ", and drop all user databases (so that we can clone them).";
    const auto schedStatus = scheduleWork(
        _exec, [&statusFromWrites, this](OperationContext* txn, const CallbackArgs& cd) {
            /**
             * This functions does the following:
             *      1.) Drop oplog
             *      2.) Drop user databases (replicated dbs)
             *      3.) Create oplog
             */
            if (!cd.status.isOK()) {
                error() << "Error while being called to drop/create oplog and drop users "
                        << "databases, oplogNS: " << _opts.localOplogNS
                        << " with status:" << cd.status.toString();
                statusFromWrites = cd.status;
                return;
            }

            invariant(txn);
            // We are not replicating nor validating these writes.
            txn->setReplicatedWrites(false);

            // 1.) Drop the oplog.
            LOG(2) << "Dropping the existing oplog: " << _opts.localOplogNS;
            statusFromWrites = _storage->dropCollection(txn, _opts.localOplogNS);


            // 2.) Drop user databases.
            // TODO: Do not do this once we have resume.
            if (statusFromWrites.isOK()) {
                LOG(2) << "Dropping  user databases";
                statusFromWrites = _storage->dropReplicatedDatabases(txn);
            }

            // 3.) Crete the oplog.
            if (statusFromWrites.isOK()) {
                LOG(2) << "Creating the oplog: " << _opts.localOplogNS;
                statusFromWrites = _storage->createOplog(txn, _opts.localOplogNS);
            }

        });

    if (!schedStatus.isOK())
        return schedStatus.getStatus();

    lk.unlock();
    _exec->wait(schedStatus.getValue());
    if (!statusFromWrites.isOK()) {
        lk.lock();
        return statusFromWrites;
    }

    auto rollbackStatus = rollbackChecker.reset_sync();
    lk.lock();
    if (!rollbackStatus.isOK())
        return rollbackStatus;

    Event initialSyncFinishEvent;
    StatusWith<Event> eventStatus = _exec->makeEvent();
    if (!eventStatus.isOK()) {
        return eventStatus.getStatus();
    }
    initialSyncFinishEvent = eventStatus.getValue();

    invariant(initialSyncFinishEvent.isValid());
    _initialSyncState.reset(new InitialSyncState(
        stdx::make_unique<DatabasesCloner>(
            _storage,
            _exec,
            _dataReplicatorExternalState->getDbWorkThreadPool(),
            syncSource,
            [](BSONObj dbInfo) {
                const std::string name = dbInfo["name"].str();
                return (name != "local");
            },
            stdx::bind(&DataReplicator::_onDataClonerFinish, this, stdx::placeholders::_1)),
        initialSyncFinishEvent));

    const NamespaceString ns(_opts.remoteOplogNS);
    lk.unlock();
    // get the latest oplog entry, and parse out the optime + hash.
    const auto lastOplogEntry = getLatestOplogEntry(_exec, syncSource, ns);
    const auto lastOplogEntryOpTimeWithHashStatus = lastOplogEntry.isOK()
        ? parseOpTimeWithHash(lastOplogEntry.getValue())
        : StatusWith<OpTimeWithHash>{lastOplogEntry.getStatus()};

    lk.lock();

    if (!lastOplogEntryOpTimeWithHashStatus.isOK())
        return lastOplogEntryOpTimeWithHashStatus.getStatus();

    _initialSyncState->oplogSeedDoc = lastOplogEntry.getValue().getOwned();
    const auto lastOpTimeWithHash = lastOplogEntryOpTimeWithHashStatus.getValue();
    _initialSyncState->beginTimestamp = lastOpTimeWithHash.opTime.getTimestamp();

    if (_oplogFetcher) {
        if (_oplogFetcher->isActive()) {
            LOG(3) << "Fetcher is active, stopping it.";
            _oplogFetcher->shutdown();
        }
    }
    _oplogFetcher.reset();

    const auto config = uassertStatusOK(_dataReplicatorExternalState->getCurrentConfig());
    _oplogFetcher = stdx::make_unique<OplogFetcher>(_exec,
                                                    lastOpTimeWithHash,
                                                    syncSource,
                                                    _opts.remoteOplogNS,
                                                    config,
                                                    _dataReplicatorExternalState.get(),
                                                    stdx::bind(&DataReplicator::_enqueueDocuments,
                                                               this,
                                                               stdx::placeholders::_1,
                                                               stdx::placeholders::_2,
                                                               stdx::placeholders::_3,
                                                               stdx::placeholders::_4),
                                                    stdx::bind(&DataReplicator::_onOplogFetchFinish,
                                                               this,
                                                               stdx::placeholders::_1,
                                                               stdx::placeholders::_2));
    _scheduleFetch_inlock();
    DatabasesCloner* cloner = _initialSyncState->dbsCloner.get();
    lk.unlock();

    if (MONGO_FAIL_POINT(initialSyncHangBeforeCopyingDatabases)) {
        // This log output is used in js tests so please leave it.
        log() << "initial sync - initialSyncHangBeforeCopyingDatabases fail point "
                 "enabled. Blocking until fail point is disabled.";
        while (MONGO_FAIL_POINT(initialSyncHangBeforeCopyingDatabases)) {
            mongo::sleepsecs(1);
        }
    }

    cloner->startup();  // When the cloner is done applier starts.
    _exec->waitForEvent(initialSyncFinishEvent);

    // Check for roll back, and fail if so.
    if (rollbackChecker.hasHadRollback()) {
        lk.lock();
        _initialSyncState->status = {ErrorCodes::UnrecoverableRollbackError,
                                     "Rollback occurred during initial sync"};
        return _initialSyncState->status;
    } else {
        lk.lock();
    }

    if (!_initialSyncState->status.isOK()) {
        return _initialSyncState->status;
    }

    lk.unlock();
    // Store the first oplog entry, after initial sync completes.
    const auto insertStatus =
        _storage->insertDocuments(txn, _opts.localOplogNS, {_initialSyncState->oplogSeedDoc});
    lk.lock();

    if (!insertStatus.isOK()) {
        return insertStatus;
    }

    return Status::OK();  // success
}

StatusWith<OpTimeWithHash> DataReplicator::doInitialSync(OperationContext* txn) {
    Timer t;
    if (!txn) {
        std::string msg = "Initial Sync attempted but no OperationContext*, so aborting.";
        error() << msg;
        return Status{ErrorCodes::InitialSyncFailure, msg};
    }
    UniqueLock lk(_mutex);
    if (_state != DataReplicatorState::Uninitialized) {
        if (_state == DataReplicatorState::InitialSync)
            return {ErrorCodes::InitialSyncActive,
                    (str::stream() << "Initial sync in progress; try resync to start anew.")};
        else {
            return {
                ErrorCodes::AlreadyInitialized,
                (str::stream() << "Cannot do initial sync in " << toString(_state) << " state.")};
        }
    }

    _setState_inlock(DataReplicatorState::InitialSync);

    // TODO: match existing behavior.
    while (true) {
        const auto status = _ensureGoodSyncSource_inlock();
        if (status.isOK()) {
            break;
        }
        LOG(1) << "Error getting sync source: " << status.toString() << ", trying again in 1 sec.";
        sleepsecs(1);
    }

    _oplogBuffer = _dataReplicatorExternalState->makeInitialSyncOplogBuffer(txn);
    _oplogBuffer->startup(txn);
    ON_BLOCK_EXIT([this, txn, &lk]() {
        if (!lk.owns_lock()) {
            lk.lock();
        }
        if (_oplogBuffer) {
            _oplogBuffer->shutdown(txn);
            _oplogBuffer.reset();
        }
        lk.unlock();
    });

    _storage->setInitialSyncFlag(txn);

    const int maxFailedAttempts = kInitialSyncMaxRetries + 1;
    int failedAttempts = 0;
    while (failedAttempts < maxFailedAttempts) {
        Status attemptErrorStatus(Status::OK());
        _initialSyncState.reset();
        _reporterPaused = true;
        _applierPaused = true;

        // The reporter is paused for the duration of the initial sync, so shut down just in case.
        if (_reporter) {
            warning() << "The reporter is running, so stopping it.";
            _reporter->shutdown();
            _reporter.reset();
        }

        if (_applier) {
            warning() << "The applier is running, so stopping it.";
            _applier.reset();
        }

        // For testing, we may want to fail if we receive a getmore.
        if (MONGO_FAIL_POINT(failInitialSyncWithBadHost)) {
            attemptErrorStatus =
                Status(ErrorCodes::InvalidSyncSource,
                       "no sync source avail(failInitialSyncWithBadHost failpoint is set).");
        }

        if (attemptErrorStatus.isOK()) {
            if (_syncSource.empty()) {
                // TODO: Handle no sync source better.
                auto sourceStatus = _ensureGoodSyncSource_inlock();
                if (!sourceStatus.isOK())
                    return sourceStatus;
            }

            RollbackChecker rollbackChecker(_exec, _syncSource);
            attemptErrorStatus =
                _runInitialSyncAttempt_inlock(txn, lk, _syncSource, rollbackChecker);
        }
        if (attemptErrorStatus.isOK()) {
            break;
        }

        ++failedAttempts;

        error() << "Initial sync attempt failed -- attempts left: "
                << (maxFailedAttempts - failedAttempts) << " cause: " << attemptErrorStatus;

        // Reset state.
        if (_oplogFetcher) {
            _oplogFetcher->shutdown();
            std::unique_ptr<OplogFetcher> oplogFetcher;
            _oplogFetcher.swap(oplogFetcher);
            lk.unlock();

            oplogFetcher->join();
            invariant(!oplogFetcher->isActive());

            lk.lock();
        }

        // Check if need to do more retries.
        if (failedAttempts >= maxFailedAttempts) {
            const std::string err =
                "The maximum number of retries"
                " have been exhausted for initial sync.";
            severe() << err;

            _setState_inlock(DataReplicatorState::Uninitialized);
            return Status(ErrorCodes::InitialSyncFailure, err);
        }

        // Sleep for retry time
        lk.unlock();
        sleepmillis(durationCount<Milliseconds>(_opts.initialSyncRetryWait));
        lk.lock();
    }

    // Success, cleanup
    _cancelAllHandles_inlock();
    _waitOnAndResetAll(lk);
    invariant(!_anyActiveHandles_inlock());

    _reporterPaused = false;
    _fetcherPaused = false;
    _applierPaused = false;
    _applierActive = false;
    _initialSyncState.reset();
    _oplogFetcher.reset();
    _lastOplogEntryFetcher.reset();
    _applier.reset();

    _lastFetched = _lastApplied;

    _storage->clearInitialSyncFlag(txn);
    _storage->setMinValid(txn, _lastApplied.opTime, DurableRequirement::Strong);
    log() << "initial sync done; took " << t.millis() << " milliseconds.";
    return _lastApplied;
}

void DataReplicator::_onDataClonerFinish(const Status& status) {
    log() << "data clone finished, status: " << status.toString();
    if (!status.isOK()) {
        // Initial sync failed during cloning of databases
        error() << "Failed to clone data due to '" << status << "'";
        _initialSyncState->status = status;
        _exec->signalEvent(_initialSyncState->finishEvent);
        return;
    }

    BSONObj query = BSON(
        "find" << _opts.remoteOplogNS.coll() << "sort" << BSON("$natural" << -1) << "limit" << 1);

    _lastOplogEntryFetcher = stdx::make_unique<Fetcher>(
        _exec,
        _syncSource,
        _opts.remoteOplogNS.db().toString(),
        query,
        stdx::bind(&DataReplicator::_onApplierReadyStart, this, stdx::placeholders::_1),
        rpc::ServerSelectionMetadata(true, boost::none).toBSON(),
        RemoteCommandRequest::kNoTimeout,
        RemoteCommandRetryScheduler::makeRetryPolicy(
            numFindRetries,
            executor::RemoteCommandRequest::kNoTimeout,
            RemoteCommandRetryScheduler::kAllRetriableErrors));
    Status scheduleStatus = _lastOplogEntryFetcher->schedule();
    if (!scheduleStatus.isOK()) {
        _initialSyncState->status = scheduleStatus;
    }
}

void DataReplicator::_onApplierReadyStart(const QueryResponseStatus& fetchResult) {
    // Data clone done, move onto apply.
    auto&& optimeWithHashStatus = parseOpTimeWithHash(fetchResult);
    if (optimeWithHashStatus.isOK()) {
        LockGuard lk(_mutex);
        auto&& optimeWithHash = optimeWithHashStatus.getValue();
        _initialSyncState->stopTimestamp = optimeWithHash.opTime.getTimestamp();

        // Check if applied to/past our stopTimestamp.
        if (_initialSyncState->beginTimestamp < _initialSyncState->stopTimestamp) {
            invariant(_applierPaused);
            log() << "Applying operations until "
                  << _initialSyncState->stopTimestamp.toStringPretty()
                  << " before initial sync can complete. (starting at "
                  << _initialSyncState->beginTimestamp.toStringPretty() << ")";
            _applierPaused = false;
        } else if (_lastApplied.opTime.getTimestamp() < _initialSyncState->stopTimestamp) {
            log() << "No need to apply operations. (currently at "
                  << _initialSyncState->stopTimestamp.toStringPretty() << ")";
            _lastApplied = optimeWithHash;
        }
        _doNextActions_InitialSync_inlock();
    } else {
        _initialSyncState->status = optimeWithHashStatus.getStatus();
        _doNextActions();
    }
}

bool DataReplicator::_anyActiveHandles_inlock() const {
    return _applierActive || (_oplogFetcher && _oplogFetcher->isActive()) ||
        (_initialSyncState && _initialSyncState->dbsCloner &&
         _initialSyncState->dbsCloner->isActive()) ||
        (_reporter && _reporter->isActive());
}

void DataReplicator::_cancelAllHandles_inlock() {
    if (_oplogFetcher)
        _oplogFetcher->shutdown();
    if (_applier)
        _applier->cancel();
    if (_reporter)
        _reporter->shutdown();
    if (_initialSyncState && _initialSyncState->dbsCloner &&
        _initialSyncState->dbsCloner->isActive()) {
        _initialSyncState->dbsCloner->shutdown();
    }
}

void DataReplicator::_waitOnAndResetAll(UniqueLock& lk) {
    if (_lastOplogEntryFetcher) {
        std::unique_ptr<Fetcher> oldFetcher;
        oldFetcher.swap(_lastOplogEntryFetcher);
        lk.unlock();
        oldFetcher->wait();
        lk.lock();
    }
    if (_oplogFetcher) {
        std::unique_ptr<OplogFetcher> oldFetcher;
        oldFetcher.swap(_oplogFetcher);
        lk.unlock();
        oldFetcher->join();
        lk.lock();
    }
    if (_applier) {
        std::unique_ptr<MultiApplier> oldApplier;
        oldApplier.swap(_applier);
        lk.unlock();
        oldApplier->wait();
        lk.lock();
    }
    if (_reporter) {
        std::unique_ptr<Reporter> oldReporter;
        oldReporter.swap(_reporter);
        lk.unlock();
        oldReporter->join();
        lk.lock();
    }
    if (_initialSyncState) {
        std::unique_ptr<DatabasesCloner> cloner;
        cloner.swap(_initialSyncState->dbsCloner);
        lk.unlock();
        cloner->join();
        lk.lock();
    }
}

void DataReplicator::_doNextActions() {
    // Can be in one of 3 main states/modes (DataReplicatorState):
    // 1.) Initial Sync
    // 2.) Rollback
    // 3.) Steady (Replication)

    // Check for shutdown flag, signal event
    LockGuard lk(_mutex);
    if (_onShutdown.isValid()) {
        if (!_anyActiveHandles_inlock()) {
            _exec->signalEvent(_onShutdown);
            _setState_inlock(DataReplicatorState::Uninitialized);
        }
        return;
    }

    // Do work for the current state
    switch (_state) {
        case DataReplicatorState::Rollback:
            _doNextActions_Rollback_inlock();
            break;
        case DataReplicatorState::InitialSync:
            _doNextActions_InitialSync_inlock();
            break;
        case DataReplicatorState::Steady:
            _doNextActions_Steady_inlock();
            break;
        default:
            return;
    }

    // transition when needed
    _changeStateIfNeeded();
}

void DataReplicator::_doNextActions_InitialSync_inlock() {
    invariant(_initialSyncState);

    if (!_initialSyncState->status.isOK()) {
        return;
    }

    if (_initialSyncState->dbsCloner->isActive() ||
        !_initialSyncState->dbsCloner->getStatus().isOK()) {
        return;
    }

    // The DatabasesCloner has completed so make sure we apply far enough to be consistent.
    const auto lastAppliedTS = _lastApplied.opTime.getTimestamp();
    if (!lastAppliedTS.isNull() && lastAppliedTS >= _initialSyncState->stopTimestamp) {
        invariant(_initialSyncState->finishEvent.isValid());
        invariant(_initialSyncState->status.isOK());
        _setState_inlock(DataReplicatorState::Uninitialized);
        _exec->signalEvent(_initialSyncState->finishEvent);
    } else {
        // Run steady state events to fetch/apply.
        _doNextActions_Steady_inlock();
    }
}

void DataReplicator::_doNextActions_Rollback_inlock() {
    auto s = _ensureGoodSyncSource_inlock();
    if (!s.isOK()) {
        warning() << "Valid sync source unavailable for rollback: " << s;
    }
    _doNextActions_Steady_inlock();
    // TODO: check rollback state and do next actions
    // move from rollback phase to rollback phase via scheduled work in exec
}

void DataReplicator::_doNextActions_Steady_inlock() {
    // Check sync source is still good.
    if (_syncSource.empty()) {
        _syncSource =
            _opts.syncSourceSelector->chooseNewSyncSource(_lastFetched.opTime.getTimestamp());
    }
    if (_syncSource.empty()) {
        // No sync source, reschedule check
        Date_t when = _exec->now() + _opts.syncSourceRetryWait;
        // schedule self-callback w/executor
        // to try to get a new sync source in a bit
        auto scheduleResult = _exec->scheduleWorkAt(when, [this](const CallbackArgs& cbData) {
            if (cbData.status == ErrorCodes::CallbackCanceled) {
                return;
            }
            _doNextActions();
        });
        if (!scheduleResult.isOK()) {
            severe() << "failed to schedule sync source refresh: " << scheduleResult.getStatus()
                     << ". stopping data replicator";
            _setState_inlock(DataReplicatorState::Uninitialized);
            return;
        }
    } else if (!_fetcherPaused) {
        // Check if active fetch, if not start one
        if (!_oplogFetcher || !_oplogFetcher->isActive()) {
            const auto scheduleStatus = _scheduleFetch_inlock();
            if (!scheduleStatus.isOK() && scheduleStatus != ErrorCodes::ShutdownInProgress) {
                error() << "Error scheduling fetcher '" << scheduleStatus << "'.";
                _oplogFetcher.reset();
                _scheduleDoNextActions();
            }
        }
    }

    // Check if no active apply and ops to apply
    if (!_applierActive) {
        if (_oplogBuffer->getSize() > 0) {
            const auto scheduleStatus = _scheduleApplyBatch_inlock();
            if (!scheduleStatus.isOK()) {
                _applierActive = false;
                if (scheduleStatus != ErrorCodes::ShutdownInProgress) {
                    error() << "Error scheduling apply batch '" << scheduleStatus << "'.";
                    _applier.reset();
                    _scheduleDoNextActions();
                }
            }
        } else {
            LOG(3) << "Cannot apply a batch since we have nothing buffered.";
        }
    } else if (_applierActive && !_applier->isActive()) {
        error() << "ERROR: DataReplicator::_applierActive is false but _applier is not active.";
    }

    if (!_reporterPaused && (!_reporter || !_reporter->isActive()) && !_syncSource.empty()) {
        // TODO(benety): Initialize from replica set config election timeout / 2.
        Milliseconds keepAliveInterval(1000);
        _reporter.reset(new Reporter(
            _exec, _opts.prepareReplSetUpdatePositionCommandFn, _syncSource, keepAliveInterval));
    }
}

StatusWith<Operations> DataReplicator::_getNextApplierBatch_inlock() {
    const int slaveDelaySecs = durationCount<Seconds>(_opts.getSlaveDelay());

    size_t totalBytes = 0;
    Operations ops;
    BSONObj op;

    // Return a new batch of ops to apply.
    // A batch may consist of:
    //      * at most "replBatchLimitOperations" OplogEntries
    //      * at most "replBatchLimitBytes" worth of OplogEntries
    //      * only OplogEntries from before the slaveDelay point
    //      * a single command OplogEntry (including index builds, which appear to be inserts)
    //          * consequently, commands bound the previous batch to be in a batch of their own
    auto txn = makeOpCtx();
    while (_oplogBuffer->peek(txn.get(), &op)) {
        auto entry = OplogEntry(std::move(op));

        // Check for ops that must be processed one at a time.
        if (entry.isCommand() ||
            // Index builds are achieved through the use of an insert op, not a command op.
            // The following line is the same as what the insert code uses to detect an index
            // build.
            (entry.hasNamespace() && entry.getCollectionName() == "system.indexes")) {
            if (ops.empty()) {
                // Apply commands one-at-a-time.
                ops.push_back(std::move(entry));
                invariant(_oplogBuffer->tryPop(txn.get(), &op));
                dassert(ops.back().raw == op);
            }

            // Otherwise, apply what we have so far and come back for the command.
            return std::move(ops);
        }

        // Check for oplog version change. If it is absent, its value is one.
        if (entry.getVersion() != OplogEntry::kOplogVersion) {
            std::string message = str::stream()
                << "expected oplog version " << OplogEntry::kOplogVersion << " but found version "
                << entry.getVersion() << " in oplog entry: " << entry.raw;
            severe() << message;
            return {ErrorCodes::BadValue, message};
        }

        // Apply replication batch limits.
        if (ops.size() >= _opts.replBatchLimitOperations) {
            return std::move(ops);
        }
        if (totalBytes + entry.raw.objsize() >= _opts.replBatchLimitBytes) {
            return std::move(ops);
        }

        // Check slaveDelay boundary.
        if (slaveDelaySecs > 0) {
            const unsigned int opTimestampSecs = op["ts"].timestamp().getSecs();
            const unsigned int slaveDelayBoundary =
                static_cast<unsigned int>(time(0) - slaveDelaySecs);

            // Stop the batch as the lastOp is too new to be applied. If we continue
            // on, we can get ops that are way ahead of the delay and this will
            // make this thread sleep longer when handleSlaveDelay is called
            // and apply ops much sooner than we like.
            if (opTimestampSecs > slaveDelayBoundary) {
                return std::move(ops);
            }
        }

        // Add op to buffer.
        ops.push_back(std::move(entry));
        totalBytes += ops.back().raw.objsize();
        invariant(_oplogBuffer->tryPop(txn.get(), &op));
        dassert(ops.back().raw == op);
    }
    return std::move(ops);
}

void DataReplicator::_onApplyBatchFinish(const StatusWith<Timestamp>& ts,
                                         const Operations& ops,
                                         const size_t numApplied) {
    UniqueLock lk(_mutex);
    _applierActive = false;

    if (!ts.isOK()) {
        switch (_state) {
            case DataReplicatorState::InitialSync:
                error() << "Failed to apply batch due to '" << ts.getStatus() << "'";
                _initialSyncState->status = ts.getStatus();
                _exec->signalEvent(_initialSyncState->finishEvent);
                return;
            default:
                fassertFailedWithStatusNoTrace(40190, ts.getStatus());
                break;
        }
    }

    if (_initialSyncState) {
        _initialSyncState->appliedOps += numApplied;
        // When initial sync is done we need to record this seed document in the oplog.
        _initialSyncState->oplogSeedDoc = ops.back().raw.getOwned();
    }

    // TODO: Change OplogFetcher to pass in a OpTimeWithHash, and wire up here instead of parsing.
    const auto lastEntry = ops.back().raw;
    const auto opTimeWithHashStatus = parseOpTimeWithHash(lastEntry);
    _lastApplied = uassertStatusOK(opTimeWithHashStatus);
    lk.unlock();

    _opts.setMyLastOptime(OpTime(ts.getValue(), OpTime::kUninitializedTerm));

    lk.lock();
    if (_reporter) {
        _reporter->trigger();
    }
    lk.unlock();

    _doNextActions();
}

Status DataReplicator::_scheduleDoNextActions() {
    auto status = _exec->scheduleWork([this](const CallbackArgs& cbData) {
        if (cbData.status == ErrorCodes::CallbackCanceled) {
            return;
        }
        _doNextActions();
    });
    return status.getStatus();
}

Status DataReplicator::_scheduleApplyBatch() {
    LockGuard lk(_mutex);
    return _scheduleApplyBatch_inlock();
}

Status DataReplicator::_scheduleApplyBatch_inlock() {
    if (_applierPaused || _applierActive) {
        return Status::OK();
    }

    // If the fail-point is active, delay the apply batch.
    if (MONGO_FAIL_POINT(rsSyncApplyStop)) {
        auto status = _exec->scheduleWorkAt(_exec->now() + Milliseconds(10),
                                            [this](const CallbackArgs& cbData) {
                                                if (cbData.status == ErrorCodes::CallbackCanceled) {
                                                    return;
                                                }
                                                _doNextActions();
                                            });
        return status.getStatus();
    }

    auto batchStatus = _getNextApplierBatch_inlock();
    if (!batchStatus.isOK()) {
        warning() << "Failure creating next apply batch: " << batchStatus.getStatus();
        return batchStatus.getStatus();
    }
    const Operations& ops = batchStatus.getValue();
    if (ops.empty()) {
        _applierActive = false;
        return _scheduleDoNextActions();
    }
    return _scheduleApplyBatch_inlock(ops);
}

Status DataReplicator::_scheduleApplyBatch_inlock(const Operations& ops) {
    MultiApplier::ApplyOperationFn applierFn;
    if (_state == DataReplicatorState::Steady) {
        applierFn = stdx::bind(&DataReplicatorExternalState::_multiSyncApply,
                               _dataReplicatorExternalState.get(),
                               stdx::placeholders::_1);
    } else {
        invariant(_state == DataReplicatorState::InitialSync);
        // "_syncSource" has to be copied to stdx::bind result.
        HostAndPort source = _syncSource;
        applierFn = stdx::bind(&DataReplicatorExternalState::_multiInitialSyncApply,
                               _dataReplicatorExternalState.get(),
                               stdx::placeholders::_1,
                               source);
    }

    auto multiApplyFn = stdx::bind(&DataReplicatorExternalState::_multiApply,
                                   _dataReplicatorExternalState.get(),
                                   stdx::placeholders::_1,
                                   stdx::placeholders::_2,
                                   stdx::placeholders::_3);

    auto lambda = [this](const StatusWith<Timestamp>& ts, const Operations& theOps) {
        if (ErrorCodes::CallbackCanceled == ts) {
            return;
        }

        _onApplyBatchFinish(ts, theOps, theOps.size());
    };

    invariant(!(_applier && _applier->isActive()));
    _applier = stdx::make_unique<MultiApplier>(_exec, ops, applierFn, multiApplyFn, lambda);
    _applierActive = true;
    return _applier->start();
}

Status DataReplicator::_scheduleFetch() {
    LockGuard lk(_mutex);
    return _scheduleFetch_inlock();
}

void DataReplicator::_setState(const DataReplicatorState& newState) {
    LockGuard lk(_mutex);
    _setState_inlock(newState);
}

void DataReplicator::_setState_inlock(const DataReplicatorState& newState) {
    _state = newState;
    _stateCondition.notify_all();
}

Status DataReplicator::_ensureGoodSyncSource_inlock() {
    if (_syncSource.empty()) {
        _syncSource =
            _opts.syncSourceSelector->chooseNewSyncSource(_lastFetched.opTime.getTimestamp());
        if (!_syncSource.empty()) {
            return Status::OK();
        }

        return Status{ErrorCodes::InvalidSyncSource, "No valid sync source."};
    }
    return Status::OK();
}

Status DataReplicator::_scheduleFetch_inlock() {
    if (!_oplogFetcher) {
        if (!_ensureGoodSyncSource_inlock().isOK()) {
            auto status = _scheduleDoNextActions();
            if (!status.isOK()) {
                return status;
            }
        }

        auto startOpTimeWithHash = _lastFetched;
        switch (_state) {
            case DataReplicatorState::InitialSync:
                // Fine to use _lastFetched above.
                break;
            default:
                if (!startOpTimeWithHash.opTime.isNull()) {
                    break;
                }
                // TODO: Read last applied hash from storage. See
                // BackgroundSync::_readLastAppliedHash(OperationContex*).
                long long startHash = 0LL;
                auto&& startOptime = _opts.getMyLastOptime();
                startOpTimeWithHash = OpTimeWithHash{startHash, startOptime};
                break;
        }

        const auto remoteOplogNS = _opts.remoteOplogNS;
        _oplogFetcher = stdx::make_unique<OplogFetcher>(
            _exec,
            startOpTimeWithHash,
            _syncSource,
            remoteOplogNS,
            uassertStatusOK(_dataReplicatorExternalState->getCurrentConfig()),
            _dataReplicatorExternalState.get(),
            stdx::bind(&DataReplicator::_enqueueDocuments,
                       this,
                       stdx::placeholders::_1,
                       stdx::placeholders::_2,
                       stdx::placeholders::_3,
                       stdx::placeholders::_4),
            stdx::bind(&DataReplicator::_onOplogFetchFinish,
                       this,
                       stdx::placeholders::_1,
                       stdx::placeholders::_2));
    }
    if (!_oplogFetcher->isActive()) {
        LOG(2) << "Starting OplogFetcher: " << _oplogFetcher->toString();
        Status status = _oplogFetcher->startup();
        if (!status.isOK()) {
            return status;
        }
    }
    return Status::OK();
}

Status DataReplicator::_scheduleReport() {
    // TODO
    return Status::OK();
}

void DataReplicator::_changeStateIfNeeded() {
    // TODO
}

Status DataReplicator::scheduleShutdown(OperationContext* txn) {
    auto eventStatus = _exec->makeEvent();
    if (!eventStatus.isOK()) {
        return eventStatus.getStatus();
    }

    {
        LockGuard lk(_mutex);
        invariant(!_onShutdown.isValid());
        _onShutdown = eventStatus.getValue();
        _cancelAllHandles_inlock();
        _oplogBuffer->shutdown(txn);
        _oplogBuffer.reset();
    }

    // Schedule _doNextActions in case nothing is active to trigger the _onShutdown event.
    return _scheduleDoNextActions();
}

void DataReplicator::waitForShutdown() {
    Event onShutdown;
    {
        LockGuard lk(_mutex);
        invariant(_onShutdown.isValid());
        onShutdown = _onShutdown;
    }
    _exec->waitForEvent(onShutdown);
    {
        LockGuard lk(_mutex);
        invariant(!_oplogFetcher->isActive());
        invariant(!_applierActive);
        invariant(!_reporter->isActive());
    }
}

Status DataReplicator::_shutdown(OperationContext* txn) {
    auto status = scheduleShutdown(txn);
    if (status.isOK()) {
        waitForShutdown();
    }
    return status;
}

void DataReplicator::_enqueueDocuments(Fetcher::Documents::const_iterator begin,
                                       Fetcher::Documents::const_iterator end,
                                       const OplogFetcher::DocumentsInfo& info,
                                       Milliseconds getMoreElapsed) {
    if (info.toApplyDocumentCount == 0) {
        return;
    }

    // TODO(SH): Remove this once we fix waiting for the OplogFetcher before completing
    // InitSync.
    //           In "Complete" test this is called after Initial Sync completes sometimes.
    if (!_oplogBuffer) {
        error() << "No _oplogBuffer to add documents to; throwing away the batch.";
        return;
    }

    // Wait for enough space.
    // Gets unblocked on shutdown.
    _oplogBuffer->waitForSpace(makeOpCtx().get(), info.toApplyDocumentBytes);

    OCCASIONALLY {
        LOG(2) << "bgsync buffer has " << _oplogBuffer->getSize() << " bytes";
    }

    // Buffer docs for later application.
    fassert(40143, _oplogBuffer->pushAllNonBlocking(makeOpCtx().get(), begin, end));

    _lastFetched = info.lastDocument;

    // TODO: updates metrics with "info" and "getMoreElapsed".

    _doNextActions();
}

void DataReplicator::_onOplogFetchFinish(const Status& status, const OpTimeWithHash& lastFetched) {
    if (status.code() == ErrorCodes::CallbackCanceled) {
        return;
    } else if (status.isOK()) {
        LockGuard lk(_mutex);
        _lastFetched = lastFetched;
    } else {
        invariant(!status.isOK());
        // Got an error, now decide what to do...
        switch (status.code()) {
            case ErrorCodes::OplogStartMissing:
            case ErrorCodes::RemoteOplogStale: {
                LockGuard lk(_mutex);
                if (_state == DataReplicatorState::InitialSync) {
                    // Do not do rollback, just log.
                    error() << "Error fetching oplog during initial sync: " << status;
                    if (_initialSyncState) {
                        _initialSyncState->status = status;
                    }
                    break;
                }
                _setState_inlock(DataReplicatorState::Rollback);
                // possible rollback
                auto scheduleResult = _exec->scheduleWork(
                    stdx::bind(&DataReplicator::_rollbackOperations, this, stdx::placeholders::_1));
                if (!scheduleResult.isOK()) {
                    error() << "Failed to schedule rollback work: " << scheduleResult.getStatus();
                    _setState_inlock(DataReplicatorState::Uninitialized);
                    return;
                }
                _applierPaused = true;
                _fetcherPaused = true;
                _reporterPaused = true;
                break;
            }
            case ErrorCodes::OplogOutOfOrder: {
                // TODO: Remove this once we fix the oplog fetcher code causing the problem.
                break;
            }
            default: {
                Date_t until{_exec->now() +
                             _opts.blacklistSyncSourcePenaltyForNetworkConnectionError};
                _opts.syncSourceSelector->blacklistSyncSource(_syncSource, until);
                LockGuard lk(_mutex);
                _syncSource = HostAndPort();
            }
        }
    }

    _doNextActions();
}

void DataReplicator::_rollbackOperations(const CallbackArgs& cbData) {
    if (cbData.status.code() == ErrorCodes::CallbackCanceled) {
        return;
    }

    auto lastOpTimeWritten = getLastApplied();
    HostAndPort syncSource = getSyncSource();
    auto rollbackStatus = _opts.rollbackFn(makeOpCtx().get(), lastOpTimeWritten.opTime, syncSource);
    if (!rollbackStatus.isOK()) {
        error() << "Failed rollback: " << rollbackStatus;
        Date_t until{_exec->now() + _opts.blacklistSyncSourcePenaltyForOplogStartMissing};
        _opts.syncSourceSelector->blacklistSyncSource(_syncSource, until);
        LockGuard lk(_mutex);
        _syncSource = HostAndPort();
        _oplogFetcher.reset();
        _fetcherPaused = false;
    } else {
        // Go back to steady sync after a successful rollback.
        auto s = _opts.setFollowerMode(MemberState::RS_SECONDARY);
        if (!s) {
            error() << "Failed to transition to SECONDARY after rolling back from sync source: "
                    << _syncSource.toString();
        }
        // TODO: cleanup state/restart -- set _lastApplied, and other stuff
        LockGuard lk(_mutex);
        _applierPaused = false;
        _fetcherPaused = false;
        _reporterPaused = false;
        _setState_inlock(DataReplicatorState::Steady);
    }

    _doNextActions();
};

}  // namespace repl
}  // namespace mongo
