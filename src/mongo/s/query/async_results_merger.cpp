/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/s/query/async_results_merger.h"

#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/db/query/killcursors_request.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

// Maximum number of retries for network and replication notMaster errors (per host).
const int kMaxNumFailedHostRetryAttempts = 3;

}  // namespace

AsyncResultsMerger::AsyncResultsMerger(executor::TaskExecutor* executor,
                                       ClusterClientCursorParams* params)
    : _executor(executor),
      _params(params),
      _mergeQueue(MergingComparator(_remotes, _params->sort)) {
    size_t remoteIndex = 0;
    for (const auto& remote : _params->remotes) {
        _remotes.emplace_back(remote.hostAndPort, remote.cursorResponse.getCursorId());

        // We don't check the return value of addBatchToBuffer here; if there was an error,
        // it will be stored in the remote and the first call to ready() will return true.
        addBatchToBuffer(remoteIndex, remote.cursorResponse.getBatch());
        ++remoteIndex;
    }

    // Initialize command metadata to handle the read preference. We do this in case the readPref
    // is primaryOnly, in which case if the remote host for one of the cursors changes roles, the
    // remote will return an error.
    if (_params->readPreference) {
        _metadataObj = _params->readPreference->toContainingBSON();
    }
}

AsyncResultsMerger::~AsyncResultsMerger() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(remotesExhausted_inlock() || _lifecycleState == kKillComplete);
}

bool AsyncResultsMerger::remotesExhausted() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return remotesExhausted_inlock();
}

bool AsyncResultsMerger::remotesExhausted_inlock() {
    for (const auto& remote : _remotes) {
        if (!remote.exhausted()) {
            return false;
        }
    }

    return true;
}

Status AsyncResultsMerger::setAwaitDataTimeout(Milliseconds awaitDataTimeout) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    if (!_params->isTailable || !_params->isAwaitData) {
        return Status(ErrorCodes::BadValue,
                      "maxTimeMS can only be used with getMore for tailable, awaitData cursors");
    }

    _awaitDataTimeout = awaitDataTimeout;
    return Status::OK();
}

bool AsyncResultsMerger::ready() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return ready_inlock();
}

bool AsyncResultsMerger::ready_inlock() {
    if (_lifecycleState != kAlive) {
        return true;
    }

    if (_eofNext) {
        // We are ready to return boost::none due to reaching the end of a batch of results from a
        // tailable cursor.
        return true;
    }

    for (const auto& remote : _remotes) {
        // First check whether any of the remotes reported an error.
        if (!remote.status.isOK()) {
            _status = remote.status;
            return true;
        }
    }

    const bool hasSort = !_params->sort.isEmpty();
    return hasSort ? readySorted_inlock() : readyUnsorted_inlock();
}

bool AsyncResultsMerger::readySorted_inlock() {
    // Tailable cursors cannot have a sort.
    invariant(!_params->isTailable);

    for (const auto& remote : _remotes) {
        if (!remote.hasNext() && !remote.exhausted()) {
            return false;
        }
    }

    return true;
}

bool AsyncResultsMerger::readyUnsorted_inlock() {
    bool allExhausted = true;
    for (const auto& remote : _remotes) {
        if (!remote.exhausted()) {
            allExhausted = false;
        }

        if (remote.hasNext()) {
            return true;
        }
    }

    return allExhausted;
}

StatusWith<ClusterQueryResult> AsyncResultsMerger::nextReady() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    dassert(ready_inlock());
    if (_lifecycleState != kAlive) {
        return Status(ErrorCodes::IllegalOperation, "AsyncResultsMerger killed");
    }

    if (!_status.isOK()) {
        return _status;
    }

    if (_eofNext) {
        _eofNext = false;
        return {ClusterQueryResult()};
    }

    const bool hasSort = !_params->sort.isEmpty();
    return hasSort ? nextReadySorted() : nextReadyUnsorted();
}

