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

#include "mongo/s/query/async_cluster_client_cursor.h"

#include "mongo/client/remote_command_runner.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/db/query/getmore_response.h"
#include "mongo/db/query/killcursors_request.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

AsyncClusterClientCursor::AsyncClusterClientCursor(executor::TaskExecutor* executor,
                                                   const ClusterClientCursorParams& params,
                                                   const std::vector<HostAndPort>& remotes)
    : _executor(executor), _params(params), _mergeQueue(MergingComparator(_remotes, _params.sort)) {
    for (const auto& remote : remotes) {
        _remotes.emplace_back(remote);
    }
}

AsyncClusterClientCursor::~AsyncClusterClientCursor() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    bool allExhausted = true;
    for (const auto& remote : _remotes) {
        if (!remote.exhausted()) {
            allExhausted = false;
        }
    }

    invariant(allExhausted || _lifecycleState == kKillComplete);
}

bool AsyncClusterClientCursor::ready() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return ready_inlock();
}

bool AsyncClusterClientCursor::ready_inlock() {
    if (_lifecycleState != kAlive) {
        return true;
    }

    // First check whether any of the remotes reported an error.
    for (const auto& remote : _remotes) {
        if (!remote.status.isOK()) {
            _status = remote.status;
            return true;
        }
    }

    const bool hasSort = !_params.sort.isEmpty();
    return hasSort ? readySorted_inlock() : readyUnsorted_inlock();
}

bool AsyncClusterClientCursor::readySorted_inlock() {
    for (const auto& remote : _remotes) {
        if (!remote.hasNext() && !remote.exhausted()) {
            return false;
        }
    }

    return true;
}

bool AsyncClusterClientCursor::readyUnsorted_inlock() {
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

StatusWith<boost::optional<BSONObj>> AsyncClusterClientCursor::nextReady() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    dassert(ready_inlock());
    if (_lifecycleState != kAlive) {
        return Status(ErrorCodes::IllegalOperation, "async cluster client cursor killed");
    }

    if (!_status.isOK()) {
        return _status;
    }

    const bool hasSort = !_params.sort.isEmpty();
    return hasSort ? nextReadySorted() : nextReadyUnsorted();
}

boost::optional<BSONObj> AsyncClusterClientCursor::nextReadySorted() {
    if (_mergeQueue.empty()) {
        return boost::none;
    }

    size_t smallestRemote = _mergeQueue.top();
    _mergeQueue.pop();

    invariant(!_remotes[smallestRemote].docBuffer.empty());
    invariant(_remotes[smallestRemote].status.isOK());

    BSONObj front = _remotes[smallestRemote].docBuffer.front();
    _remotes[smallestRemote].docBuffer.pop();

    // Re-populate the merging queue with the next result from 'smallestRemote', if it has a
    // next result.
    if (!_remotes[smallestRemote].docBuffer.empty()) {
        _mergeQueue.push(smallestRemote);
    }

    return front;
}

boost::optional<BSONObj> AsyncClusterClientCursor::nextReadyUnsorted() {
    size_t remotesAttempted = 0;
    while (remotesAttempted < _remotes.size()) {
        // It is illegal to call this method if there is an error received from any shard.
        invariant(_remotes[_gettingFromRemote].status.isOK());

        if (_remotes[_gettingFromRemote].hasNext()) {
            BSONObj front = _remotes[_gettingFromRemote].docBuffer.front();
            _remotes[_gettingFromRemote].docBuffer.pop();
            return front;
        }

        // Nothing from the current remote so move on to the next one.
        ++remotesAttempted;
        if (++_gettingFromRemote == _remotes.size()) {
            _gettingFromRemote = 0;
        }
    }

    return boost::none;
}

