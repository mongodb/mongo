/**
 *    Copyright 2016 MongoDB Inc.
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

#include "mongo/db/repl/sync_source_resolver.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/sync_source_selector.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/destructor_guard.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {

const NamespaceString SyncSourceResolver::kLocalOplogNss("local.oplog.rs");
const Seconds SyncSourceResolver::kFetcherTimeout(30);
const Seconds SyncSourceResolver::kFetcherErrorBlacklistDuration(10);
const Seconds SyncSourceResolver::kOplogEmptyBlacklistDuration(10);
const Seconds SyncSourceResolver::kFirstOplogEntryEmptyBlacklistDuration(10);
const Seconds SyncSourceResolver::kFirstOplogEntryNullTimestampBlacklistDuration(10);
const Minutes SyncSourceResolver::kTooStaleBlacklistDuration(1);
const Seconds SyncSourceResolver::kNoRequiredOpTimeBlacklistDuration(60);

SyncSourceResolver::SyncSourceResolver(executor::TaskExecutor* taskExecutor,
                                       SyncSourceSelector* syncSourceSelector,
                                       const OpTime& lastOpTimeFetched,
                                       const OpTime& requiredOpTime,
                                       const OnCompletionFn& onCompletion)
    : _taskExecutor(taskExecutor),
      _syncSourceSelector(syncSourceSelector),
      _lastOpTimeFetched(lastOpTimeFetched),
      _requiredOpTime(requiredOpTime),
      _onCompletion(onCompletion) {
    uassert(ErrorCodes::BadValue, "task executor cannot be null", taskExecutor);
    uassert(ErrorCodes::BadValue, "sync source selector cannot be null", syncSourceSelector);
    uassert(
        ErrorCodes::BadValue, "last fetched optime cannot be null", !lastOpTimeFetched.isNull());
    uassert(ErrorCodes::BadValue,
            str::stream() << "required optime (if provided) must be more recent than last "
                             "fetched optime. requiredOpTime: "
                          << requiredOpTime.toString()
                          << ", lastOpTimeFetched: "
                          << lastOpTimeFetched.toString(),
            requiredOpTime.isNull() || requiredOpTime > lastOpTimeFetched);
    uassert(ErrorCodes::BadValue, "callback function cannot be null", onCompletion);
}

SyncSourceResolver::~SyncSourceResolver() {
    DESTRUCTOR_GUARD(shutdown(); join(););
}

bool SyncSourceResolver::isActive() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _isActive_inlock();
}

bool SyncSourceResolver::_isActive_inlock() const {
    return State::kRunning == _state || State::kShuttingDown == _state;
}

Status SyncSourceResolver::startup() {
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        switch (_state) {
            case State::kPreStart:
                _state = State::kRunning;
                break;
            case State::kRunning:
                return Status(ErrorCodes::IllegalOperation, "sync source resolver already started");
            case State::kShuttingDown:
                return Status(ErrorCodes::ShutdownInProgress, "sync source resolver shutting down");
            case State::kComplete:
                return Status(ErrorCodes::ShutdownInProgress, "sync source resolver completed");
        }
    }

    return _chooseAndProbeNextSyncSource(OpTime());
}

void SyncSourceResolver::shutdown() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    // Transition directly from PreStart to Complete if not started yet.
    if (State::kPreStart == _state) {
        _state = State::kComplete;
        return;
    }

    // Nothing to do if we are already in ShuttingDown or Complete state.
    if (State::kShuttingDown == _state || State::kComplete == _state) {
        return;
    }

    invariant(_state == State::kRunning);
    _state = State::kShuttingDown;

    if (_fetcher) {
        _fetcher->shutdown();
    }
    if (_rbidCommandHandle) {
        _taskExecutor->cancel(_rbidCommandHandle);
    }
}

void SyncSourceResolver::join() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _condition.wait(lk, [this]() { return !_isActive_inlock(); });
}

bool SyncSourceResolver::_isShuttingDown() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return State::kShuttingDown == _state;
}

StatusWith<HostAndPort> SyncSourceResolver::_chooseNewSyncSource() {
    HostAndPort candidate;
    try {
        candidate = _syncSourceSelector->chooseNewSyncSource(_lastOpTimeFetched);
    } catch (...) {
        return exceptionToStatus();
    }

    if (_isShuttingDown()) {
        return Status(ErrorCodes::CallbackCanceled,
                      str::stream() << "sync source resolver shut down before probing candidate: "
                                    << candidate);
    }

    return candidate;
}

std::unique_ptr<Fetcher> SyncSourceResolver::_makeFirstOplogEntryFetcher(
    HostAndPort candidate, OpTime earliestOpTimeSeen) {
    return stdx::make_unique<Fetcher>(
        _taskExecutor,
        candidate,
        kLocalOplogNss.db().toString(),
        BSON("find" << kLocalOplogNss.coll() << "limit" << 1 << "sort" << BSON("$natural" << 1)
                    << "projection"
                    << BSON(OplogEntryBase::kTimestampFieldName << 1
                                                                << OplogEntryBase::kTermFieldName
                                                                << 1)),
        stdx::bind(&SyncSourceResolver::_firstOplogEntryFetcherCallback,
                   this,
                   stdx::placeholders::_1,
                   candidate,
                   earliestOpTimeSeen),
        ReadPreferenceSetting::secondaryPreferredMetadata(),
        kFetcherTimeout /* find network timeout */,
        kFetcherTimeout /* getMore network timeout */);
}