ClusterQueryResult AsyncResultsMerger::nextReadySorted() {
    // Tailable cursors cannot have a sort.
    invariant(!_params->isTailable);

    if (_mergeQueue.empty()) {
        return {};
    }

    size_t smallestRemote = _mergeQueue.top();
    _mergeQueue.pop();

    invariant(!_remotes[smallestRemote].docBuffer.empty());
    invariant(_remotes[smallestRemote].status.isOK());

    ClusterQueryResult front = _remotes[smallestRemote].docBuffer.front();
    _remotes[smallestRemote].docBuffer.pop();

    // Re-populate the merging queue with the next result from 'smallestRemote', if it has a
    // next result.
    if (!_remotes[smallestRemote].docBuffer.empty()) {
        _mergeQueue.push(smallestRemote);
    }

    return front;
}

ClusterQueryResult AsyncResultsMerger::nextReadyUnsorted() {
    size_t remotesAttempted = 0;
    while (remotesAttempted < _remotes.size()) {
        // It is illegal to call this method if there is an error received from any shard.
        invariant(_remotes[_gettingFromRemote].status.isOK());

        if (_remotes[_gettingFromRemote].hasNext()) {
            ClusterQueryResult front = _remotes[_gettingFromRemote].docBuffer.front();
            _remotes[_gettingFromRemote].docBuffer.pop();

            if (_params->isTailable && !_remotes[_gettingFromRemote].hasNext()) {
                // The cursor is tailable and we're about to return the last buffered result. This
                // means that the next value returned should be boost::none to indicate the end of
                // the batch.
                _eofNext = true;
            }

            return front;
        }

        // Nothing from the current remote so move on to the next one.
        ++remotesAttempted;
        if (++_gettingFromRemote == _remotes.size()) {
            _gettingFromRemote = 0;
        }
    }

    return {};
}

Status AsyncResultsMerger::askForNextBatch_inlock(OperationContext* opCtx, size_t remoteIndex) {
    auto& remote = _remotes[remoteIndex];

    invariant(!remote.cbHandle.isValid());

    // If mongod returned less docs than the requested batchSize then modify the next getMore
    // request to fetch the remaining docs only. If the remote node has a plan with OR for top k and
    // a full sort as is the case for the OP_QUERY find then this optimization will prevent
    // switching to the full sort plan branch.
    auto adjustedBatchSize = _params->batchSize;
    if (_params->batchSize && *_params->batchSize > remote.fetchedCount) {
        adjustedBatchSize = *_params->batchSize - remote.fetchedCount;
    }

    BSONObj cmdObj = GetMoreRequest(_params->nsString,
                                    remote.cursorId,
                                    adjustedBatchSize,
                                    _awaitDataTimeout,
                                    boost::none,
                                    boost::none)
                         .toBSON();

    executor::RemoteCommandRequest request(
        remote.getTargetHost(), _params->nsString.db().toString(), cmdObj, _metadataObj, opCtx);

    auto callbackStatus =
        _executor->scheduleRemoteCommand(request,
                                         stdx::bind(&AsyncResultsMerger::handleBatchResponse,
                                                    this,
                                                    stdx::placeholders::_1,
                                                    opCtx,
                                                    remoteIndex));
    if (!callbackStatus.isOK()) {
        return callbackStatus.getStatus();
    }

    remote.cbHandle = callbackStatus.getValue();
    return Status::OK();
}

/*
 * Note: When nextEvent() is called to do retries, only the remotes with retriable errors will
 * be rescheduled because:
 *
 * 1. Other pending remotes still have callback assigned to them.
 * 2. Remotes that already has some result will have a non-empty buffer.
 * 3. Remotes that reached maximum retries will be in 'exhausted' state.
 */
