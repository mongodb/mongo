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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/executor/task_executor_cursor.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/db/query/killcursors_request.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace executor {

TaskExecutorCursor::TaskExecutorCursor(executor::TaskExecutor* executor,
                                       const RemoteCommandRequest& rcr,
                                       Options&& options)
    : _executor(executor), _rcr(rcr), _options(std::move(options)), _batchIter(_batch.end()) {

    if (rcr.opCtx) {
        _lsid = rcr.opCtx->getLogicalSessionId();
    }

    _runRemoteCommand(_createRequest(_rcr.opCtx, _rcr.cmdObj));
}

TaskExecutorCursor::~TaskExecutorCursor() {
    try {
        if (_cbHandle) {
            _executor->cancel(*_cbHandle);
        }

        if (_cursorId > 0) {
            // We deliberately ignore failures to kill the cursor.  This "best effort" is acceptable
            // because some timeout mechanism on the remote host can be expected to reap it later.
            //
            // That timeout mechanism could be the default cursor timeout, or the logical session
            // timeout if an lsid is used.
            _executor
                ->scheduleRemoteCommand(
                    _createRequest(nullptr, KillCursorsRequest(_ns, {_cursorId}).toBSON()),
                    [](const auto&) {})
                .isOK();
        }
    } catch (const DBException&) {
    }
}

boost::optional<BSONObj> TaskExecutorCursor::getNext(OperationContext* opCtx) {
    if (_batchIter == _batch.end()) {
        _getNextBatch(opCtx);
    }

    if (_batchIter == _batch.end()) {
        return boost::none;
    }

    return std::move(*_batchIter++);
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
    _cbHandle = uassertStatusOK(_executor->scheduleRemoteCommand(
        rcr, [p = _pipe.producer](const TaskExecutor::RemoteCommandCallbackArgs& args) {
            try {
                if (args.response.isOK()) {
                    p.push(args.response.data);
                } else {
                    p.push(args.response.status);
                }
            } catch (const DBException&) {
                // If anything goes wrong, make sure we close the pipe to wake the caller of
                // getNext()
                p.close();
            }
        }));
}

void TaskExecutorCursor::_getNextBatch(OperationContext* opCtx) {
    if (_cursorId == 0) {
        return;
    }

    auto clock = opCtx->getServiceContext()->getPreciseClockSource();
    auto dateStart = clock->now();
    // pull out of the pipe before setting cursor id so we don't spoil this object if we're opCtx
    // interrupted
    auto out = _pipe.consumer.pop(opCtx);
    auto dateEnd = clock->now();
    _millisecondsWaiting += std::max(Milliseconds(0), dateEnd - dateStart);
    uassertStatusOK(out);

    // If we had a cursor id, set it to 0 so that we don't attempt to kill the cursor if there was
    // an error
    if (_cursorId > 0) {
        _cursorId = 0;
    }

    // if we've received a response from our last request (initial or getmore), our remote operation
    // is done.
    _cbHandle.reset();

    auto cr = uassertStatusOK(CursorResponse::parseFromBSON(out.getValue()));

    // If this was our first batch
    if (_cursorId == -1) {
        _ns = cr.getNSS();
        _rcr.dbname = _ns.db().toString();
    }

    _cursorId = cr.getCursorId();
    _batch = cr.releaseBatch();
    _batchIter = _batch.begin();

    // If we got a cursor id back, pre-fetch the next batch
    if (_cursorId) {
        _runRemoteCommand(_createRequest(
            opCtx, GetMoreRequest(_ns, _cursorId, _options.batchSize, {}, {}, {}).toBSON()));
    }
}

}  // namespace executor
}  // namespace mongo
