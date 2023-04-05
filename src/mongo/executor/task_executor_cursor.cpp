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

#include "mongo/platform/basic.h"

#include "mongo/executor/task_executor_cursor.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/getmore_command_gen.h"
#include "mongo/db/query/kill_cursors_gen.h"
#include "mongo/executor/pinned_connection_task_executor_factory.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace executor {
namespace {
MONGO_FAIL_POINT_DEFINE(blockBeforePinnedExecutorIsDestroyedOnUnderlying);
}  // namespace

TaskExecutorCursor::TaskExecutorCursor(std::shared_ptr<executor::TaskExecutor> executor,
                                       const RemoteCommandRequest& rcr,
                                       Options&& options)
    : _rcr(rcr), _options(std::move(options)), _batchIter(_batch.end()) {

    if (rcr.opCtx) {
        _lsid = rcr.opCtx->getLogicalSessionId();
    }
    if (_options.pinConnection) {
        _executor = makePinnedConnectionTaskExecutor(executor);
        _underlyingExecutor = std::move(executor);
    } else {
        _executor = std::move(executor);
    }

    _runRemoteCommand(_createRequest(_rcr.opCtx, _rcr.cmdObj));
}

TaskExecutorCursor::TaskExecutorCursor(std::shared_ptr<executor::TaskExecutor> executor,
                                       std::shared_ptr<executor::TaskExecutor> underlyingExec,
                                       CursorResponse&& response,
                                       RemoteCommandRequest& rcr,
                                       Options&& options)
    : _executor(std::move(executor)),
      _underlyingExecutor(std::move(underlyingExec)),
      _rcr(rcr),
      _options(std::move(options)),
      _batchIter(_batch.end()) {

    tassert(6253101, "rcr must have an opCtx to use construct cursor from response", rcr.opCtx);
    _lsid = rcr.opCtx->getLogicalSessionId();
    _processResponse(rcr.opCtx, std::move(response));
}

TaskExecutorCursor::TaskExecutorCursor(TaskExecutorCursor&& other)
    : _executor(std::move(other._executor)),
      _underlyingExecutor(std::move(other._underlyingExecutor)),
      _rcr(other._rcr),
      _options(std::move(other._options)),
      _lsid(other._lsid),
      _cmdState(std::move(other._cmdState)),
      _cursorId(other._cursorId),
      _millisecondsWaiting(other._millisecondsWaiting),
      _ns(other._ns),
      _batchNum(other._batchNum),
      _additionalCursors(std::move(other._additionalCursors)) {
    // Copy the status of the batch.
    auto batchIterIndex = other._batchIter - other._batch.begin();
    _batch = std::move(other._batch);
    _batchIter = _batch.begin() + batchIterIndex;

    // Get owned copy of the vars.
    if (other._cursorVars) {
        _cursorVars = other._cursorVars->getOwned();
    }
    if (other._cursorType) {
        _cursorType = other._cursorType;
    }
    // Other is no longer responsible for this cursor id.
    other._cursorId = 0;

    // Other no longer owns the state for the in progress command (if there is any).
    other._cmdState.reset();
}

TaskExecutorCursor::~TaskExecutorCursor() {
    try {
        if (_cursorId < kMinLegalCursorId || _options.pinConnection) {
            // The initial find to establish the cursor has to be canceled to avoid leaking cursors.
            // Once the cursor is established, killing the cursor will interrupt any ongoing
            // `getMore` operation.
            // Additionally, in pinned mode, we should cancel any in-progress RPC if there is one,
            // even at the cost of churning the connection, because it's the only way to interrupt
            // the ongoing operation.
            if (_cmdState) {
                _executor->cancel(_cmdState->cbHandle);
            }
            if (_cursorId < kMinLegalCursorId) {
                return;
            }
        }

        // We deliberately ignore failures to kill the cursor. This "best effort" is acceptable
        // because some timeout mechanism on the remote host can be expected to reap it later.
        //
        // That timeout mechanism could be the default cursor timeout, or the logical session
        // timeout if an lsid is used.
        //
        // In non-pinned mode, killing the cursor also interrupts any ongoing getMore operations on
        // this cursor. Avoid canceling the remote command through its callback handle as that may
        // close the underlying connection.
        //
        // In pinned mode, we must await completion of the killCursors to safely reuse the pinned
        // connection. This requires allocating an executor thread (from `_underlyingExecutor`) upon
        // completion of the killCursors command to shutdown and destroy the pinned executor. This
        // is necessary as joining an executor from its own threads results in a deadlock.
        TaskExecutor::RemoteCommandCallbackFn callbackToRun = [](const auto&) {
        };
        if (_options.pinConnection) {
            invariant(_underlyingExecutor,
                      "TaskExecutorCursor in pinning mode must have an underlying executor");
            callbackToRun = [main = _executor, underlying = _underlyingExecutor](const auto&) {
                underlying->schedule([main = std::move(main)](const auto&) {
                    if (MONGO_unlikely(
                            blockBeforePinnedExecutorIsDestroyedOnUnderlying.shouldFail())) {
                        LOGV2(7361300,
                              "Hanging before destroying a TaskExecutorCursor's pinning executor.");
                        blockBeforePinnedExecutorIsDestroyedOnUnderlying.pauseWhileSet();
                    }
                    // Returning from this callback will destroy the pinned executor on
                    // underlying if this is the last TaskExecutorCursor using that pinned executor.
                });
            };
        }
        auto swCallback = _executor->scheduleRemoteCommand(
            _createRequest(nullptr, KillCursorsCommandRequest(_ns, {_cursorId}).toBSON(BSONObj{})),
            callbackToRun);

        // It's possible the executor is already shutdown and rejects work. If so, run the callback
        // inline.
        if (!swCallback.isOK()) {
            TaskExecutor::RemoteCommandCallbackArgs args(
                _executor.get(), {}, {}, swCallback.getStatus());
            callbackToRun(args);
        }
    } catch (const DBException& ex) {
        LOGV2(6531704,
              "Encountered an error while destroying a cursor executor",
              "error"_attr = ex.toStatus());
    }
}

