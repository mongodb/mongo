// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_cursor_options.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

namespace executor {
class PinnedExecutorRegistryToken;

/**
 * A synchronous cursor API for managing a remote cursor that uses an async task executor to run all
 * stages of the command cursor protocol (initial command, getMore, killCursors)
 *
 * The main differentiator for this type over DBClientCursor is the use of a task executor (which
 * provides access to a different connection pool, as well as interruptibility) and the ability to
 * overlap getMores.  This starts fetching the next batch as soon as the previous one is received
 * (rather than on a call to 'getNext()').
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

    /**
     * Construct the cursor with a RemoteCommandRequest wrapping the initial command.
     *
     * Doesn't retry the command if we fail to establish the cursor. To create a TaskExecutorCursor
     * with the option to retry the initial command, see `makeTaskExecutorCursor`below.
     *
     * One value is carried over in successive calls to getMore/killCursor:
     *
     * opCtx - The Logical Session Id from the initial command is carried over in all later stages.
     *         NOTE - the actual command must not include the lsid
     */
    TaskExecutorCursor(std::shared_ptr<executor::TaskExecutor> executor,
                       const RemoteCommandRequest& rcr,
                       TaskExecutorCursorOptions options);

    /**
     * Construct the cursor from a cursor response from a previously executed RemoteCommandRequest.
     * The executor is used for subsequent getMore calls. Uses the original RemoteCommandRequest
     * to build subsequent commands. Takes ownership of the CursorResponse and gives it to the new
     * cursor.
     * If the cursor should reuse the original transport connection that opened the original
     * cursor, make sure the pinning executor that was used to open that cursor is provided.
     */
    TaskExecutorCursor(std::shared_ptr<executor::TaskExecutor> executor,
                       std::shared_ptr<executor::TaskExecutor> underlyingExec,
                       CursorResponse&& response,
                       const RemoteCommandRequest& rcr,
                       TaskExecutorCursorOptions&& options);

    /**
     * Move constructor to enable storing cursors in vectors.
     */
    TaskExecutorCursor(TaskExecutorCursor&& other) noexcept;

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

    boost::optional<BSONObj> getCursorExplain() const {
        return _cursorExplain;
    }

    boost::optional<CursorTypeEnum> getType() const {
        return _cursorType;
    }

    long long getBatchNum() const {
        return _batchNum;
    }

    const TaskExecutorCursorOptions& getOptions() const {
        return _options;
    }

    /**
     * Returns the vector of cursors that were returned alongside this one. Calling this claims
     * ownership of the cursors and will return an empty vector on subsequent calls.
     */
    std::vector<std::unique_ptr<TaskExecutorCursor>> releaseAdditionalCursors() {
        return std::move(_additionalCursors);
    }

    auto getNumAdditionalCursors() {
        return _additionalCursors.size();
    }

    void updateYieldPolicy(std::unique_ptr<PlanYieldPolicy> yieldPolicy) {
        _options.yieldPolicy = std::move(yieldPolicy);
    }

    PlanYieldPolicy* getYieldPolicy() {
        return _options.yieldPolicy.get();
    }

    std::shared_ptr<executor::TaskExecutor> getExecutor_forTest();

private:
    /**
     * Runs a remote command and pipes the output back to this object
     */
    void _runRemoteCommand(const RemoteCommandRequest& rcr);

    /**
     * Gets the next batch with interruptibility via the opCtx.
     */
    void _getNextBatch(OperationContext* opCtx);

    /**
     * Helper for '_getNextBatch' that handles the reading of the 'CursorResponse' object and
     * storing of relevant values.
     */
    void _processResponse(OperationContext* opCtx, CursorResponse&& response);

    /**
     * Create a new request, annotating with lsid and current opCtx
     */
    const RemoteCommandRequest& _createRequest(OperationContext* opCtx, const BSONObj& cmd);

    /**
     * Schedules a 'GetMore' request to run asyncronously.
     * This function can only be invoked when:
     * - There is no in-flight request ('_cmdState' is null).
     * - We have an open '_cursorId'.
     */
    void _scheduleGetMore(OperationContext* opCtx);

    // RAII-style token for the (pinned, underlying) executor pair. Declared before
    // _executor/_underlyingExecutor so it is destroyed after them, ensuring the PCTE stays
    // registered in the shutdown registry until it is fully destroyed.
    std::unique_ptr<PinnedExecutorRegistryToken> _pcteToken;

    std::shared_ptr<executor::TaskExecutor> _executor;
    // If we are pinning connections, we need to keep a separate reference to the
    // non-pinning, normal executor, so that we can shut down the pinned executor
    // out-of-line.
    std::shared_ptr<executor::TaskExecutor> _underlyingExecutor;

    // Used as a scratch pad for the successive scheduleRemoteCommand calls
    RemoteCommandRequest _rcr;

    TaskExecutorCursorOptions _options;

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

    // Explain object sent alongside the results in the cursor.
    boost::optional<BSONObj> _cursorExplain = boost::none;

    // For commands that return multiple cursors, the type of the cursor.
    boost::optional<CursorTypeEnum> _cursorType;

    // This is a sum of the time spent waiting on remote calls.
    Milliseconds _millisecondsWaiting = Milliseconds(0);

    // Namespace after we resolved the initial request
    NamespaceString _ns;

    // Storage for the last batch we fetched
    std::vector<BSONObj> _batch;
    decltype(_batch)::iterator _batchIter;
    long long _batchNum = 0;
    long long _totalNumDocsReceived = 0;

    // Cursors built from the responses returned alongside the results for this cursor.
    std::vector<std::unique_ptr<TaskExecutorCursor>> _additionalCursors;
};

// Make a new TaskExecutorCursor using the provided executor, RCR, and options. If we fail to create
// the cursor, the retryPolicy can inspect the error and make a decision as to whether we should
// retry. If we do retry, the error is swallowed and another attempt is made. If we don't retry,
// this function throws the error we failed with.
inline std::unique_ptr<TaskExecutorCursor> makeTaskExecutorCursor(
    OperationContext* opCtx,
    std::shared_ptr<executor::TaskExecutor> executor,
    const RemoteCommandRequest& rcr,
    TaskExecutorCursorOptions options,
    std::function<bool(Status)> retryPolicy = nullptr) {
    for (;;) {
        try {
            auto tec = std::make_unique<TaskExecutorCursor>(executor, rcr, options);
            tec->populateCursor(opCtx);
            return tec;
        } catch (const DBException& ex) {
            bool shouldRetry = retryPolicy && retryPolicy(ex.toStatus());
            if (!shouldRetry) {
                throw;
            }
        }
    }
}

}  // namespace executor
}  // namespace mongo
