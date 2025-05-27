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


#include <boost/move/utility_core.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/sync_source_resolver.h"
#include "mongo/db/repl/sync_source_selector.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#include <memory>
#include <mutex>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {

MONGO_FAIL_POINT_DEFINE(failfirstOplogEntryFetcherCallback);

const Seconds SyncSourceResolver::kFetcherTimeout(30);
const Seconds SyncSourceResolver::kFetcherErrorDenylistDuration(10);
const Seconds SyncSourceResolver::kOplogEmptyDenylistDuration(10);
const Seconds SyncSourceResolver::kFirstOplogEntryEmptyDenylistDuration(10);
const Seconds SyncSourceResolver::kFirstOplogEntryNullTimestampDenylistDuration(10);
const Minutes SyncSourceResolver::kTooStaleDenylistDuration(1);

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
    try {
        shutdown();
        join();
    } catch (...) {
        reportFailedDestructor(MONGO_SOURCE_LOCATION());
    }
}

bool SyncSourceResolver::isActive() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _isActive(lock);
}

bool SyncSourceResolver::_isActive(WithLock lk) const {
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
}

void SyncSourceResolver::join() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _condition.wait(lk, [&]() { return !_isActive(lk); });
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
    return std::make_unique<Fetcher>(
        _taskExecutor,
        candidate,
        DatabaseName::kLocal,
        BSON(
            "find"
            << NamespaceString::kRsOplogNamespace.coll() << "limit" << 1 << "sort"
            << BSON("$natural" << 1) << "projection"
            << BSON(OplogEntryBase::kTimestampFieldName << 1 << OplogEntryBase::kTermFieldName << 1)
            << ReadConcernArgs::kReadConcernFieldName << ReadConcernArgs::kLocal.toBSONInner()
            << "term"
            << -1 /* Attach a dummy term so that the find command skips ticket acquisition */),
        [=, this](const StatusWith<Fetcher::QueryResponse>& response,
                  Fetcher::NextAction*,
                  BSONObjBuilder*) {
            return _firstOplogEntryFetcherCallback(response, candidate, earliestOpTimeSeen);
        },
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
        LOGV2_ERROR(21776,
                    "Error scheduling fetcher to evaluate host as sync source",
                    "host"_attr = fetcher->getSource(),
                    "error"_attr = status);
    }
    return status;
}

OpTime SyncSourceResolver::_parseRemoteEarliestOpTime(const HostAndPort& candidate,
                                                      const Fetcher::QueryResponse& queryResponse) {
    if (queryResponse.documents.empty()) {
        // Remote oplog is empty.
        const auto until = _taskExecutor->now() + kOplogEmptyDenylistDuration;
        LOGV2(5579703,
              "Denylisting candidate due to empty oplog",
              "candidate"_attr = candidate,
              "denylistDuration"_attr = kOplogEmptyDenylistDuration,
              "denylistUntil"_attr = until);
        _syncSourceSelector->denylistSyncSource(candidate, until);
        return OpTime();
    }

    const auto& firstObjFound = queryResponse.documents.front();
    if (firstObjFound.isEmpty()) {
        // First document in remote oplog is empty.
        const auto until = _taskExecutor->now() + kFirstOplogEntryEmptyDenylistDuration;
        LOGV2(5579704,
              "Denylisting candidate due to empty first document",
              "candidate"_attr = candidate,
              "denylistDuration"_attr = kFirstOplogEntryEmptyDenylistDuration,
              "denylistUntil"_attr = until);
        _syncSourceSelector->denylistSyncSource(candidate, until);
        return OpTime();
    }

    const auto remoteEarliestOpTime = OpTime::parseFromOplogEntry(firstObjFound);
    if (!remoteEarliestOpTime.isOK()) {
        const auto until = _taskExecutor->now() + kFirstOplogEntryNullTimestampDenylistDuration;
        LOGV2(5579705,
              "Denylisting candidate due to error parsing OpTime from the oldest oplog entry",
              "candidate"_attr = candidate,
              "denylistDuration"_attr = kFirstOplogEntryNullTimestampDenylistDuration,
              "denylistUntil"_attr = until,
              "error"_attr = remoteEarliestOpTime.getStatus(),
              "oldestOplogEntry"_attr = redact(firstObjFound));
        _syncSourceSelector->denylistSyncSource(candidate, until);
        return OpTime();
    }

    if (remoteEarliestOpTime.getValue().isNull()) {
        // First document in remote oplog is empty.
        const auto until = _taskExecutor->now() + kFirstOplogEntryNullTimestampDenylistDuration;
        LOGV2(5579706,
              "Denylisting candidate due to null timestamp in first document",
              "candidate"_attr = candidate,
              "denylistDuration"_attr = kFirstOplogEntryNullTimestampDenylistDuration,
              "denylistUntil"_attr = until);
        _syncSourceSelector->denylistSyncSource(candidate, until);
        return OpTime();
    }

    return remoteEarliestOpTime.getValue();
}