StatusWith<executor::TaskExecutor::EventHandle> AsyncResultsMerger::nextEvent(
    OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    if (_lifecycleState != kAlive) {
        // Can't schedule further network operations if the ARM is being killed.
        return Status(ErrorCodes::IllegalOperation,
                      "nextEvent() called on a killed AsyncResultsMerger");
    }

    if (_currentEvent.isValid()) {
        // We can't make a new event if there's still an unsignaled one, as every event must
        // eventually be signaled.
        return Status(ErrorCodes::IllegalOperation,
                      "nextEvent() called before an outstanding event was signaled");
    }

    // Schedule remote work on hosts for which we need more results.
    for (size_t i = 0; i < _remotes.size(); ++i) {
        auto& remote = _remotes[i];

        if (!remote.status.isOK()) {
            return remote.status;
        }

        if (!remote.hasNext() && !remote.exhausted() && !remote.cbHandle.isValid()) {
            // If this remote is not exhausted and there is no outstanding request for it, schedule
            // work to retrieve the next batch.
            auto nextBatchStatus = askForNextBatch_inlock(opCtx, i);
            if (!nextBatchStatus.isOK()) {
                return nextBatchStatus;
            }
        }
    }

    auto eventStatus = _executor->makeEvent();
    if (!eventStatus.isOK()) {
        return eventStatus;
    }
    auto eventToReturn = eventStatus.getValue();
    _currentEvent = eventToReturn;

    // It's possible that after we told the caller we had no ready results but before we replaced
    // _currentEvent with a new event, new results became available. In this case we have to signal
    // the new event right away to propagate the fact that the previous event had been signaled to
    // the new event.
    signalCurrentEventIfReady_inlock();

    return eventToReturn;
}

StatusWith<CursorResponse> AsyncResultsMerger::parseCursorResponse(const BSONObj& responseObj,
                                                                   const RemoteCursorData& remote) {
    auto getMoreParseStatus = CursorResponse::parseFromBSON(responseObj);
    if (!getMoreParseStatus.isOK()) {
        return getMoreParseStatus.getStatus();
    }

    auto cursorResponse = std::move(getMoreParseStatus.getValue());

    // If we get a non-zero cursor id that is not equal to the established cursor id, we will fail
    // the operation.
    if (cursorResponse.getCursorId() != 0 && remote.cursorId != cursorResponse.getCursorId()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Expected cursorid " << remote.cursorId << " but received "
                                    << cursorResponse.getCursorId());
    }

    return std::move(cursorResponse);
}