StatusWith<executor::TaskExecutor::EventHandle> AsyncClusterClientCursor::nextEvent() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    if (_lifecycleState != kAlive) {
        // Can't schedule further network operations if the ACCC is being killed.
        return Status(ErrorCodes::IllegalOperation,
                      "nextEvent() called on a killed async cluster client cursor");
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

        // It is illegal to call this method if there is an error received from any shard.
        invariant(remote.status.isOK());

        if (!remote.hasNext() && !remote.exhausted() && !remote.cbHandle.isValid()) {
            // If we already have established a cursor with this remote, and there is no outstanding
            // request for which we have a valid callback handle, send a getMore with the
            // appropriate cursorId. Otherwise, send the cursor-establishing command.
            BSONObj cmdObj = remote.cursorId
                ? GetMoreRequest(_params.nsString, *remote.cursorId, _params.batchSize).toBSON()
                : _params.cmdObj;

            RemoteCommandRequest request(
                remote.hostAndPort, _params.nsString.db().toString(), cmdObj);

            auto callbackStatus = _executor->scheduleRemoteCommand(
                request,
                stdx::bind(&AsyncClusterClientCursor::handleBatchResponse,
                           this,
                           stdx::placeholders::_1,
                           i));
            if (!callbackStatus.isOK()) {
                return callbackStatus.getStatus();
            }

            remote.cbHandle = callbackStatus.getValue();
        }
    }

    auto eventStatus = _executor->makeEvent();
    if (!eventStatus.isOK()) {
        return eventStatus;
    }
    _currentEvent = eventStatus.getValue();
    return _currentEvent;
}

void AsyncClusterClientCursor::handleBatchResponse(
    const executor::TaskExecutor::RemoteCommandCallbackArgs& cbData, size_t remoteIndex) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    auto& remote = _remotes[remoteIndex];

    // Clear the callback handle. This indicates that we are no longer waiting on a response from
    // 'remote'.
    remote.cbHandle = executor::TaskExecutor::CallbackHandle();

    // If we're in the process of shutting down then there's no need to process the batch.
    if (_lifecycleState != kAlive) {
        invariant(_lifecycleState == kKillStarted);

        // Make sure to wake up anyone waiting on '_currentEvent' if we're shutting down.
        signalCurrentEvent_inlock();

        // If we're killed and we're not waiting on any more batches to come back, then we are ready
        // to kill the cursors on the remote hosts and clean up this cursor. Schedule the
        // killCursors command and signal that this cursor is safe now safe to destroy. We have to
        // promise not to touch any members of this class because 'this' could become invalid as
        // soon as we signal the event.
        if (!haveOutstandingBatchRequests_inlock()) {
            // If the event handle is invalid, then the executor is in the middle of shutting down,
            // and we can't schedule any more work for it to complete.
            if (_killCursorsScheduledEvent.isValid()) {
                scheduleKillCursors_inlock();
                _executor->signalEvent(_killCursorsScheduledEvent);
            }

            _lifecycleState = kKillComplete;
        }
        return;
    }

    // Early return from this point on signal anyone waiting on an event, if ready() is true.
    ScopeGuard signaller = MakeGuard(&AsyncClusterClientCursor::signalCurrentEvent_inlock, this);

    if (!cbData.response.isOK()) {
        _remotes[remoteIndex].status = cbData.response.getStatus();
        return;
    }

    auto getMoreParseStatus = GetMoreResponse::parseFromBSON(cbData.response.getValue().data);
    if (!getMoreParseStatus.isOK()) {
        _remotes[remoteIndex].status = getMoreParseStatus.getStatus();
        return;
    }

    auto getMoreResponse = getMoreParseStatus.getValue();

    // If we have a cursor established, and we get a non-zero cursorid that is not equal to the
    // established cursorid, we will fail the operation.
    if (remote.cursorId && getMoreResponse.cursorId != 0 &&
        *remote.cursorId != getMoreResponse.cursorId) {
        _remotes[remoteIndex].status =
            Status(ErrorCodes::BadValue,
                   str::stream() << "Expected cursorid " << *remote.cursorId << " but received "
                                 << getMoreResponse.cursorId);
        return;
    }

    remote.cursorId = getMoreResponse.cursorId;

    for (const auto& obj : getMoreResponse.batch) {
        remote.docBuffer.push(obj);
    }

    // If we're doing a sorted merge, then we have to make sure to put this remote onto the
    // merge queue.
    if (!_params.sort.isEmpty() && !getMoreResponse.batch.empty()) {
        _mergeQueue.push(remoteIndex);
    }

    // ScopeGuard requires dismiss on success, but we want waiter to be signalled on success as
    // well as failure.
    signaller.Dismiss();
    signalCurrentEvent_inlock();
}

