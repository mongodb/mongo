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
#include "mongo/rpc/metadata/server_selection_metadata.h"
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
                                       ClusterClientCursorParams&& params)
    : _executor(executor),
      _params(std::move(params)),
      _mergeQueue(MergingComparator(_remotes, _params.sort)) {
    for (const auto& remote : _params.remotes) {
        if (remote.shardId) {
            invariant(remote.cmdObj);
            invariant(!remote.cursorId);
            invariant(!remote.hostAndPort);
            _remotes.emplace_back(*remote.shardId, *remote.cmdObj);
        } else {
            invariant(!remote.cmdObj);
            invariant(remote.cursorId);
            invariant(remote.hostAndPort);
            _remotes.emplace_back(*remote.hostAndPort, *remote.cursorId);
        }
    }

    // Initialize command metadata to handle the read preference.
    if (_params.readPreference) {
        BSONObjBuilder metadataBuilder;
        rpc::ServerSelectionMetadata metadata(
            _params.readPreference->pref != ReadPreference::PrimaryOnly, boost::none);
        uassertStatusOK(metadata.writeToMetadata(&metadataBuilder));
        _metadataObj = metadataBuilder.obj();
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

    if (!_params.isTailable || !_params.isAwaitData) {
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

        // We don't return any results until we have received at least one response from each remote
        // node. This is necessary for versioned commands: we have to ensure that we've properly
        // established the shard version on each node before we can start returning results.
        if (!remote.cursorId) {
            return false;
        }
    }

    const bool hasSort = !_params.sort.isEmpty();
    return hasSort ? readySorted_inlock() : readyUnsorted_inlock();
}

bool AsyncResultsMerger::readySorted_inlock() {
    // Tailable cursors cannot have a sort.
    invariant(!_params.isTailable);

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

StatusWith<boost::optional<BSONObj>> AsyncResultsMerger::nextReady() {
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
        return {boost::none};
    }

    const bool hasSort = !_params.sort.isEmpty();
    return hasSort ? nextReadySorted() : nextReadyUnsorted();
}

boost::optional<BSONObj> AsyncResultsMerger::nextReadySorted() {
    // Tailable cursors cannot have a sort.
    invariant(!_params.isTailable);

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

boost::optional<BSONObj> AsyncResultsMerger::nextReadyUnsorted() {
    size_t remotesAttempted = 0;
    while (remotesAttempted < _remotes.size()) {
        // It is illegal to call this method if there is an error received from any shard.
        invariant(_remotes[_gettingFromRemote].status.isOK());

        if (_remotes[_gettingFromRemote].hasNext()) {
            BSONObj front = _remotes[_gettingFromRemote].docBuffer.front();
            _remotes[_gettingFromRemote].docBuffer.pop();

            if (_params.isTailable && !_remotes[_gettingFromRemote].hasNext()) {
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

    return boost::none;
}

Status AsyncResultsMerger::askForNextBatch_inlock(size_t remoteIndex) {
    auto& remote = _remotes[remoteIndex];

    invariant(!remote.cbHandle.isValid());

    // If mongod returned less docs than the requested batchSize then modify the next getMore
    // request to fetch the remaining docs only. If the remote node has a plan with OR for top k and
    // a full sort as is the case for the OP_QUERY find then this optimization will prevent
    // switching to the full sort plan branch.
    BSONObj cmdObj;

    if (remote.cursorId) {
        auto adjustedBatchSize = _params.batchSize;

        if (_params.batchSize && *_params.batchSize > remote.fetchedCount) {
            adjustedBatchSize = *_params.batchSize - remote.fetchedCount;
        }

        cmdObj = GetMoreRequest(_params.nsString,
                                *remote.cursorId,
                                adjustedBatchSize,
                                _awaitDataTimeout,
                                boost::none,
                                boost::none)
                     .toBSON();
    } else {
        // Do the first time shard host resolution.
        invariant(_params.readPreference);
        Status resolveStatus = remote.resolveShardIdToHostAndPort(*_params.readPreference);
        if (!resolveStatus.isOK()) {
            return resolveStatus;
        }

        remote.fetchedCount = 0;
        cmdObj = *remote.initialCmdObj;
    }

    executor::RemoteCommandRequest request(
        remote.getTargetHost(), _params.nsString.db().toString(), cmdObj, _metadataObj);

    auto callbackStatus = _executor->scheduleRemoteCommand(
        request,
        stdx::bind(
            &AsyncResultsMerger::handleBatchResponse, this, stdx::placeholders::_1, remoteIndex));
    if (!callbackStatus.isOK()) {
        return callbackStatus.getStatus();
    }

    remote.cbHandle = callbackStatus.getValue();
    return Status::OK();
}

StatusWith<executor::TaskExecutor::EventHandle> AsyncResultsMerger::nextEvent() {
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

        // It is illegal to call this method if there is an error received from any shard.
        invariant(remote.status.isOK());

        if (!remote.hasNext() && !remote.exhausted() && !remote.cbHandle.isValid()) {
            // If we already have established a cursor with this remote, and there is no outstanding
            // request for which we have a valid callback handle, then schedule work to retrieve the
            // next batch.
            auto nextBatchStatus = askForNextBatch_inlock(i);
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

    // It's possible that after we told the caller we had no ready results but before the call to
    // this method, new results became available. In this case we have to signal the event right
    // away so that the caller will not block.
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

    // If we have a cursor established, and we get a non-zero cursor id that is not equal to the
    // established cursor id, we will fail the operation.
    if (remote.cursorId && cursorResponse.getCursorId() != 0 &&
        *remote.cursorId != cursorResponse.getCursorId()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Expected cursorid " << *remote.cursorId << " but received "
                                    << cursorResponse.getCursorId());
    }

    return std::move(cursorResponse);
}

void AsyncResultsMerger::handleBatchResponse(
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
        signalCurrentEventIfReady_inlock();

        // Make a best effort to parse the response and retrieve the cursor id. We need the cursor
        // id in order to issue a killCursors command against it.
        if (cbData.response.isOK()) {
            auto cursorResponse = parseCursorResponse(cbData.response.getValue().data, remote);
            if (cursorResponse.isOK()) {
                remote.cursorId = cursorResponse.getValue().getCursorId();
            }
        }

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
    ScopeGuard signaller = MakeGuard(&AsyncResultsMerger::signalCurrentEventIfReady_inlock, this);

    StatusWith<CursorResponse> cursorResponseStatus(
        cbData.response.isOK() ? parseCursorResponse(cbData.response.getValue().data, remote)
                               : cbData.response.getStatus());

    if (!cursorResponseStatus.isOK()) {
        auto shard = remote.getShard();
        if (!shard) {
            remote.status = Status(cursorResponseStatus.getStatus().code(),
                                   str::stream() << "Could not find shard " << *remote.shardId
                                                 << " containing host "
                                                 << remote.getTargetHost().toString());
        } else {
            shard->updateReplSetMonitor(remote.getTargetHost(), cursorResponseStatus.getStatus());

            // Retry initial cursor establishment if possible.  Never retry getMores to avoid
            // accidentally skipping results.
            if (!remote.cursorId && remote.retryCount < kMaxNumFailedHostRetryAttempts &&
                shard->isRetriableError(cursorResponseStatus.getStatus().code(),
                                        Shard::RetryPolicy::kIdempotent)) {
                invariant(remote.shardId);
                LOG(1) << "Initial cursor establishment failed with retriable error and will be "
                          "retried"
                       << causedBy(cursorResponseStatus.getStatus());

                ++remote.retryCount;

                // Since we potentially updated the targeter that the last host it chose might be
                // faulty, the call below may end up getting a different host.
                remote.status = askForNextBatch_inlock(remoteIndex);
                if (remote.status.isOK()) {
                    return;
                }

                // If we end up here, it means we failed to schedule the retry request, which is a
                // more
                // severe error that should not be retried. Just pass through to the error handling
                // logic below.
            } else {
                remote.status = cursorResponseStatus.getStatus();
            }
        }

        // Unreachable host errors are swallowed if the 'allowPartialResults' option is set. We
        // remove the unreachable host entirely from consideration by marking it as exhausted.
        if (_params.isAllowPartialResults) {
            remote.status = Status::OK();

            // Clear the results buffer and cursor id.
            std::queue<BSONObj> emptyBuffer;
            std::swap(remote.docBuffer, emptyBuffer);
            remote.cursorId = 0;
        }

        return;
    }

    // Cursor id successfully established.
    auto cursorResponse = std::move(cursorResponseStatus.getValue());
    remote.cursorId = cursorResponse.getCursorId();
    remote.initialCmdObj = boost::none;

    for (const auto& obj : cursorResponse.getBatch()) {
        // If there's a sort, we're expecting the remote node to give us back a sort key.
        if (!_params.sort.isEmpty() &&
            obj[ClusterClientCursorParams::kSortKeyField].type() != BSONType::Object) {
            remote.status = Status(ErrorCodes::InternalError,
                                   str::stream() << "Missing field '"
                                                 << ClusterClientCursorParams::kSortKeyField
                                                 << "' in document: "
                                                 << obj);
            return;
        }

        remote.docBuffer.push(obj);
        ++remote.fetchedCount;
    }

    // If we're doing a sorted merge, then we have to make sure to put this remote onto the
    // merge queue.
    if (!_params.sort.isEmpty() && !cursorResponse.getBatch().empty()) {
        _mergeQueue.push(remoteIndex);
    }

    // If the cursor is tailable and we just received an empty batch, the next return value should
    // be boost::none in order to indicate the end of the batch.
    if (_params.isTailable && !remote.hasNext()) {
        _eofNext = true;
    }

    // If even after receiving this batch we still don't have anything buffered (i.e. the batchSize
    // was zero), then can schedule work to retrieve the next batch right away.
    //
    // We do not ask for the next batch if the cursor is tailable, as batches received from remote
    // tailable cursors should be passed through to the client without asking for more batches.
    if (!_params.isTailable && !remote.hasNext() && !remote.exhausted()) {
        remote.status = askForNextBatch_inlock(remoteIndex);
        if (!remote.status.isOK()) {
            return;
        }
    }

    // ScopeGuard requires dismiss on success, but we want waiter to be signalled on success as
    // well as failure.
    signaller.Dismiss();
    signalCurrentEventIfReady_inlock();
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

void AsyncResultsMerger::scheduleKillCursors_inlock() {
    invariant(_lifecycleState == kKillStarted);
    invariant(_killCursorsScheduledEvent.isValid());

    for (const auto& remote : _remotes) {
        invariant(!remote.cbHandle.isValid());

        if (remote.status.isOK() && remote.cursorId && !remote.exhausted()) {
            BSONObj cmdObj = KillCursorsRequest(_params.nsString, {*remote.cursorId}).toBSON();

            executor::RemoteCommandRequest request(
                remote.getTargetHost(), _params.nsString.db().toString(), cmdObj);

            _executor->scheduleRemoteCommand(
                request,
                stdx::bind(&AsyncResultsMerger::handleKillCursorsResponse, stdx::placeholders::_1));
        }
    }
}

void AsyncResultsMerger::handleKillCursorsResponse(
    const executor::TaskExecutor::RemoteCommandCallbackArgs& cbData) {
    // We just ignore any killCursors command responses.
}

executor::TaskExecutor::EventHandle AsyncResultsMerger::kill() {
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
        scheduleKillCursors_inlock();
        _lifecycleState = kKillComplete;
        _executor->signalEvent(_killCursorsScheduledEvent);
    }

    return _killCursorsScheduledEvent;
}

//
// AsyncResultsMerger::RemoteCursorData
//

AsyncResultsMerger::RemoteCursorData::RemoteCursorData(ShardId shardId, BSONObj cmdObj)
    : shardId(std::move(shardId)), initialCmdObj(std::move(cmdObj)) {}

AsyncResultsMerger::RemoteCursorData::RemoteCursorData(HostAndPort hostAndPort,
                                                       CursorId establishedCursorId)
    : cursorId(establishedCursorId), _shardHostAndPort(std::move(hostAndPort)) {}

const HostAndPort& AsyncResultsMerger::RemoteCursorData::getTargetHost() const {
    invariant(_shardHostAndPort);
    return *_shardHostAndPort;
}

bool AsyncResultsMerger::RemoteCursorData::hasNext() const {
    return !docBuffer.empty();
}

bool AsyncResultsMerger::RemoteCursorData::exhausted() const {
    return cursorId && (*cursorId == 0);
}

Status AsyncResultsMerger::RemoteCursorData::resolveShardIdToHostAndPort(
    const ReadPreferenceSetting& readPref) {
    invariant(shardId);
    invariant(!cursorId);

    const auto shard = getShard();
    if (!shard) {
        return Status(ErrorCodes::ShardNotFound,
                      str::stream() << "Could not find shard " << *shardId);
    }

    // TODO: Pass down an OperationContext* to use here.
    auto findHostStatus = shard->getTargeter()->findHost(
        readPref, RemoteCommandTargeter::selectFindHostMaxWaitTime(nullptr));
    if (!findHostStatus.isOK()) {
        return findHostStatus.getStatus();
    }

    _shardHostAndPort = std::move(findHostStatus.getValue());

    return Status::OK();
}

std::shared_ptr<Shard> AsyncResultsMerger::RemoteCursorData::getShard() {
    invariant(shardId || _shardHostAndPort);
    if (shardId) {
        return grid.shardRegistry()->getShardNoReload(*shardId);
    } else {
        return grid.shardRegistry()->getShardNoReload(_shardHostAndPort->toString());
    }
}

//
// AsyncResultsMerger::MergingComparator
//

bool AsyncResultsMerger::MergingComparator::operator()(const size_t& lhs, const size_t& rhs) {
    const BSONObj& leftDoc = _remotes[lhs].docBuffer.front();
    const BSONObj& rightDoc = _remotes[rhs].docBuffer.front();

    BSONObj leftDocKey = leftDoc[ClusterClientCursorParams::kSortKeyField].Obj();
    BSONObj rightDocKey = rightDoc[ClusterClientCursorParams::kSortKeyField].Obj();

    // This does not need to sort with a collator, since mongod has already mapped strings to their
    // ICU comparison keys as part of the $sortKey meta projection.
    return leftDocKey.woCompare(rightDocKey, _sort, false /*considerFieldName*/) > 0;
}

}  // namespace mongo