std::unique_ptr<Fetcher> SyncSourceResolver::_makeRequiredOpTimeFetcher(HostAndPort candidate,
                                                                        OpTime earliestOpTimeSeen,
                                                                        int rbid) {
    // This query is structured so that it is executed on the sync source using the oplog
    // start hack (oplogReplay=true and $gt/$gte predicate over "ts").
    return stdx::make_unique<Fetcher>(
        _taskExecutor,
        candidate,
        kLocalOplogNss.db().toString(),
        BSON("find" << kLocalOplogNss.coll() << "oplogReplay" << true << "filter"
                    << BSON("ts" << BSON("$gte" << _requiredOpTime.getTimestamp() << "$lte"
                                                << _requiredOpTime.getTimestamp()))),
        stdx::bind(&SyncSourceResolver::_requiredOpTimeFetcherCallback,
                   this,
                   stdx::placeholders::_1,
                   candidate,
                   earliestOpTimeSeen,
                   rbid),
        ReadPreferenceSetting::secondaryPreferredMetadata(),
        kFetcherTimeout /* find network timeout */,
        kFetcherTimeout /* getMore network timeout */);
}

Status SyncSourceResolver::_scheduleFetcher(std::unique_ptr<Fetcher> fetcher) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    // TODO SERVER-27499 need to check if _state is kShuttingDown inside the mutex.
    // Must schedule fetcher inside lock in case fetcher's callback gets invoked immediately by task
    // executor.
    auto status = fetcher->schedule();
    if (status.isOK()) {
        // Fetcher destruction blocks on all outstanding callbacks. If we are currently in a
        // Fetcher-related callback, we can't destroy the Fetcher just yet, so we assign it to a
        // temporary unique pointer to allow the destruction to run to completion.
        _shuttingDownFetcher = std::move(_fetcher);
        _fetcher = std::move(fetcher);
    } else {
        error() << "Error scheduling fetcher to evaluate host as sync source, host:"
                << fetcher->getSource() << ", error: " << status;
    }
    return status;
}