void SyncSourceResolver::_firstOplogEntryFetcherCallback(
    const StatusWith<Fetcher::QueryResponse>& queryResult,
    HostAndPort candidate,
    OpTime earliestOpTimeSeen) {

    if (_isShuttingDown() || MONGO_unlikely(failfirstOplogEntryFetcherCallback.shouldFail())) {
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
        const auto until = _taskExecutor->now() + kFetcherErrorDenylistDuration;
        LOGV2(5579707,
              "Denylisting candidate due to error",
              "candidate"_attr = candidate,
              "error"_attr = queryResult.getStatus(),
              "denylistDuration"_attr = kFetcherErrorDenylistDuration,
              "denylistUntil"_attr = until);
        _syncSourceSelector->denylistSyncSource(candidate, until);

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
        const auto denylistDuration = kTooStaleDenylistDuration;
        const auto until = _taskExecutor->now() + denylistDuration;

        LOGV2(5579708,
              "We are too stale to use candidate as a sync source. Denylisting this sync source "
              "because our last fetched timestamp is before their earliest timestamp",
              "candidate"_attr = candidate,
              "lastOpTimeFetchedTimestamp"_attr = _lastOpTimeFetched.getTimestamp(),
              "remoteEarliestOpTimeTimestamp"_attr = remoteEarliestOpTime.getTimestamp(),
              "denylistDuration"_attr = denylistDuration,
              "denylistUntil"_attr = until);

        _syncSourceSelector->denylistSyncSource(candidate, until);

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

    // we can safely return the candidate.
    _finishCallback(candidate).ignore();
    return;
}

Status SyncSourceResolver::_chooseAndProbeNextSyncSource(OpTime earliestOpTimeSeen) {
    auto candidateResult = _chooseNewSyncSource();
    if (!candidateResult.isOK()) {
        return _finishCallback(candidateResult.getStatus());
    }

    if (candidateResult.getValue().empty()) {
        if (earliestOpTimeSeen.isNull()) {
            return _finishCallback(candidateResult.getValue());
        }

        SyncSourceResolverResponse response;
        response.syncSourceStatus = {ErrorCodes::TooStaleToSyncFromSource, "too stale to catch up"};
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

Status SyncSourceResolver::_finishCallback(HostAndPort hostAndPort) {
    SyncSourceResolverResponse response;
    response.syncSourceStatus = std::move(hostAndPort);
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
        LOGV2_WARNING(21775,
                      "Sync source resolver finish callback threw exception",
                      "error"_attr = exceptionToStatus());
    }

    stdx::lock_guard<stdx::mutex> lock(_mutex);
    invariant(_state != State::kComplete);
    _state = State::kComplete;
    _condition.notify_all();
    return response.syncSourceStatus.getStatus();
}

}  // namespace repl
}  // namespace mongo