void AsyncResultsMerger::handleBatchResponse(
    const executor::TaskExecutor::RemoteCommandCallbackArgs& cbData,
    OperationContext* opCtx,
    size_t remoteIndex) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    auto& remote = _remotes[remoteIndex];

    // Clear the callback handle. This indicates that we are no longer waiting on a response from
    // 'remote'.
    remote.cbHandle = executor::TaskExecutor::CallbackHandle();

    // If we're in the process of shutting down then there's no need to process the batch.
    if (_lifecycleState != kAlive) {
        invariant(_lifecycleState == kKillStarted);

        // Make sure to wake up anyone waiting on '_currentEvent' if we're shutting down.
        signalCurrentEventIfReady_inlock();

        // If we're killed and we're not waiting on any more batches to come back, then we are ready
        // to kill the cursors on the remote hosts and clean up this cursor. Schedule the
        // killCursors command and signal that this cursor is safe now safe to destroy. We have to
        // promise not to touch any members of this class because 'this' could become invalid as
        // soon as we signal the event.
        if (!haveOutstandingBatchRequests_inlock()) {
            // If the event handle is invalid, then the executor is in the middle of shutting down,
            // and we can't schedule any more work for it to complete.
            if (_killCursorsScheduledEvent.isValid()) {
                scheduleKillCursors_inlock(opCtx);
                _executor->signalEvent(_killCursorsScheduledEvent);
            }

            _lifecycleState = kKillComplete;
        }

        return;
    }

    // Early return from this point on signal anyone waiting on an event, if ready() is true.
    ScopeGuard signaller = MakeGuard(&AsyncResultsMerger::signalCurrentEventIfReady_inlock, this);

    StatusWith<CursorResponse> cursorResponseStatus(
        cbData.response.isOK() ? parseCursorResponse(cbData.response.data, remote)
                               : cbData.response.status);

    if (!cursorResponseStatus.isOK()) {
        auto shard = remote.getShard();
        if (!shard) {
            remote.status = Status(cursorResponseStatus.getStatus().code(),
                                   str::stream() << "Could not find shard containing host "
                                                 << remote.getTargetHost().toString());
        } else {
            shard->updateReplSetMonitor(remote.getTargetHost(), cursorResponseStatus.getStatus());
            remote.status = cursorResponseStatus.getStatus();
        }

        // Unreachable host errors are swallowed if the 'allowPartialResults' option is set. We
        // remove the unreachable host entirely from consideration by marking it as exhausted.
        if (_params->isAllowPartialResults) {
            remote.status = Status::OK();

            // Clear the results buffer and cursor id.
            std::queue<ClusterQueryResult> emptyBuffer;
            std::swap(remote.docBuffer, emptyBuffer);
            remote.cursorId = 0;
        }

        return;
    }

    // Response successfully received.

    auto cursorResponse = std::move(cursorResponseStatus.getValue());

    // Update the cursorId; it is sent as '0' when the cursor has been exhausted on the shard.
    remote.cursorId = cursorResponse.getCursorId();

    // Save the batch in the remote's buffer.
    if (!addBatchToBuffer(remoteIndex, cursorResponse.getBatch())) {
        return;
    }

    // If the cursor is tailable and we just received an empty batch, the next return value should
    // be boost::none in order to indicate the end of the batch.
    // (Note: tailable cursors are only valid on unsharded collections, so the end of the batch from
    // one shard means the end of the overall batch).
    if (_params->isTailable && !remote.hasNext()) {
        _eofNext = true;
    }

    // If even after receiving this batch we still don't have anything buffered (i.e. the batchSize
    // was zero), then can schedule work to retrieve the next batch right away.
    //
    // We do not ask for the next batch if the cursor is tailable, as batches received from remote
    // tailable cursors should be passed through to the client without asking for more batches.
    if (!_params->isTailable && !remote.hasNext() && !remote.exhausted()) {
        remote.status = askForNextBatch_inlock(opCtx, remoteIndex);
        if (!remote.status.isOK()) {
            return;
        }
    }

    // ScopeGuard requires dismiss on success, but we want waiter to be signalled on success as
    // well as failure.
    signaller.Dismiss();
    signalCurrentEventIfReady_inlock();
}

bool AsyncResultsMerger::addBatchToBuffer(size_t remoteIndex, const std::vector<BSONObj>& batch) {
    auto& remote = _remotes[remoteIndex];
    for (const auto& obj : batch) {
        // If there's a sort, we're expecting the remote node to have given us back a sort key.
        if (!_params->sort.isEmpty() &&
            obj[ClusterClientCursorParams::kSortKeyField].type() != BSONType::Object) {
            remote.status = Status(ErrorCodes::InternalError,
                                   str::stream() << "Missing field '"
                                                 << ClusterClientCursorParams::kSortKeyField
                                                 << "' in document: "
                                                 << obj);
            return false;
        }

        ClusterQueryResult result(obj);
        remote.docBuffer.push(result);
        ++remote.fetchedCount;
    }

    // If we're doing a sorted merge, then we have to make sure to put this remote onto the
    // merge queue.
    if (!_params->sort.isEmpty() && !batch.empty()) {
        _mergeQueue.push(remoteIndex);
    }
    return true;
}

void AsyncResultsMerger::signalCurrentEventIfReady_inlock() {
    if (ready_inlock() && _currentEvent.isValid()) {
        // To prevent ourselves from signalling the event twice, we set '_currentEvent' as
        // invalid after signalling it.
        _executor->signalEvent(_currentEvent);
        _currentEvent = executor::TaskExecutor::EventHandle();
    }
}

bool AsyncResultsMerger::haveOutstandingBatchRequests_inlock() {
    for (const auto& remote : _remotes) {
        if (remote.cbHandle.isValid()) {
            return true;
        }
    }

    return false;
}

