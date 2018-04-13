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

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/db/query/killcursors_request.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

// Maximum number of retries for network and replication notMaster errors (per host).
const int kMaxNumFailedHostRetryAttempts = 3;

/**
 * Returns the sort key out of the $sortKey metadata field in 'obj'. This object is of the form
 * {'': 'firstSortKey', '': 'secondSortKey', ...}.
 */
BSONObj extractSortKey(BSONObj obj) {
    auto key = obj[ClusterClientCursorParams::kSortKeyField];
    invariant(key.type() == BSONType::Object);
    return key.Obj();
}

/**
 * Returns an int less than 0 if 'leftSortKey' < 'rightSortKey', 0 if the two are equal, and an int
 * > 0 if 'leftSortKey' > 'rightSortKey' according to the pattern 'sortKeyPattern'.
 */
int compareSortKeys(BSONObj leftSortKey, BSONObj rightSortKey, BSONObj sortKeyPattern) {
    // This does not need to sort with a collator, since mongod has already mapped strings to their
    // ICU comparison keys as part of the $sortKey meta projection.
    const bool considerFieldName = false;
    return leftSortKey.woCompare(rightSortKey, sortKeyPattern, considerFieldName);
}

}  // namespace

AsyncResultsMerger::AsyncResultsMerger(OperationContext* opCtx,
                                       executor::TaskExecutor* executor,
                                       ClusterClientCursorParams* params)
    : _opCtx(opCtx),
      _executor(executor),
      _params(params),
      _mergeQueue(MergingComparator(_remotes, _params->sort)) {
    size_t remoteIndex = 0;
    for (const auto& remote : _params->remotes) {
        _remotes.emplace_back(remote.hostAndPort,
                              remote.cursorResponse.getNSS(),
                              remote.cursorResponse.getCursorId());

        // We don't check the return value of _addBatchToBuffer here; if there was an error,
        // it will be stored in the remote and the first call to ready() will return true.
        _addBatchToBuffer(WithLock::withoutLock(), remoteIndex, remote.cursorResponse);
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
    invariant(_remotesExhausted(lk) || _lifecycleState == kKillComplete);
}

bool AsyncResultsMerger::remotesExhausted() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _remotesExhausted(lk);
}

bool AsyncResultsMerger::_remotesExhausted(WithLock) {
    for (const auto& remote : _remotes) {
        if (!remote.exhausted()) {
            return false;
        }
    }

    return true;
}

Status AsyncResultsMerger::setAwaitDataTimeout(Milliseconds awaitDataTimeout) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    if (_params->tailableMode != TailableMode::kTailableAndAwaitData) {
        return Status(ErrorCodes::BadValue,
                      "maxTimeMS can only be used with getMore for tailable, awaitData cursors");
    }

    // For sorted tailable awaitData cursors on multiple shards, cap the getMore timeout at 1000ms.
    // This is to ensure that we get a continuous stream of updates from each shard with their most
    // recent optimes, which allows us to return sorted $changeStream results even if some shards
    // are yet to provide a batch of data. If the timeout specified by the client is greater than
    // 1000ms, then it will be enforced elsewhere.
    _awaitDataTimeout = (!_params->sort.isEmpty() && _remotes.size() > 1u
                             ? std::min(awaitDataTimeout, Milliseconds{1000})
                             : awaitDataTimeout);

    return Status::OK();
}

bool AsyncResultsMerger::ready() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _ready(lk);
}

void AsyncResultsMerger::detachFromOperationContext() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _opCtx = nullptr;
    // If we were about ready to return a boost::none because a tailable cursor reached the end of
    // the batch, that should no longer apply to the next use - when we are reattached to a
    // different OperationContext, it signals that the caller is ready for a new batch, and wants us
    // to request a new batch from the tailable cursor.
    _eofNext = false;
}

void AsyncResultsMerger::reattachToOperationContext(OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(!_opCtx);
    _opCtx = opCtx;
}

void AsyncResultsMerger::addNewShardCursors(
    const std::vector<ClusterClientCursorParams::RemoteCursor>& newCursors) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    for (auto&& remote : newCursors) {
        _remotes.emplace_back(remote.hostAndPort,
                              remote.cursorResponse.getNSS(),
                              remote.cursorResponse.getCursorId());
    }
}

