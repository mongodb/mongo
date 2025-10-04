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

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_retry_scheduler.h"
#include "mongo/client/retry_strategy.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/client_cursor/clientcursor.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/net/hostandport.h"

#include <functional>
#include <iosfwd>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class Fetcher {
    Fetcher(const Fetcher&) = delete;
    Fetcher& operator=(const Fetcher&) = delete;
    using RemoteCommandRequest = executor::RemoteCommandRequest;

public:
    /**
     * Container for BSON documents extracted from cursor results.
     */
    typedef std::vector<BSONObj> Documents;

    /**
     * Documents in current query response with cursor ID and associated namespace name.
     * If cursor ID is zero, there are no additional batches.
     */
    struct QueryResponse {
        CursorId cursorId = 0;
        NamespaceString nss;
        Documents documents;
        struct OtherFields {
            BSONObj metadata;
            boost::optional<BSONObj> postBatchResumeToken = boost::none;
        } otherFields;
        Microseconds elapsed = Microseconds(0);
        bool first = false;
    };

    using QueryResponseStatus = RetryStrategy::Result<QueryResponse>;

    /**
     * Represents next steps of fetcher.
     */
    enum class NextAction : int {
        kInvalid = 0,
        kNoAction = 1,
        kGetMore = 2,
        kExitAndKeepCursorAlive = 3
    };

    /**
     * Type of a fetcher callback function.
     */
    typedef std::function<void(const QueryResponseStatus&, NextAction*, BSONObjBuilder*)>
        CallbackFn;

    /**
     * Creates Fetcher task but does not schedule it to be run by the executor.
     *
     * First remote command to be run by the executor will be 'cmdObj'. The results
     * of 'cmdObj' must contain a cursor response object.
     * See Commands::appendCursorResponseObject.
     *
     * Callback function 'work' will be called 1 or more times after a successful
     * schedule() call depending on the results of the remote command.
     *
     * Depending on the cursor ID in the initial cursor response object, the fetcher may run
     * subsequent getMore commands on the remote server in order to obtain a complete
     * set of results.
     *
     * Failed remote commands will also cause 'work' to be invoked with the
     * error details provided by the remote server. On failure, the fetcher will stop
     * sending getMore requests to the remote server.
     *
     * If the fetcher is canceled (either by calling cancel() or shutting down the executor),
     * 'work' will not be invoked.
     *
     * Fetcher uses the NextAction and BSONObjBuilder arguments to inform client via callback
     * if a follow-up (like getMore) command will be scheduled to be run by the executor to
     * retrieve additional results. The BSONObjBuilder pointer will be valid only if NextAction
     * is kGetMore.
     * Otherwise, the BSONObjBuilder pointer will be null.
     * Also, note that the NextAction is both an input and output argument to allow
     * the client to suggest a different action for the fetcher to take post-callback.
     *
     * The callback function 'work' is not allowed to call into the Fetcher instance. This
     * behavior is undefined and may result in a deadlock.
     *
     * An optional retry strategy may be provided for the first remote command request so that
     * the remote command scheduler will re-send the command in case of transient network errors.
     */
    Fetcher(executor::TaskExecutor* executor,
            const HostAndPort& source,
            const DatabaseName& dbname,
            const BSONObj& cmdObj,
            CallbackFn work,
            const BSONObj& metadata = ReadPreferenceSetting::secondaryPreferredMetadata(),
            Milliseconds findNetworkTimeout = RemoteCommandRequest::kNoTimeout,
            Milliseconds getMoreNetworkTimeout = RemoteCommandRequest::kNoTimeout,
            std::unique_ptr<mongo::RetryStrategy> firstCommandRetryStrategy =
                std::make_unique<NoRetryStrategy>(),
            transport::ConnectSSLMode sslMode = transport::kGlobalSSLMode);

    virtual ~Fetcher();

    /**
     * Returns host where remote commands will be sent to.
     */
    HostAndPort getSource() const;

    /**
     * Returns command object sent in first remote command.
     */
    BSONObj getCommandObject() const;

    /**
     * Returns metadata object sent in remote commands.
     */
    BSONObj getMetadataObject() const;

    /**
     * Returns diagnostic information.
     */
    std::string getDiagnosticString() const;

    /**
     * Returns an informational string.
     */
    std::string toString() const;

    /**
     * Returns true if a remote command has been scheduled (but not completed)
     * with the executor.
     */
    bool isActive() const;

    /**
     * Schedules 'cmdObj' to be run on the remote server.
     */
    Status schedule();

    /**
     * Cancels remote command request.
     * Returns immediately if fetcher is not active.
     */
    void shutdown();

    /**
     * Waits for remote command requests to complete subject to the Interruptible being interrupted.
     * Returns immediately if fetcher is not active.
     *
     * Returns an OK Status if the wait completed successfully without interruption.
     * Returns a non-OK Status if the Interruptible had been interrupted.
     */
    Status join(Interruptible* interruptible);

    // State transitions:
    // PreStart --> Running --> ShuttingDown --> Complete
    // It is possible to skip intermediate states. For example,
    // Calling shutdown() when the cloner has not started will transition from PreStart directly
    // to Complete.
    // This enum class is made public for testing.
    enum class State { kPreStart, kRunning, kShuttingDown, kComplete };

    /**
     * Returns current fetcher state.
     * For testing only.
     */
    State getState_forTest() const;

    /**
     * Returns a Future that will be resolved when the fetcher completes its work.
     */
    SharedSemiFuture<void> onCompletion() const {
        return _completionPromise.getFuture();
    }