boost::optional<BSONObj> TaskExecutorCursor::getNext(OperationContext* opCtx) {
    while (_batchIter == _batch.end() && _cursorId != kClosedCursorId) {
        _getNextBatch(opCtx);
    }

    if (_batchIter == _batch.end()) {
        return boost::none;
    }

    return std::move(*_batchIter++);
}

void TaskExecutorCursor::populateCursor(OperationContext* opCtx) {
    tassert(6253502,
            "populateCursors should only be called before cursor is initialized",
            _cursorId == kUnitializedCursorId);
    tassert(6253503,
            "populateCursors should only be called after a remote command has been run",
            _cmdState);
    // We really only care about populating the cursor "first batch" fields, but at some point we'll
    // have to do all of the work done by this function anyway. This would have been called by
    // getNext() the first time it was called.
    _getNextBatch(opCtx);
}

const RemoteCommandRequest& TaskExecutorCursor::_createRequest(OperationContext* opCtx,
                                                               const BSONObj& cmd) {
    // we pull this every time for updated client metadata
    _rcr.opCtx = opCtx;

    _rcr.cmdObj = [&] {
        if (!_lsid) {
            return cmd;
        }

        BSONObjBuilder bob(cmd);
        {
            BSONObjBuilder subbob(bob.subobjStart("lsid"));
            _lsid->serialize(&subbob);
            subbob.done();
        }

        return bob.obj();
    }();

    return _rcr;
}

void TaskExecutorCursor::_runRemoteCommand(const RemoteCommandRequest& rcr) {
    auto state = std::make_shared<CommandState>();
    state->cbHandle = uassertStatusOK(_executor->scheduleRemoteCommand(
        rcr, [state](const TaskExecutor::RemoteCommandCallbackArgs& args) {
            if (args.response.isOK()) {
                state->promise.emplaceValue(args.response.data);
            } else {
                state->promise.setError(args.response.status);
            }
        }));
    _cmdState.swap(state);
}

void TaskExecutorCursor::_processResponse(OperationContext* opCtx, CursorResponse&& response) {
    // If this was our first batch.
    if (_cursorId == kUnitializedCursorId) {
        _ns = response.getNSS();
        _rcr.dbname = _ns.db().toString();
        // 'vars' and type are only included in the first batch.
        _cursorVars = response.getVarsField();
        _cursorType = response.getCursorType();
    }

    _cursorId = response.getCursorId();
    _batch = response.releaseBatch();
    _batchIter = _batch.begin();

    // If we got a cursor id back, pre-fetch the next batch
    if (_cursorId) {
        GetMoreCommandRequest getMoreRequest(_cursorId, _ns.coll().toString());
        getMoreRequest.setBatchSize(_options.batchSize);
        _runRemoteCommand(_createRequest(opCtx, getMoreRequest.toBSON({})));
    }
}

void TaskExecutorCursor::_getNextBatch(OperationContext* opCtx) {
    invariant(_cmdState, "_getNextBatch() requires an async request to have already been sent.");
    invariant(_cursorId != kClosedCursorId);

    auto clock = opCtx->getServiceContext()->getPreciseClockSource();
    auto dateStart = clock->now();
    // pull out of the pipe before setting cursor id so we don't spoil this object if we're opCtx
    // interrupted
    auto out = _cmdState->promise.getFuture().getNoThrow(opCtx);
    auto dateEnd = clock->now();
    _millisecondsWaiting += std::max(Milliseconds(0), dateEnd - dateStart);
    uassertStatusOK(out);
    ++_batchNum;

    // If we had a cursor id, set it to kClosedCursorId so that we don't attempt to kill the cursor
    // if there was an error.
    if (_cursorId >= kMinLegalCursorId) {
        _cursorId = kClosedCursorId;
    }

    // if we've received a response from our last request (initial or getmore), our remote operation
    // is done.
    _cmdState.reset();

    // Parse into a vector in case the remote sent back multiple cursors.
    auto cursorResponses = CursorResponse::parseFromBSONMany(out.getValue());
    tassert(6253100, "Expected at least one response for cursor", cursorResponses.size() > 0);
    CursorResponse cr = uassertStatusOK(std::move(cursorResponses[0]));
    _processResponse(opCtx, std::move(cr));
    // If we have more responses, build them into cursors then hold them until a caller accesses
    // them. Skip the first response, we used it to populate this cursor.
    // Ensure we update the RCR we give to each 'child cursor' with the current opCtx.
    auto freshRcr = _createRequest(opCtx, _rcr.cmdObj);
    auto copyOptions = [&] {
        TaskExecutorCursor::Options options;
        options.pinConnection = _options.pinConnection;
        return options;
    };
    for (unsigned int i = 1; i < cursorResponses.size(); ++i) {
        _additionalCursors.emplace_back(_executor,
                                        _underlyingExecutor,
                                        uassertStatusOK(std::move(cursorResponses[i])),
                                        freshRcr,
                                        copyOptions());
    }
}

}  // namespace executor
}  // namespace mongo
