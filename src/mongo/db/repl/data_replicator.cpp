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
#include <boost/thread.hpp>

#include "mongo/base/status.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/namespace_string.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {

    using CBHStatus = StatusWith<ReplicationExecutor::CallbackHandle>;
    using CallbackFn = ReplicationExecutor::CallbackFn;
    using Request = ReplicationExecutor::RemoteCommandRequest;
    using Response = ReplicationExecutor::RemoteCommandResponse;
    using CommandCallbackData = ReplicationExecutor::RemoteCommandCallbackData;
//    typedef void (*run_func)();

    // Failpoint for initial sync
    MONGO_FP_DECLARE(failInitialSyncWithBadHost);

    namespace {
        int NoSyncSourceRetryDelayMS = 4000;

        std::string toString(DataReplicatiorState s) {
            switch (s) {
                case DataReplicatiorState::InitialSync:
                    return "InitialSync";
                case DataReplicatiorState::Rollback:
                    return "Rollback";
                case DataReplicatiorState::Steady:
                    return "Steady Replication";
                case DataReplicatiorState::Uninitialized:
                    return "Uninitialized";
                default:
                    return "<invalid>";
            }
        }

    /*
        Status callViaExecutor(ReplicationExecutor* exec, const CallbackFn& work) {
            CBHStatus cbh = exec->scheduleWork(work);
            if (!cbh.getStatus().isOK()) {
                return cbh.getStatus();
            }

            exec->wait(cbh.getValue());

            return Status::OK();
        }
    */

        Timestamp findCommonPoint(HostAndPort host, Timestamp start) {
            // TODO: walk back in the oplog looking for a known/shared optime.
            return Timestamp();
        }

        bool _didRollback(HostAndPort host) {
            // TODO: rollback here, report if done.
            return false;
        }
    } // namespace

    Status InitialSyncImpl::start() {
        // For testing, we may want to fail if we receive a getmore.
        if (MONGO_FAIL_POINT(failInitialSyncWithBadHost)) {
            _status = StatusWith<Timestamp>(ErrorCodes::InvalidSyncSource, "no sync source avail.");
        }
        else {
            _status = Status::OK();
        }
        return _status.getStatus();
    }

    DataReplicator::DataReplicator(DataReplicatorOptions opts,
                                   ReplicationExecutor* exec,
                                   ReplicationCoordinator* replCoord)
                            : _opts(opts),
                              _exec(exec),
                              _replCoord(replCoord),
                              _state(DataReplicatiorState::Uninitialized) {

    }

    DataReplicator::DataReplicator(DataReplicatorOptions opts,
                                   ReplicationExecutor* exec)
                            : _opts(opts),
                              _exec(exec),
                              _state(DataReplicatiorState::Uninitialized) {
    }