OpTime SyncSourceResolver::_parseRemoteEarliestOpTime(const HostAndPort& candidate,
                                                      const Fetcher::QueryResponse& queryResponse) {
    if (queryResponse.documents.empty()) {
        // Remote oplog is empty.
        const auto until = _taskExecutor->now() + kOplogEmptyBlacklistDuration;
        log() << "Blacklisting " << candidate << " due to empty oplog for "
              << kOplogEmptyBlacklistDuration << " until: " << until;
        _syncSourceSelector->blacklistSyncSource(candidate, until);
        return OpTime();
    }

    const auto& firstObjFound = queryResponse.documents.front();
    if (firstObjFound.isEmpty()) {
        // First document in remote oplog is empty.
        const auto until = _taskExecutor->now() + kFirstOplogEntryEmptyBlacklistDuration;
        log() << "Blacklisting " << candidate << " due to empty first document for "
              << kFirstOplogEntryEmptyBlacklistDuration << " until: " << until;
        _syncSourceSelector->blacklistSyncSource(candidate, until);
        return OpTime();
    }

    const auto remoteEarliestOpTime = OpTime::parseFromOplogEntry(firstObjFound);
    if (!remoteEarliestOpTime.isOK()) {
        const auto until = _taskExecutor->now() + kFirstOplogEntryNullTimestampBlacklistDuration;
        log() << "Blacklisting " << candidate << " due to error parsing OpTime from the oldest"
              << " oplog entry for " << kFirstOplogEntryNullTimestampBlacklistDuration
              << " until: " << until << ". Error: " << remoteEarliestOpTime.getStatus()
              << ", Entry: " << redact(firstObjFound);
        _syncSourceSelector->blacklistSyncSource(candidate, until);
        return OpTime();
    }

    if (remoteEarliestOpTime.getValue().isNull()) {
        // First document in remote oplog is empty.
        const auto until = _taskExecutor->now() + kFirstOplogEntryNullTimestampBlacklistDuration;
        log() << "Blacklisting " << candidate << " due to null timestamp in first document for "
              << kFirstOplogEntryNullTimestampBlacklistDuration << " until: " << until;
        _syncSourceSelector->blacklistSyncSource(candidate, until);
        return OpTime();
    }

    return remoteEarliestOpTime.getValue();
}

void SyncSourceResolver::_firstOplogEntryFetcherCallback(
    const StatusWith<Fetcher::QueryResponse>& queryResult,
    HostAndPort candidate,
    OpTime earliestOpTimeSeen) {
    if (_isShuttingDown()) {
        _finishCallback(Status(ErrorCodes::CallbackCanceled,
                               str::stream()
                                   << "sync source resolver shut down while probing candidate: "
                                   << candidate))
            .transitional_ignore();
        return;
    }

    if (ErrorCodes::CallbackCanceled == queryResult.getStatus()) {
        _finishCallback(queryResult.getStatus()).transitional_ignore();
        return;
    }

    if (!queryResult.isOK()) {
        // We got an error.
        const auto until = _taskExecutor->now() + kFetcherErrorBlacklistDuration;
        log() << "Blacklisting " << candidate << " due to error: '" << queryResult.getStatus()
              << "' for " << kFetcherErrorBlacklistDuration << " until: " << until;
        _syncSourceSelector->blacklistSyncSource(candidate, until);

        _chooseAndProbeNextSyncSource(earliestOpTimeSeen).transitional_ignore();
        return;
    }

    const auto& queryResponse = queryResult.getValue();
    const auto remoteEarliestOpTime = _parseRemoteEarliestOpTime(candidate, queryResponse);
    if (remoteEarliestOpTime.isNull()) {
        _chooseAndProbeNextSyncSource(earliestOpTimeSeen).transitional_ignore();
        return;
    }

    // remoteEarliestOpTime may come from a very old config, so we cannot compare their terms.
    if (_lastOpTimeFetched.getTimestamp() < remoteEarliestOpTime.getTimestamp()) {
        // We're too stale to use this sync source.
        const auto blacklistDuration = kTooStaleBlacklistDuration;
        const auto until = _taskExecutor->now() + Minutes(1);

        log() << "We are too stale to use " << candidate << " as a sync source. "
              << "Blacklisting this sync source"
              << " because our last fetched timestamp: " << _lastOpTimeFetched.getTimestamp()
              << " is before their earliest timestamp: " << remoteEarliestOpTime.getTimestamp()
              << " for " << blacklistDuration << " until: " << until;

        _syncSourceSelector->blacklistSyncSource(candidate, until);

        // If all the viable sync sources are too far ahead of us (i.e. we are "too stale" relative
        // each sync source), we will want to return the starting timestamp of the sync source
        // candidate that is closest to us. See SyncSourceResolverResponse::earliestOpTimeSeen.
        // We use "earliestOpTimeSeen" to keep track of the current minimum starting timestamp.
        if (earliestOpTimeSeen.isNull() ||
            earliestOpTimeSeen.getTimestamp() > remoteEarliestOpTime.getTimestamp()) {
            earliestOpTimeSeen = remoteEarliestOpTime;
        }

        _chooseAndProbeNextSyncSource(earliestOpTimeSeen).transitional_ignore();
        return;
    }

    auto status = _scheduleRBIDRequest(candidate, earliestOpTimeSeen);
    if (!status.isOK()) {
        _finishCallback(status).ignore();
    }
}