private:
    bool _isActive(WithLock lk) const;

    /**
     * Schedules getMore command to be run by the executor
     */
    Status _scheduleGetMore(const BSONObj& cmdObj);

    /**
     * Callback for remote command.
     */
    void _callback(const executor::TaskExecutor::RemoteCommandCallbackArgs& rcbd,
                   const char* batchFieldName);

    /**
     * Sets fetcher state to inactive and notifies waiters.
     */
    void _finishCallback();

    /**
     * Sends a kill cursor for the specified id and collection (namespace)
     *
     * Note: Errors are ignored and no retry is done
     */
    void _sendKillCursors(CursorId id, const NamespaceString& nss);

    /**
     * Returns whether the fetcher is in shutdown.
     */
    bool _isShuttingDown() const;
    bool _isShuttingDown_inlock() const;

    /**
     * Waits for remote command requests to complete.
     * Returns immediately if fetcher is not active.
     */
    void _join();

    // Not owned by us.
    executor::TaskExecutor* _executor;

    HostAndPort _source;
    DatabaseName _dbname;
    BSONObj _cmdObj;
    BSONObj _metadata;
    CallbackFn _work;

    // Protects member data of this Fetcher.
    mutable stdx::mutex _mutex;

    mutable stdx::condition_variable _condition;

    // Current fetcher state. See comments for State enum class for details.
    State _state = State::kPreStart;

    // _first is true for first query response and false for subsequent responses.
    // Using boolean instead of a counter to avoid issues with wrap around.
    bool _first = true;

    // Callback handle to the scheduled getMore command.
    executor::TaskExecutor::CallbackHandle _getMoreCallbackHandle;

    // Socket timeout
    Milliseconds _findNetworkTimeout;
    Milliseconds _getMoreNetworkTimeout;

    // First remote command scheduler.
    RemoteCommandRetryScheduler _firstRemoteCommandScheduler;

    const transport::ConnectSSLMode _sslMode;

    // Promise that is resolved when a fetcher completes or shuts down.
    SharedPromise<void> _completionPromise;
};

/**
 * Insertion operator for Fetcher::State. Formats fetcher state for output stream.
 * For testing only.
 */
std::ostream& operator<<(std::ostream& os, const Fetcher::State& state);

}  // namespace mongo