/*
    Status DataReplicator::_run(run_func what) {
        return callViaExecutor(_exec,
                               stdx::bind(what,
                                          this,
                                          stdx::placeholders::_1));
    }
*/
    Status DataReplicator::start() {
        boost::unique_lock<boost::mutex> lk(_mutex);
        if (_state != DataReplicatiorState::Uninitialized) {
            // Error.
        }
        _state = DataReplicatiorState::Steady;

        if (_replCoord) {
            // TODO: Use chooseNewSyncSource? -this requires an active replCoord so not working in tests
            _fetcher.reset(new Fetcher(_exec,
                                       HostAndPort(), //_replCoord->chooseNewSyncSource(),
                                       _opts.remoteOplogNS.db().toString(),
                                       BSON("find" << _opts.remoteOplogNS),
                                       stdx::bind(&DataReplicator::_onFetchFinish,
                                                  this,
                                                  stdx::placeholders::_1,
                                                  stdx::placeholders::_2)));
        }
        else {
            _fetcher.reset(new Fetcher(_exec,
                                       _opts.syncSource,
                                       _opts.remoteOplogNS.db().toString(),
                                       BSON("find" << _opts.remoteOplogNS),
                                       stdx::bind(&DataReplicator::_onFetchFinish,
                                                  this,
                                                  stdx::placeholders::_1,
                                                  stdx::placeholders::_2)));
        }


        // TODO

        return Status::OK();
    }

    Status DataReplicator::shutdown() {
        return _shutdown();
    }

    Status DataReplicator::pause() {
        // TODO
        return Status::OK();
    }

    Status DataReplicator::resume(bool wait) {
        StatusWith<Handle> handle = _exec->scheduleWork(stdx::bind(&DataReplicator::_resumeFinish,
                                                                   this,
                                                                   stdx::placeholders::_1));
        const Status status = handle.getStatus();
        if (wait && status.isOK()) {
            _exec->wait(handle.getValue());
        }
        return status;
    }

    void DataReplicator::_resumeFinish(CallbackData cbData) {
        _fetcherPaused = _applierPaused = false;
        _doNextActions();
    }

    void DataReplicator::_pauseApplier() {
        _applierPaused = true;
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
        boost::unique_lock<boost::mutex> lk(_mutex);
        if (_applierActive) {
            lk.unlock();
            _exec->wait(_applierHandle);
            lk.lock();
        }
        return StatusWith<Timestamp>(_lastOptimeApplied);
    }

    void DataReplicator::_resetState(Timestamp lastAppliedOptime) {
        boost::lock_guard<boost::mutex> lk(_mutex);
        _lastOptimeApplied = _lastOptimeFetched = lastAppliedOptime;
    }

    void DataReplicator::slavesHaveProgressed() {
        if (_reporter) {
            _reporter->trigger();
        }
    }

    StatusWith<Timestamp> DataReplicator::resync() {
        _shutdown();
        // Drop databases and do initialSync();
        // TODO drop database
        StatusWith<Timestamp> status = initialSync();
        if (status.isOK()) {
            _resetState(status.getValue());
        }
        return status;
    }

    StatusWith<Timestamp> DataReplicator::initialSync() {
        boost::lock_guard<boost::mutex> lk(_mutex);
        if (_state != DataReplicatiorState::Uninitialized) {
            if (_state == DataReplicatiorState::InitialSync)
                return StatusWith<Timestamp>(
                                        ErrorCodes::InvalidRoleModification,
                                        (str::stream() << "Already doing initial sync;try resync"));
            else {
                return StatusWith<Timestamp>(
                                        ErrorCodes::AlreadyInitialized,
                                        (str::stream() << "Cannot do initial sync in "
                                                       << toString(_state)));
            }
        }

        _state = DataReplicatiorState::InitialSync;
        const int maxFailedAttempts = 10;
        int failedAttempts = 0;
        while (failedAttempts < maxFailedAttempts) {
            _initialSync.reset(new InitialSyncImpl(_exec));
            _initialSync->start();
            _initialSync->wait();
            Status s = _initialSync->getStatus().getStatus();

            if (s.isOK()) {
                // we are done :)
                break;
            }

            ++failedAttempts;

            error() << "Initial sync attempt failed -- attempts left: "
                    << (maxFailedAttempts - failedAttempts) << " cause: "
                    << s;
            // TODO: uncomment
            //sleepsecs(5);

            // No need to print a stack
            if (failedAttempts >= maxFailedAttempts) {
                const std::string err = "The maximum number of retries"
                                       " have been exhausted for initial sync.";
                severe() << err;
                return Status(ErrorCodes::InitialSyncFailure, err);
            }
        }
        return _initialSync->getStatus();
    }

    const bool DataReplicator::_anyActiveHandles() {
        boost::lock_guard<boost::mutex> lk(_mutex);
        return _fetcher->isActive() || _applierActive || _reporter->isActive();
    }

    const void DataReplicator::_cancelAllHandles() {
        boost::lock_guard<boost::mutex> lk(_mutex);
        _fetcher->cancel();
        _exec->cancel(_applierHandle);
        _reporter->cancel();
    }

    void DataReplicator::_doNextActionsCB(CallbackData cbData) {
        _doNextActions();
    }

    void DataReplicator::_doNextActions() {
        // Can be in one of 3 main states/modes (DataReplicatiorState):
        // 1.) Initial Sync
        // 2.) Rollback
        // 3.) Steady (Replication)

        // Check for shutdown flag, signal event
        boost::lock_guard<boost::mutex> lk(_mutex);
        if (_doShutdown) {
            if(!_anyActiveHandles()) {
                _exec->signalEvent(_onShutdown);
            }
            return;
        }

        // Do work for the current state
        switch (_state) {
            case DataReplicatiorState::Rollback:
                _doNextActions_Rollback_inlock();
                break;
            case DataReplicatiorState::InitialSync:
                _doNextActions_InitialSync_inlock();
                break;
            case DataReplicatiorState::Steady:
                _doNextActions_Steady_inlock();
                break;
            default:
                return;
        }

        // transition when needed
        _changeStateIfNeeded();
    }

    void DataReplicator::_doNextActions_InitialSync_inlock() {
        // TODO: check initial sync state and do next actions
        // move from initial sync phase to initial sync phase via scheduled work in exec

        if (!_initialSync) {
            // Error case?, reset to uninit'd
            _state = DataReplicatiorState::Uninitialized;
            return;
        }

        if (!_initialSync->isActive()) {
            if (!_initialSync->getStatus().isOK()) {
                // TODO: Initial sync failed
            }
            else {
                // TODO: success
            }
        }
    }

    void DataReplicator::_doNextActions_Rollback_inlock() {
        // TODO: check rollback state and do next actions
        // move from rollback phase to rollback phase via scheduled work in exec
    }

    void DataReplicator::_doNextActions_Steady_inlock() {
        // Check sync source is still good.
        if (_syncSource.empty()) {
            _syncSource = _replCoord->chooseNewSyncSource();
        }
        if (_syncSource.empty()) {
            // No sync source, reschedule check
            Date_t when = _exec->now() + NoSyncSourceRetryDelayMS;
            // schedule self-callback w/executor
            _exec->scheduleWorkAt(when, // to try to get a new sync source in a bit
                                  stdx::bind(&DataReplicator::_doNextActionsCB,
                                             this,
                                             stdx::placeholders::_1));
        } else {
            // Check if active fetch, if not start one
            if (!_fetcher->isActive()) {
                _scheduleFetch();
            }
        }

        // Check if active apply, if not start one
        if (!_applierActive) {
            _scheduleApplyBatch();
        }

        if (!_reporter || !_reporter->previousReturnStatus().isOK()) {
            // TODO get reporter in good shape
            _reporter.reset(new Reporter(_exec, _replCoord, HostAndPort()));
        }
    }

    void DataReplicator::_onApplyBatchFinish(CallbackData cbData) {
        _reporter->trigger();
        // TODO
    }

    Status DataReplicator::_scheduleApplyBatch() {
        boost::lock_guard<boost::mutex> lk(_mutex);
        if (!_applierActive) {
            // TODO
            _applierActive = true;
            auto status =  _exec->scheduleWork(
                                stdx::bind(&DataReplicator::_onApplyBatchFinish,
                                           this,
                                           stdx::placeholders::_1));
            if (!status.isOK()) {
                return status.getStatus();
            }

            _applierHandle = status.getValue();
        }
        return Status::OK();
    }

    Status DataReplicator::_scheduleFetch() {
        boost::lock_guard<boost::mutex> lk(_mutex);
        if (!_fetcher->isActive()) {
            // TODO
            Status status = _fetcher->schedule();
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

    Status DataReplicator::_shutdown() {
        StatusWith<Event> eventStatus = _exec->makeEvent();
        if (!eventStatus.isOK()) return eventStatus.getStatus();
        boost::unique_lock<boost::mutex> lk(_mutex);
        _onShutdown = eventStatus.getValue();
        lk.unlock();

        _cancelAllHandles();

        lk.lock();
        _doShutdown = true;
        lk.unlock();

        // Schedule _doNextActions in case nothing is active to trigger the _onShutdown event.
        StatusWith<Handle> statusHandle = _exec->scheduleWork(
                                                stdx::bind(&DataReplicator::_doNextActionsCB,
                                                           this,
                                                           stdx::placeholders::_1));

        if (statusHandle.isOK()) {
            _exec->waitForEvent(_onShutdown);
        } else {
            return statusHandle.getStatus();
        }

        invariant(!_fetcher->isActive());
        invariant(!_applierActive);
        invariant(!_reporter->isActive());
        return Status::OK();
    }

    bool DataReplicator::_needToRollback(HostAndPort source, Timestamp lastApplied) {
        if ((_rollbackCommonOptime = findCommonPoint(source, lastApplied)).isNull()) {
            return false;
        } else {
            return true;
        }
    }

    void DataReplicator::_onFetchFinish(const StatusWith<Fetcher::BatchData>& fetchResult,
                                        Fetcher::NextAction* nextAction) {
        const Status status = fetchResult.getStatus();
        if (status.code() == ErrorCodes::CallbackCanceled)
            return;

        if (status.isOK()) {
            const auto docs = fetchResult.getValue().documents;
            _oplogBuffer.insert(_oplogBuffer.end(), docs.begin(), docs.end());
            if (*nextAction == Fetcher::NextAction::kNoAction) {
                // TODO: create new fetcher?, with new query from where we left off
            }
        }
        else {
            // Error, decide what to do...

            if (status.code() == ErrorCodes::InvalidSyncSource) {
                // Error, sync source
                Date_t until = 0;
                _replCoord->blacklistSyncSource(_syncSource, until);
                _syncSource = HostAndPort();
            }

            if (status.code() == ErrorCodes::OplogStartMissing) {
                // possible rollback
                bool didRollback = _didRollback(_syncSource);
                if (!didRollback) {
                    _replCoord->setFollowerMode(MemberState::RS_RECOVERING); // TODO too stale
                }
                else {
                    // TODO: cleanup state/restart -- set _lastApplied, and other stuff
                }
            }
        }

        _doNextActions();
    }
} // namespace repl
} // namespace mongo