Status SyncSourceResolver::_scheduleRBIDRequest(HostAndPort candidate, OpTime earliestOpTimeSeen) {
    // Once a work is scheduled, nothing prevents it finishing. We need the mutex to protect the
    // access of member variables after scheduling, because otherwise the scheduled callback could
    // finish and allow the destructor to fire before we access the member variables.
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_state == State::kShuttingDown) {
        return Status(
            ErrorCodes::CallbackCanceled,
            str::stream()
                << "sync source resolver shut down while checking rollbackId on candidate: "
                << candidate);
    }

    invariant(_state == State::kRunning);
    auto handle = _taskExecutor->scheduleRemoteCommand(
        {candidate, "admin", BSON("replSetGetRBID" << 1), nullptr, kFetcherTimeout},
        stdx::bind(&SyncSourceResolver::_rbidRequestCallback,
                   this,
                   candidate,
                   earliestOpTimeSeen,
                   stdx::placeholders::_1));

    if (!handle.isOK()) {
        return handle.getStatus();
    }

    _rbidCommandHandle = std::move(handle.getValue());
    return Status::OK();
}

void SyncSourceResolver::_rbidRequestCallback(
    HostAndPort candidate,
    OpTime earliestOpTimeSeen,
    const executor::TaskExecutor::RemoteCommandCallbackArgs& rbidReply) {
    if (rbidReply.response.status == ErrorCodes::CallbackCanceled) {
        _finishCallback(rbidReply.response.status).transitional_ignore();
        return;
    }

    int rbid = ReplicationProcess::kUninitializedRollbackId;
    try {
        uassertStatusOK(rbidReply.response.status);
        uassertStatusOK(getStatusFromCommandResult(rbidReply.response.data));
        rbid = rbidReply.response.data["rbid"].Int();
    } catch (const DBException& ex) {
        const auto until = _taskExecutor->now() + kFetcherErrorBlacklistDuration;
        log() << "Blacklisting " << candidate << " due to error: '" << ex << "' for "
              << kFetcherErrorBlacklistDuration << " until: " << until;
        _syncSourceSelector->blacklistSyncSource(candidate, until);
        _chooseAndProbeNextSyncSource(earliestOpTimeSeen).transitional_ignore();
        return;
    }

    if (!_requiredOpTime.isNull()) {
        // Schedule fetcher to look for '_requiredOpTime' in the remote oplog.
        // Unittest requires that this kind of failure be handled specially.
        auto status =
            _scheduleFetcher(_makeRequiredOpTimeFetcher(candidate, earliestOpTimeSeen, rbid));
        if (!status.isOK()) {
            _finishCallback(status).transitional_ignore();
        }
        return;
    }

    _finishCallback(candidate, rbid).ignore();
}

Status SyncSourceResolver::_compareRequiredOpTimeWithQueryResponse(
    const Fetcher::QueryResponse& queryResponse) {
    if (queryResponse.documents.empty()) {
        return Status(
            ErrorCodes::NoMatchingDocument,
            "remote oplog does not contain entry with optime matching our required optime");
    }
    const OplogEntry oplogEntry(queryResponse.documents.front());
    const auto opTime = oplogEntry.getOpTime();
    if (_requiredOpTime != opTime) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "remote oplog contain entry with matching timestamp "
                                    << opTime.getTimestamp().toString()
                                    << " but optime "
                                    << opTime.toString()
                                    << " does not "
                                       "match our required optime");
    }
    if (_requiredOpTime.getTerm() != opTime.getTerm()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "remote oplog contain entry with term " << opTime.getTerm()
                                    << " that does not "
                                       "match the term in our required optime");
    }
    return Status::OK();
}