bool AsyncResultsMerger::_ready(WithLock lk) {
    if (_lifecycleState != kAlive) {
        return true;
    }

    if (_eofNext) {
        // Mark this operation as ready to return boost::none due to reaching the end of a batch of
        // results from a tailable cursor.
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
    return hasSort ? _readySorted(lk) : _readyUnsorted(lk);
}

bool AsyncResultsMerger::_readySorted(WithLock lk) {
    if (_params->tailableMode == TailableMode::kTailableAndAwaitData) {
        return _readySortedTailable(lk);
    }
    // Tailable non-awaitData cursors cannot have a sort.
    invariant(_params->tailableMode == TailableMode::kNormal);

    for (const auto& remote : _remotes) {
        if (!remote.hasNext() && !remote.exhausted()) {
            return false;
        }
    }

    return true;
}

bool AsyncResultsMerger::_readySortedTailable(WithLock) {
    if (_mergeQueue.empty()) {
        return false;
    }

    auto smallestRemote = _mergeQueue.top();
    auto smallestResult = _remotes[smallestRemote].docBuffer.front();
    auto keyWeWantToReturn = extractSortKey(*smallestResult.getResult());
    for (const auto& remote : _remotes) {
        if (!remote.promisedMinSortKey) {
            // In order to merge sorted tailable cursors, we need this value to be populated.
            return false;
        }
        if (compareSortKeys(keyWeWantToReturn, *remote.promisedMinSortKey, _params->sort) > 0) {
            // The key we want to return is not guaranteed to be smaller than future results from
            // this remote, so we can't yet return it.
            return false;
        }
    }
    return true;
}

bool AsyncResultsMerger::_readyUnsorted(WithLock) {
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
    dassert(_ready(lk));
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
    return hasSort ? _nextReadySorted(lk) : _nextReadyUnsorted(lk);
}

ClusterQueryResult AsyncResultsMerger::_nextReadySorted(WithLock) {
    // Tailable non-awaitData cursors cannot have a sort.
    invariant(_params->tailableMode != TailableMode::kTailable);

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

ClusterQueryResult AsyncResultsMerger::_nextReadyUnsorted(WithLock) {
    size_t remotesAttempted = 0;
    while (remotesAttempted < _remotes.size()) {
        // It is illegal to call this method if there is an error received from any shard.
        invariant(_remotes[_gettingFromRemote].status.isOK());

        if (_remotes[_gettingFromRemote].hasNext()) {
            ClusterQueryResult front = _remotes[_gettingFromRemote].docBuffer.front();
            _remotes[_gettingFromRemote].docBuffer.pop();

            if (_params->tailableMode == TailableMode::kTailable &&
                !_remotes[_gettingFromRemote].hasNext()) {
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

Status AsyncResultsMerger::_askForNextBatch(WithLock, size_t remoteIndex) {
    invariant(_opCtx, "Cannot schedule a getMore without an OperationContext");
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

    BSONObj cmdObj = GetMoreRequest(remote.cursorNss,
                                    remote.cursorId,
                                    adjustedBatchSize,
                                    _awaitDataTimeout,
                                    boost::none,
                                    boost::none)
                         .toBSON();

    executor::RemoteCommandRequest request(
        remote.getTargetHost(), _params->nsString.db().toString(), cmdObj, _metadataObj, _opCtx);

    auto callbackStatus =
        _executor->scheduleRemoteCommand(request, [this, remoteIndex](auto const& cbData) {
            stdx::lock_guard<stdx::mutex> lk(this->_mutex);
            this->_handleBatchResponse(lk, cbData, remoteIndex);
        });

    if (!callbackStatus.isOK()) {
        return callbackStatus.getStatus();
    }

    remote.cbHandle = callbackStatus.getValue();
    return Status::OK();
}

Status AsyncResultsMerger::scheduleGetMores() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _scheduleGetMores(lk);
}

Status AsyncResultsMerger::_scheduleGetMores(WithLock lk) {
    // Schedule remote work on hosts for which we need more results.
    for (size_t i = 0; i < _remotes.size(); ++i) {
        auto& remote = _remotes[i];

        if (!remote.status.isOK()) {
            return remote.status;
        }

        if (!remote.hasNext() && !remote.exhausted() && !remote.cbHandle.isValid()) {
            // If this remote is not exhausted and there is no outstanding request for it, schedule
            // work to retrieve the next batch.
            auto nextBatchStatus = _askForNextBatch(lk, i);
            if (!nextBatchStatus.isOK()) {
                return nextBatchStatus;
            }
        }
    }
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

    auto getMoresStatus = _scheduleGetMores(lk);
    if (!getMoresStatus.isOK()) {
        return getMoresStatus;
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
    _signalCurrentEventIfReady(lk);
    return eventToReturn;
}

StatusWith<CursorResponse> AsyncResultsMerger::_parseCursorResponse(
    const BSONObj& responseObj, const RemoteCursorData& remote) {

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

void AsyncResultsMerger::updateRemoteMetadata(RemoteCursorData* remote,
                                              const CursorResponse& response) {
    // Update the cursorId; it is sent as '0' when the cursor has been exhausted on the shard.
    remote->cursorId = response.getCursorId();
    if (response.getLastOplogTimestamp() && !response.getLastOplogTimestamp()->isNull()) {
        // We only expect to see this for change streams.
        invariant(SimpleBSONObjComparator::kInstance.evaluate(
            _params->sort == DocumentSourceChangeStream::kSortSpec));

        auto newLatestTimestamp = *response.getLastOplogTimestamp();
        if (remote->promisedMinSortKey) {
            auto existingLatestTimestamp = remote->promisedMinSortKey->firstElement().timestamp();
            if (existingLatestTimestamp == newLatestTimestamp) {
                // Nothing to update.
                return;
            }
            // The most recent oplog timestamp should never be smaller than the timestamp field of
            // the previous min sort key for this remote, if one exists.
            invariant(existingLatestTimestamp < newLatestTimestamp);
        }

        // Our new minimum promised sort key is the first key whose timestamp matches the most
        // recent reported oplog timestamp.
        auto newPromisedMin =
            BSON("" << *response.getLastOplogTimestamp() << "" << MINKEY << "" << MINKEY);

        // The promised min sort key should never be smaller than any results returned. If the
        // last entry in the batch is also the most recent entry in the oplog, then its sort key
        // of {lastOplogTimestamp, uuid, docID} will be greater than the artificial promised min
        // sort key of {lastOplogTimestamp, MINKEY, MINKEY}.
        auto maxSortKeyFromResponse =
            (response.getBatch().empty() ? BSONObj() : extractSortKey(response.getBatch().back()));

        remote->promisedMinSortKey =
            (compareSortKeys(
                 newPromisedMin, maxSortKeyFromResponse, DocumentSourceChangeStream::kSortSpec) < 0
                 ? maxSortKeyFromResponse.getOwned()
                 : newPromisedMin.getOwned());
    }
}

void AsyncResultsMerger::_handleBatchResponse(WithLock lk,
                                              CbData const& cbData,
                                              size_t remoteIndex) {
    // Got a response from remote, so indicate we are no longer waiting for one.
    _remotes[remoteIndex].cbHandle = executor::TaskExecutor::CallbackHandle();

    //  On shutdown, there is no need to process the response.
    if (_lifecycleState != kAlive) {
        _signalCurrentEventIfReady(lk);  // First, wake up anyone waiting on '_currentEvent'.
        _cleanUpKilledBatch(lk);
        return;
    }
    try {
        _processBatchResults(lk, cbData.response, remoteIndex);
    } catch (DBException const& e) {
        _remotes[remoteIndex].status = e.toStatus();
    }
    _signalCurrentEventIfReady(lk);  // Wake up anyone waiting on '_currentEvent'.
}

void AsyncResultsMerger::_cleanUpKilledBatch(WithLock lk) {
    invariant(_lifecycleState == kKillStarted);

    // If we're killed and we're not waiting on any more batches to come back, then we are ready
    // to kill the cursors on the remote hosts and clean up this cursor. Schedule the killCursors
    // command and signal that this cursor is now safe to destroy.  We must not touch this object
    // again after dropping the lock, because 'this' could become invalid immediately.
    if (!_haveOutstandingBatchRequests(lk)) {
        // If the event handle is invalid, then the executor is in the middle of shutting down,
        // and we can't schedule any more work for it to complete.
        if (_killCursorsScheduledEvent.isValid()) {
            _scheduleKillCursors(lk, _opCtx);
            _executor->signalEvent(_killCursorsScheduledEvent);
        }

        _lifecycleState = kKillComplete;
    }
}

void AsyncResultsMerger::_cleanUpFailedBatch(WithLock lk, Status status, size_t remoteIndex) {
    auto& remote = _remotes[remoteIndex];
    remote.status = std::move(status);
    // Unreachable host errors are swallowed if the 'allowPartialResults' option is set. We
    // remove the unreachable host entirely from consideration by marking it as exhausted.
    if (_params->isAllowPartialResults) {
        remote.status = Status::OK();

        // Clear the results buffer and cursor id.
        std::queue<ClusterQueryResult> emptyBuffer;
        std::swap(remote.docBuffer, emptyBuffer);
        remote.cursorId = 0;
    }
}

void AsyncResultsMerger::_processBatchResults(WithLock lk,
                                              CbResponse const& response,
                                              size_t remoteIndex) {
    auto& remote = _remotes[remoteIndex];
    if (!response.isOK()) {
        _cleanUpFailedBatch(lk, response.status, remoteIndex);
        return;
    }
    auto cursorResponseStatus = _parseCursorResponse(response.data, remote);
    if (!cursorResponseStatus.isOK()) {
        _cleanUpFailedBatch(lk, cursorResponseStatus.getStatus(), remoteIndex);
        return;
    }

    CursorResponse cursorResponse = std::move(cursorResponseStatus.getValue());

    // Update the cursorId; it is sent as '0' when the cursor has been exhausted on the shard.
    remote.cursorId = cursorResponse.getCursorId();

    // Save the batch in the remote's buffer.
    if (!_addBatchToBuffer(lk, remoteIndex, cursorResponse)) {
        return;
    }

    // If the cursor is tailable and we just received an empty batch, the next return value should
    // be boost::none in order to indicate the end of the batch. We do not ask for the next batch if
    // the cursor is tailable, as batches received from remote tailable cursors should be passed
    // through to the client as-is.
    // (Note: tailable cursors are only valid on unsharded collections, so the end of the batch from
    // one shard means the end of the overall batch).
    if (_params->tailableMode == TailableMode::kTailable && !remote.hasNext()) {
        invariant(_remotes.size() == 1);
        _eofNext = true;
    } else if (!remote.hasNext() && !remote.exhausted() && _lifecycleState == kAlive && _opCtx) {
        // If this is normal or tailable-awaitData cursor and we still don't have anything buffered
        // after receiving this batch, we can schedule work to retrieve the next batch right away.
        // Be careful only to do this when '_opCtx' is non-null, since it is illegal to schedule a
        // remote command on a user's behalf without a non-null OperationContext.
        remote.status = _askForNextBatch(lk, remoteIndex);
    }
}

bool AsyncResultsMerger::_addBatchToBuffer(WithLock lk,
                                           size_t remoteIndex,
                                           const CursorResponse& response) {
    auto& remote = _remotes[remoteIndex];
    updateRemoteMetadata(&remote, response);
    for (const auto& obj : response.getBatch()) {
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
    if (!_params->sort.isEmpty() && !response.getBatch().empty()) {
        _mergeQueue.push(remoteIndex);
    }
    return true;
}

void AsyncResultsMerger::_signalCurrentEventIfReady(WithLock lk) {
    if (_ready(lk) && _currentEvent.isValid()) {
        // To prevent ourselves from signalling the event twice, we set '_currentEvent' as
        // invalid after signalling it.
        _executor->signalEvent(_currentEvent);
        _currentEvent = executor::TaskExecutor::EventHandle();
    }
}

bool AsyncResultsMerger::_haveOutstandingBatchRequests(WithLock) {
    for (const auto& remote : _remotes) {
        if (remote.cbHandle.isValid()) {
            return true;
        }
    }

    return false;
}

void AsyncResultsMerger::_scheduleKillCursors(WithLock, OperationContext* opCtx) {
    invariant(_lifecycleState == kKillStarted);
    invariant(_killCursorsScheduledEvent.isValid());

    for (const auto& remote : _remotes) {
        invariant(!remote.cbHandle.isValid());

        if (remote.status.isOK() && remote.cursorId && !remote.exhausted()) {
            BSONObj cmdObj = KillCursorsRequest(_params->nsString, {remote.cursorId}).toBSON();

            executor::RemoteCommandRequest request(
                remote.getTargetHost(), _params->nsString.db().toString(), cmdObj, opCtx);

            // Send kill request; discard callback handle, if any, or failure report, if not.
            Status s = _executor->scheduleRemoteCommand(request, [](auto const&) {}).getStatus();
            std::move(s).ignore();
        }
    }
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
        if (!_haveOutstandingBatchRequests(lk)) {
            _lifecycleState = kKillComplete;
        }
        return executor::TaskExecutor::EventHandle();
    }
    fassertStatusOK(28716, statusWithEvent);
    _killCursorsScheduledEvent = statusWithEvent.getValue();

    // If we're not waiting for responses from remotes, we can schedule killCursors commands on the
    // remotes now. Otherwise, we have to wait until all responses are back because a cursor that
    // is active (pinned) on a remote cannot be killed through killCursors.
    if (!_haveOutstandingBatchRequests(lk)) {
        _scheduleKillCursors(lk, opCtx);
        _lifecycleState = kKillComplete;
        _executor->signalEvent(_killCursorsScheduledEvent);
    }

    return _killCursorsScheduledEvent;
}

//
// AsyncResultsMerger::RemoteCursorData
//

AsyncResultsMerger::RemoteCursorData::RemoteCursorData(HostAndPort hostAndPort,
                                                       NamespaceString cursorNss,
                                                       CursorId establishedCursorId)
    : cursorId(establishedCursorId),
      cursorNss(std::move(cursorNss)),
      shardHostAndPort(std::move(hostAndPort)) {}

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

    return compareSortKeys(extractSortKey(*leftDoc.getResult()),
                           extractSortKey(*rightDoc.getResult()),
                           _sort) > 0;
}

}  // namespace mongo