void AsyncClusterClientCursor::signalCurrentEvent_inlock() {
    if (ready_inlock() && _currentEvent.isValid()) {
        // To prevent ourselves from signalling the event twice, we set '_currentEvent' as
        // invalid after signalling it.
        _executor->signalEvent(_currentEvent);
        _currentEvent = executor::TaskExecutor::EventHandle();
    }
}

bool AsyncClusterClientCursor::haveOutstandingBatchRequests_inlock() {
    for (const auto& remote : _remotes) {
        if (remote.cbHandle.isValid()) {
            return true;
        }
    }

    return false;
}

void AsyncClusterClientCursor::scheduleKillCursors_inlock() {
    invariant(_lifecycleState == kKillStarted);
    invariant(_killCursorsScheduledEvent.isValid());

    for (const auto& remote : _remotes) {
        invariant(!remote.cbHandle.isValid());

        if (remote.status.isOK() && remote.cursorId && !remote.exhausted()) {
            BSONObj cmdObj = KillCursorsRequest(_params.nsString, {*remote.cursorId}).toBSON();

            RemoteCommandRequest request(
                remote.hostAndPort, _params.nsString.db().toString(), cmdObj);

            _executor->scheduleRemoteCommand(
                request,
                stdx::bind(&AsyncClusterClientCursor::handleKillCursorsResponse,
                           stdx::placeholders::_1));
        }
    }
}

void AsyncClusterClientCursor::handleKillCursorsResponse(
    const executor::TaskExecutor::RemoteCommandCallbackArgs& cbData) {
    // We just ignore any killCursors command responses.
}

executor::TaskExecutor::EventHandle AsyncClusterClientCursor::kill() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_killCursorsScheduledEvent.isValid()) {
        invariant(_lifecycleState != kAlive);
        return _killCursorsScheduledEvent;
    }

    _lifecycleState = kKillStarted;

    // Cancel callbacks.
    for (const auto& remote : _remotes) {
        if (remote.cbHandle.isValid()) {
            _executor->cancel(remote.cbHandle);
        }
    }

    // Make '_killCursorsScheduledEvent', which we will signal as soon as we have scheduled a
    // killCursors command to run on all the remote shards.
    auto statusWithEvent = _executor->makeEvent();
    if (statusWithEvent.getStatus().code() == ErrorCodes::ShutdownInProgress) {
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
        scheduleKillCursors_inlock();
        _lifecycleState = kKillComplete;
        _executor->signalEvent(_killCursorsScheduledEvent);
    }

    return _killCursorsScheduledEvent;
}

//
// AsyncClusterClientCursor::RemoteCursorData
//

AsyncClusterClientCursor::RemoteCursorData::RemoteCursorData(const HostAndPort& host)
    : hostAndPort(host) {}

bool AsyncClusterClientCursor::RemoteCursorData::hasNext() const {
    return !docBuffer.empty();
}

bool AsyncClusterClientCursor::RemoteCursorData::exhausted() const {
    return cursorId && (*cursorId == 0);
}

//
// AsyncClusterClientCursor::MergingComparator
//

bool AsyncClusterClientCursor::MergingComparator::operator()(const size_t& lhs, const size_t& rhs) {
    const BSONObj& leftDoc = _remotes[lhs].docBuffer.front();
    const BSONObj& rightDoc = _remotes[rhs].docBuffer.front();

    return leftDoc.woSortOrder(rightDoc, _sort, true /*useDotted*/) > 0;
}

}  // namespace mongo