void SyncSourceResolver::_requiredOpTimeFetcherCallback(
    const StatusWith<Fetcher::QueryResponse>& queryResult,
    HostAndPort candidate,
    OpTime earliestOpTimeSeen,
    int rbid) {
    if (_isShuttingDown()) {
        _finishCallback(Status(ErrorCodes::CallbackCanceled,
                               str::stream() << "sync source resolver shut down while looking for "
                                                "required optime "
                                             << _requiredOpTime.toString()
                                             << " in candidate's oplog: "
                                             << candidate))
            .transitional_ignore();
        return;
    }

    if (ErrorCodes::CallbackCanceled == queryResult.getStatus()) {
        _finishCallback(queryResult.getStatus()).transitional_ignore();
        return;
    }

    if (!queryResult.isOK()) {
        // We got an error.
        const auto until = _taskExecutor->now() + kFetcherErrorBlacklistDuration;
        log() << "Blacklisting " << candidate << " due to required optime fetcher error: '"
              << queryResult.getStatus() << "' for " << kFetcherErrorBlacklistDuration
              << " until: " << until << ". required optime: " << _requiredOpTime;
        _syncSourceSelector->blacklistSyncSource(candidate, until);

        _chooseAndProbeNextSyncSource(earliestOpTimeSeen).transitional_ignore();
        return;
    }

    const auto& queryResponse = queryResult.getValue();
    auto status = _compareRequiredOpTimeWithQueryResponse(queryResponse);
    if (!status.isOK()) {
        const auto until = _taskExecutor->now() + kNoRequiredOpTimeBlacklistDuration;
        warning() << "We cannot use " << candidate.toString()
                  << " as a sync source because it does not contain the necessary "
                     "operations for us to reach a consistent state: "
                  << status << " last fetched optime: " << _lastOpTimeFetched
                  << ". required optime: " << _requiredOpTime
                  << ". Blacklisting this sync source for " << kNoRequiredOpTimeBlacklistDuration
                  << " until: " << until;
        _syncSourceSelector->blacklistSyncSource(candidate, until);

        _chooseAndProbeNextSyncSource(earliestOpTimeSeen).transitional_ignore();
        return;
    }

    _finishCallback(candidate, rbid).ignore();
}

Status SyncSourceResolver::_chooseAndProbeNextSyncSource(OpTime earliestOpTimeSeen) {
    auto candidateResult = _chooseNewSyncSource();
    if (!candidateResult.isOK()) {
        return _finishCallback(candidateResult.getStatus());
    }

    if (candidateResult.getValue().empty()) {
        if (earliestOpTimeSeen.isNull()) {
            return _finishCallback(candidateResult.getValue(),
                                   ReplicationProcess::kUninitializedRollbackId);
        }

        SyncSourceResolverResponse response;
        response.syncSourceStatus = {ErrorCodes::OplogStartMissing, "too stale to catch up"};
        response.earliestOpTimeSeen = earliestOpTimeSeen;
        return _finishCallback(response);
    }

    auto status = _scheduleFetcher(
        _makeFirstOplogEntryFetcher(candidateResult.getValue(), earliestOpTimeSeen));
    if (!status.isOK()) {
        return _finishCallback(status);
    }

    return Status::OK();
}

Status SyncSourceResolver::_finishCallback(HostAndPort hostAndPort, int rbid) {
    SyncSourceResolverResponse response;
    response.syncSourceStatus = std::move(hostAndPort);
    if (rbid != ReplicationProcess::kUninitializedRollbackId) {
        response.rbid = rbid;
    }
    return _finishCallback(response);
}

Status SyncSourceResolver::_finishCallback(Status status) {
    invariant(!status.isOK());
    SyncSourceResolverResponse response;
    response.syncSourceStatus = std::move(status);
    return _finishCallback(response);
}

Status SyncSourceResolver::_finishCallback(const SyncSourceResolverResponse& response) {
    try {
        _onCompletion(response);
    } catch (...) {
        warning() << "sync source resolver finish callback threw exception: "
                  << exceptionToStatus();
    }

    stdx::lock_guard<stdx::mutex> lock(_mutex);
    invariant(_state != State::kComplete);
    _state = State::kComplete;
    _condition.notify_all();
    return response.syncSourceStatus.getStatus();
}

}  // namespace repl
}  // namespace mongo
