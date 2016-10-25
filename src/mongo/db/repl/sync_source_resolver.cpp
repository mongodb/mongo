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
#include "mongo/db/repl/sync_source_selector.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
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

SyncSourceResolver::SyncSourceResolver(executor::TaskExecutor* taskExecutor,
                                       SyncSourceSelector* syncSourceSelector,
                                       const OpTime& lastOpTimeFetched,
                                       const OnCompletionFn& onCompletion)
    : _taskExecutor(taskExecutor),
      _syncSourceSelector(syncSourceSelector),
      _lastOpTimeFetched(lastOpTimeFetched),
      _onCompletion(onCompletion) {
    uassert(ErrorCodes::BadValue, "task executor cannot be null", taskExecutor);
    uassert(ErrorCodes::BadValue, "sync source selector cannot be null", syncSourceSelector);
    uassert(
        ErrorCodes::BadValue, "last fetched optime cannot be null", !lastOpTimeFetched.isNull());
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
        if (State::kPreStart != _state) {
            return Status(ErrorCodes::IllegalOperation, "sync source resolver already started");
        }
        _state = State::kRunning;
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
        candidate = _syncSourceSelector->chooseNewSyncSource(_lastOpTimeFetched.getTimestamp());
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
        BSON("find" << kLocalOplogNss.coll() << "limit" << 1 << "sort" << BSON("$natural" << 1)),
        stdx::bind(&SyncSourceResolver::_firstOplogEntryFetcherCallback,
                   this,
                   stdx::placeholders::_1,
                   candidate,
                   earliestOpTimeSeen),
        rpc::ServerSelectionMetadata(true, boost::none).toBSON(),
        kFetcherTimeout);
}

Status SyncSourceResolver::_scheduleFetcher(std::unique_ptr<Fetcher> fetcher) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    // Must schedule fetcher inside lock in case fetcher's callback gets invoked immediately by task
    // executor.
    auto status = fetcher->schedule();
    if (status.isOK()) {
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
        log() << "Blacklisting due to empty oplog on host " << candidate << " for "
              << kOplogEmptyBlacklistDuration << " until: " << until;
        _syncSourceSelector->blacklistSyncSource(candidate, until);
        return OpTime();
    }

    const auto& firstObjFound = queryResponse.documents.front();
    if (firstObjFound.isEmpty()) {
        // First document in remote oplog is empty.
        const auto until = _taskExecutor->now() + kFirstOplogEntryEmptyBlacklistDuration;
        log() << "Blacklisting due to empty first document from host " << candidate << " for "
              << kFirstOplogEntryEmptyBlacklistDuration << " until: " << until;
        _syncSourceSelector->blacklistSyncSource(candidate, until);
        return OpTime();
    }

    const OplogEntry oplogEntry(firstObjFound);
    const auto remoteEarliestOpTime = oplogEntry.getOpTime();
    if (remoteEarliestOpTime.isNull()) {
        // First document in remote oplog is empty.
        const auto until = _taskExecutor->now() + kFirstOplogEntryNullTimestampBlacklistDuration;
        log() << "Blacklisting due to null timestamp in first document from host " << candidate
              << " for " << kFirstOplogEntryNullTimestampBlacklistDuration << " until: " << until;
        _syncSourceSelector->blacklistSyncSource(candidate, until);
        return OpTime();
    }

    return remoteEarliestOpTime;
}

void SyncSourceResolver::_firstOplogEntryFetcherCallback(
    const StatusWith<Fetcher::QueryResponse>& queryResult,
    HostAndPort candidate,
    OpTime earliestOpTimeSeen) {
    if (_isShuttingDown()) {
        _finishCallback(Status(ErrorCodes::CallbackCanceled,
                               str::stream()
                                   << "sync source resolver shut down while probing candidate: "
                                   << candidate));
        return;
    }

    if (ErrorCodes::CallbackCanceled == queryResult.getStatus()) {
        _finishCallback(queryResult.getStatus());
        return;
    }

    if (!queryResult.isOK()) {
        // We got an error.
        const auto until = _taskExecutor->now() + kFetcherErrorBlacklistDuration;
        log() << "Blacklisting " << candidate << " due to error: '" << queryResult.getStatus()
              << "' for " << kFetcherErrorBlacklistDuration << " until: " << until;
        _syncSourceSelector->blacklistSyncSource(candidate, until);

        _chooseAndProbeNextSyncSource(earliestOpTimeSeen);
        return;
    }

    const auto& queryResponse = queryResult.getValue();
    const auto remoteEarliestOpTime = _parseRemoteEarliestOpTime(candidate, queryResponse);
    if (remoteEarliestOpTime.isNull()) {
        _chooseAndProbeNextSyncSource(earliestOpTimeSeen);
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

        _chooseAndProbeNextSyncSource(earliestOpTimeSeen);
        return;
    }

    _finishCallback(candidate);
}

Status SyncSourceResolver::_chooseAndProbeNextSyncSource(OpTime earliestOpTimeSeen) {
    auto candidateResult = _chooseNewSyncSource();
    if (!candidateResult.isOK()) {
        return _finishCallback(candidateResult);
    }

    if (candidateResult.getValue().empty()) {
        if (earliestOpTimeSeen.isNull()) {
            return _finishCallback(candidateResult);
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

Status SyncSourceResolver::_finishCallback(StatusWith<HostAndPort> result) {
    SyncSourceResolverResponse response;
    response.syncSourceStatus = std::move(result);
    return _finishCallback(response);
}

Status SyncSourceResolver::_finishCallback(const SyncSourceResolverResponse& response) {
    _onCompletion(response);

    stdx::lock_guard<stdx::mutex> lock(_mutex);
    invariant(_state != State::kComplete);
    _state = State::kComplete;
    _condition.notify_all();
    return response.syncSourceStatus.getStatus();
}

}  // namespace repl
}  // namespace mongo
