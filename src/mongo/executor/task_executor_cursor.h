/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <boost/optional.hpp>
#include <memory>
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/cursor_id.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"

namespace mongo {
namespace executor {

/**
 * A synchronous cursor API for managing a remote cursor that uses an async task executor to run all
 * stages of the command cursor protocol (initial command, getMore, killCursors)
 *
 * The main differentiator for this type over DBClientCursor is the use of a task executor (which
 * provides access to a different connection pool, as well as interruptibility) and the ability to
 * overlap getMores.  This starts fetching the next batch as soon as one is exhausted (rather than
 * on a call to getNext()).
 */
class TaskExecutorCursor {
public:
    // Cursor id has 1 of 3 states.
    // <0 - We haven't yet received a response for our initial request
    // 0  - Cursor is done (errored or consumed)
    // >=1 - Cursor is live on the remote
    constexpr static CursorId kUnitializedCursorId = -1;
    constexpr static CursorId kClosedCursorId = 0;
    constexpr static CursorId kMinLegalCursorId = 1;

    struct Options {
        boost::optional<int64_t> batchSize;
    };

    /**
     * Construct the cursor with a RemoteCommandRequest wrapping the initial command
     *
     * One value is carried over in successive calls to getMore/killCursor:
     *
     * opCtx - The Logical Session Id from the initial command is carried over in all later stages.
     *         NOTE - the actual command must not include the lsid
     */
    explicit TaskExecutorCursor(executor::TaskExecutor* executor,
                                const RemoteCommandRequest& rcr,
                                Options&& options = {});

    /**
     * Construct the cursor from a cursor response from a previously executed RemoteCommandRequest.
     * The executor is used for subsequent getMore calls. Uses the original RemoteCommandRequest
     * to build subsequent commands. Takes ownership of the CursorResponse and gives it to the new
     * cursor.
     */
    TaskExecutorCursor(executor::TaskExecutor* executor,
                       CursorResponse&& response,
                       RemoteCommandRequest& rcr,
                       Options&& options = {});

    /**
     * Move constructor to enable storing cursors in vectors.
     */
    TaskExecutorCursor(TaskExecutorCursor&& other);

    /**
     * Asynchronously kills async ops and kills the underlying cursor on destruction.
     */
    ~TaskExecutorCursor();

    /**
     * Returns the next document from this cursor until the cursor is exhausted (in which case we
     * return an unset optional).  This method can throw if there is an error running any commands,
     * if the remote server returns a not ok command result, or if the passed in opCtx is
     * interrupted (by killOp or maxTimeMS).
     *
     * The opCtx may also be used as the source of client metadata if this call to getNext()
     * triggers a new getMore to fetch the next batch.
     */
    boost::optional<BSONObj> getNext(OperationContext* opCtx);

    /**
     * Read the response from the remote command issued by this cursor and parse it into this
     * object. Performs the same work as getNext() above does on the first call to it, and so this
     * can throw any error that getNext can throw.
     *
     * Should not be called once getNext() has been called or the cursor has been otherwise
     * initialized.
     */
    void populateCursor(OperationContext* opCtx);

    CursorId getCursorId() const {
        return _cursorId;
    }

    Milliseconds resetWaitingTime() {
        auto toRet = _millisecondsWaiting;
        _millisecondsWaiting = Milliseconds(0);
        return toRet;
    }

    boost::optional<BSONObj> getCursorVars() const {
        return _cursorVars;
    }

    auto getType() const {
        return _cursorType;
    }

    long long getBatchNum() const {
        return _batchNum;
    }

    /**
     * Returns the vector of cursors that were returned alongside this one. Calling this claims
     * ownership of the cursors and will return an empty vector on subsequent calls.
     */
    std::vector<TaskExecutorCursor> releaseAdditionalCursors() {
        return std::move(_additionalCursors);
    }

    auto getNumAdditionalCursors() {
        return _additionalCursors.size();
    }

    /**
     * Return the callback that this cursor is waiting on. Can be used to block on getting a
     * response to this request. Can be boost::none.
     */
    boost::optional<TaskExecutor::CallbackHandle> getCallbackHandle() {
        if (_cmdState)
            return _cmdState->cbHandle;
        return {};
    }

private:
    /**
     * Runs a remote command and pipes the output back to this object
     */
    void _runRemoteCommand(const RemoteCommandRequest& rcr);

    /**
     * Gets the next batch with interruptibility via the opCtx
     */
    void _getNextBatch(OperationContext* opCtx);

    /**
     * Helper for '_getNextBatch' that handles the reading of the 'CursorResponse' object and
     * storing of relevant values. This is also responsible for issuing a getMore request if it
     * is required to populate the next batch.
     */
    void _processResponse(OperationContext* opCtx, CursorResponse&& response);

    /**
     * Create a new request, annotating with lsid and current opCtx
     */
    const RemoteCommandRequest& _createRequest(OperationContext* opCtx, const BSONObj& cmd);

    executor::TaskExecutor* const _executor;

    // Used as a scratch pad for the successive scheduleRemoteCommand calls
    RemoteCommandRequest _rcr;

    const Options _options;

    // If the opCtx is in our initial request, re-use it for all subsequent operations
    boost::optional<LogicalSessionId> _lsid;

    struct CommandState {
        TaskExecutor::CallbackHandle cbHandle;
        SharedPromise<BSONObj> promise;
    };

    /**
     * Maintains the state for the in progress command (if there is any):
     * - Handle for the task scheduled on `_executor`.
     * - A promise that will be emplaced by the result of running the command.
     *
     * The state may outlive `TaskExecutorCursor` and is shared with the callback that runs on
     * `_executor` upon completion of the remote command.
     */
    std::shared_ptr<CommandState> _cmdState;

    CursorId _cursorId = kUnitializedCursorId;

    // Variables sent alongside the results in the cursor.
    boost::optional<BSONObj> _cursorVars = boost::none;

    // For commands that return multiple cursors, the type of the cursor.
    boost::optional<std::string> _cursorType;

    // This is a sum of the time spent waiting on remote calls.
    Milliseconds _millisecondsWaiting = Milliseconds(0);

    // Namespace after we resolved the initial request
    NamespaceString _ns;

    // Storage for the last batch we fetched
    std::vector<BSONObj> _batch;
    decltype(_batch)::iterator _batchIter;
    long long _batchNum = 0;

    // Cursors built from the responses returned alongside the results for this cursor.
    std::vector<TaskExecutorCursor> _additionalCursors;
};

}  // namespace executor
}  // namespace mongo