void AsyncResultsMerger::scheduleKillCursors_inlock(OperationContext* opCtx) {
    invariant(_lifecycleState == kKillStarted);
    invariant(_killCursorsScheduledEvent.isValid());

    for (const auto& remote : _remotes) {
        invariant(!remote.cbHandle.isValid());

        if (remote.status.isOK() && remote.cursorId && !remote.exhausted()) {
            BSONObj cmdObj = KillCursorsRequest(_params->nsString, {remote.cursorId}).toBSON();

            executor::RemoteCommandRequest request(
                remote.getTargetHost(), _params->nsString.db().toString(), cmdObj, opCtx);

            _executor
                ->scheduleRemoteCommand(request,
                                        stdx::bind(&AsyncResultsMerger::handleKillCursorsResponse,
                                                   stdx::placeholders::_1))
                .status_with_transitional_ignore();
        }
    }
}

void AsyncResultsMerger::handleKillCursorsResponse(
    const executor::TaskExecutor::RemoteCommandCallbackArgs& cbData) {
    // We just ignore any killCursors command responses.
}

executor::TaskExecutor::EventHandle AsyncResultsMerger::kill(OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_killCursorsScheduledEvent.isValid()) {
        invariant(_lifecycleState != kAlive);
        return _killCursorsScheduledEvent;
    }

    _lifecycleState = kKillStarted;

    // Make '_killCursorsScheduledEvent', which we will signal as soon as we have scheduled a
    // killCursors command to run on all the remote shards.
    auto statusWithEvent = _executor->makeEvent();
    if (ErrorCodes::isShutdownError(statusWithEvent.getStatus().code())) {
        // The underlying task executor is shutting down.
        if (!haveOutstandingBatchRequests_inlock()) {
            _lifecycleState = kKillComplete;
        }
        return executor::TaskExecutor::EventHandle();
    }
    fassertStatusOK(28716, statusWithEvent);
    _killCursorsScheduledEvent = statusWithEvent.getValue();

    // If we're not waiting for responses from remotes, we can schedule killCursors commands on the
    // remotes now. Otherwise, we have to wait until all responses are back, and then we can kill
    // the remote cursors.
    if (!haveOutstandingBatchRequests_inlock()) {
        scheduleKillCursors_inlock(opCtx);
        _lifecycleState = kKillComplete;
        _executor->signalEvent(_killCursorsScheduledEvent);
    } else {
        for (const auto& remote : _remotes) {
            if (remote.cbHandle.isValid()) {
                _executor->cancel(remote.cbHandle);
            }
        }
    }

    return _killCursorsScheduledEvent;
}

//
// AsyncResultsMerger::RemoteCursorData
//

AsyncResultsMerger::RemoteCursorData::RemoteCursorData(HostAndPort hostAndPort,
                                                       CursorId establishedCursorId)
    : cursorId(establishedCursorId), shardHostAndPort(std::move(hostAndPort)) {}

const HostAndPort& AsyncResultsMerger::RemoteCursorData::getTargetHost() const {
    return shardHostAndPort;
}

bool AsyncResultsMerger::RemoteCursorData::hasNext() const {
    return !docBuffer.empty();
}

bool AsyncResultsMerger::RemoteCursorData::exhausted() const {
    return cursorId == 0;
}

std::shared_ptr<Shard> AsyncResultsMerger::RemoteCursorData::getShard() {
    return grid.shardRegistry()->getShardNoReload(shardHostAndPort.toString());
}

//
// AsyncResultsMerger::MergingComparator
//

bool AsyncResultsMerger::MergingComparator::operator()(const size_t& lhs, const size_t& rhs) {
    const ClusterQueryResult& leftDoc = _remotes[lhs].docBuffer.front();
    const ClusterQueryResult& rightDoc = _remotes[rhs].docBuffer.front();

    BSONObj leftDocKey = (*leftDoc.getResult())[ClusterClientCursorParams::kSortKeyField].Obj();
    BSONObj rightDocKey = (*rightDoc.getResult())[ClusterClientCursorParams::kSortKeyField].Obj();

    // This does not need to sort with a collator, since mongod has already mapped strings to their
    // ICU comparison keys as part of the $sortKey meta projection.
    return leftDocKey.woCompare(rightDocKey, _sort, false /*considerFieldName*/) > 0;
}

}  // namespace mongo
