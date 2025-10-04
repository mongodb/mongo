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

#pragma once
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/client/fetcher.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/sync_source_selector.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <functional>
#include <memory>

#include <boost/move/utility_core.hpp>

namespace mongo {

class OperationContext;
class Status;
class OperationContext;

namespace repl {

class SyncSourceSelector;

/**
 * SyncSourceResolverResponse contains the result from running SyncSourceResolver. This result will
 * indicate one of the following:
 *          1. A new sync source was selected. isOK() will return true and getSyncSource() will
 *              return the HostAndPort of the new sync source.
 *          2. No sync source was selected. isOK() will return true and getSyncSource() will return
 *              an empty HostAndPort.
 *          3. All potential sync sources are too fresh. isOK() will return false and
 *              syncSourceStatus will be ErrorCodes::OplogStartMissing and earliestOpTimeSeen will
 *              contain a new MinValid boundry. getSyncSource() is not valid to call in this state.
 */
struct SyncSourceResolverResponse {
    // Contains the new syncSource if syncSourceStatus is OK and the HostAndPort is not empty.
    StatusWith<HostAndPort> syncSourceStatus = {ErrorCodes::BadValue, "status not populated"};

    // Contains the new MinValid boundry if syncSourceStatus is ErrorCodes::OplogStartMissing.
    OpTime earliestOpTimeSeen;

    bool isOK() {
        return syncSourceStatus.isOK();
    }

    HostAndPort getSyncSource() {
        invariant(syncSourceStatus.getStatus());
        return syncSourceStatus.getValue();
    }
};

/**
 * Supplies a sync source to Fetcher, Rollback and Reporter.
 * Obtains sync source candidates to probe from SyncSourceSelector.
 * Each instance is created as needed whenever a new sync source is required and
 * is meant to be discarded after the sync source resolution is finished - 'onCompletion'
 * callback is invoked with the results contained in SyncSourceResolverResponse.
 */
class SyncSourceResolver {
public:
    static const Seconds kFetcherTimeout;
    static const Seconds kFetcherErrorDenylistDuration;
    static const Seconds kOplogEmptyDenylistDuration;
    static const Seconds kFirstOplogEntryEmptyDenylistDuration;
    static const Seconds kFirstOplogEntryNullTimestampDenylistDuration;
    static const Minutes kTooStaleDenylistDuration;

    /**
     * Callback function to report final status of resolving sync source.
     */
    typedef std::function<void(const SyncSourceResolverResponse&)> OnCompletionFn;

    SyncSourceResolver(executor::TaskExecutor* taskExecutor,
                       SyncSourceSelector* syncSourceSelector,
                       const OpTime& lastOpTimeFetched,
                       const OnCompletionFn& onCompletion);
    virtual ~SyncSourceResolver();

    /**
     * Returns true if we are currently probing sync source candidates.
     */
    bool isActive() const;

    /**
     * Starts probing sync source candidates returned by the sync source selector.
     */
    Status startup();

    /**
     * Cancels all remote commands.
     */
    void shutdown();

    /**
     * Block until inactive.
     */
    void join();

private:
    bool _isActive(WithLock lk) const;
    bool _isShuttingDown() const;

    /**
     * Returns new sync source from selector.
     */
    StatusWith<HostAndPort> _chooseNewSyncSource();

    /**
     * Creates fetcher to read the first oplog entry on sync source.
     */
    std::unique_ptr<Fetcher> _makeFirstOplogEntryFetcher(HostAndPort candidate,
                                                         OpTime earliestOpTimeSeen);

    /**
     * Schedules fetcher to read oplog on sync source.
     * Saves fetcher in '_fetcher' on success.
     */
    Status _scheduleFetcher(std::unique_ptr<Fetcher> fetcher);

    /**
     * Returns optime of first oplog entry from fetcher response.
     * Returns null optime on error.
     */
    OpTime _parseRemoteEarliestOpTime(const HostAndPort& candidate,
                                      const Fetcher::QueryResponse& queryResponse);

    /**
     * Callback for fetching first oplog entry on sync source.
     */
    void _firstOplogEntryFetcherCallback(const StatusWith<Fetcher::QueryResponse>& queryResult,
                                         HostAndPort candidate,
                                         OpTime earliestOpTimeSeen);

    /**
     * Obtains new sync source candidate and schedules remote command to fetcher first oplog entry.
     * May transition state to Complete.
     * Returns status that could be used as result for startup().
     */
    Status _chooseAndProbeNextSyncSource(OpTime earliestOpTimeSeen);

    /**
     * Invokes completion callback and transitions state to State::kComplete.
     * Returns result.getStatus().
     */
    Status _finishCallback(HostAndPort hostAndPort);
    Status _finishCallback(Status status);
    Status _finishCallback(const SyncSourceResolverResponse& response);

    // Executor used to send remote commands to sync source candidates.
    executor::TaskExecutor* const _taskExecutor;

    // Sync source selector used to obtain sync source candidates and for us to denylist non-viable
    // candidates.
    SyncSourceSelector* const _syncSourceSelector;

    // A viable sync source must contain a starting oplog entry with a timestamp equal or earlier
    // than the timestamp in '_lastOpTimeFetched'.
    const OpTime _lastOpTimeFetched;

    // This is invoked exactly once after startup. The caller gets the results of the sync source
    // resolver via this callback in a SyncSourceResolverResponse struct when the resolver finishes.
    const OnCompletionFn _onCompletion;

    // Protects members of this sync source resolver defined below.
    mutable stdx::mutex _mutex;
    mutable stdx::condition_variable _condition;

    // State transitions:
    // PreStart --> Running --> ShuttingDown --> Complete
    // It is possible to skip intermediate states. For example,
    // Calling shutdown() when the resolver has not started will transition from PreStart directly
    // to Complete.
    enum class State { kPreStart, kRunning, kShuttingDown, kComplete };
    State _state = State::kPreStart;

    // Fetches first oplog entry on sync source candidate.
    std::unique_ptr<Fetcher> _fetcher;

    // Holds reference to fetcher in the process of shutting down.
    std::unique_ptr<Fetcher> _shuttingDownFetcher;
};

}  // namespace repl
}  // namespace mongo
