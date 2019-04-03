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
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/cursor_id.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/namespace_string.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/producer_consumer_queue.h"

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
     * Create a new request, annotating with lsid and current opCtx
     */
    const RemoteCommandRequest& _createRequest(OperationContext* opCtx, const BSONObj& cmd);

    executor::TaskExecutor* _executor;

    // Used as a scratch pad for the successive scheduleRemoteCommand calls
    RemoteCommandRequest _rcr;

    const Options _options;

    // If the opCtx is in our initial request, re-use it for all subsequent operations
    boost::optional<LogicalSessionId> _lsid;

    // Stash the callbackhandle for the current outstanding operation
    boost::optional<TaskExecutor::CallbackHandle> _cbHandle;

    // cursor id has 1 of 3 states.
    //
    // <1 - We haven't yet received a response for our initial request
    // 0  - Cursor is done (errored or consumed)
    // >1 - Cursor is live on the remote
    CursorId _cursorId = -1;

    // Namespace after we resolved the initial request
    NamespaceString _ns;

    // Storage for the last batch we fetched
    std::vector<BSONObj> _batch;
    decltype(_batch)::iterator _batchIter;

    // Multi producer because we hold onto the producer side in this object, as well as placing it
    // into callbacks for the task executor
    MultiProducerSingleConsumerQueue<StatusWith<BSONObj>>::Pipe _pipe;
};

}  // namespace executor
}  // namespace mongo
