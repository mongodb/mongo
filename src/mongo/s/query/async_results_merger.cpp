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


#include <algorithm>
#include <boost/cstdint.hpp>
#include <cstdint>
#include <mutex>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/pipeline/change_stream_constants.h"
#include "mongo/db/pipeline/change_stream_invalidation_info.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/getmore_command_gen.h"
#include "mongo/db/query/kill_cursors_gen.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/rpc/metadata.h"
#include "mongo/s/query/async_results_merger.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

constexpr StringData AsyncResultsMerger::kSortKeyField;
const BSONObj AsyncResultsMerger::kWholeSortKeySortPattern = BSON(kSortKeyField << 1);

namespace {

// Maximum number of retries for network and replication NotPrimary errors (per host).
const int kMaxNumFailedHostRetryAttempts = 3;

/**
 * Returns the sort key out of the $sortKey metadata field in 'obj'. The sort key should be
 * formatted as an array with one value per field of the sort pattern:
 *  {..., $sortKey: [<firstSortKeyComponent>, <secondSortKeyComponent>, ...], ...}
 *
 * This function returns the sort key not as an array, but as the equivalent BSONObj:
 *   {"0": <firstSortKeyComponent>, "1": <secondSortKeyComponent>}
 *
 * The return value is allowed to omit the key names, so the caller should not rely on the key names
 * being present. That is, the return value could consist of an object such as
 *   {"": <firstSortKeyComponent>, "": <secondSortKeyComponent>}
 *
 * If 'compareWholeSortKey' is true, then the value inside the $sortKey is directly interpreted as a
 * single-element sort key. For example, given the document
 *   {..., $sortKey: <value>, ...}
 * and 'compareWholeSortKey'=true, this function will return
 *   {"": <value>}
 */
BSONObj extractSortKey(BSONObj obj, bool compareWholeSortKey) {
    auto key = obj[AsyncResultsMerger::kSortKeyField];
    invariant(key);
    if (compareWholeSortKey) {
        return key.wrap();
    }
    invariant(key.type() == BSONType::Array);
    return key.embeddedObject();
}

/**
 * Returns an int less than 0 if 'leftSortKey' < 'rightSortKey', 0 if the two are equal, and an int
 * > 0 if 'leftSortKey' > 'rightSortKey' according to the pattern 'sortKeyPattern'.
 */
int compareSortKeys(BSONObj leftSortKey, BSONObj rightSortKey, BSONObj sortKeyPattern) {
    // This does not need to sort with a collator, since mongod has already mapped strings to their
    // ICU comparison keys as part of the $sortKey meta projection.
    const BSONObj::ComparisonRulesSet rules = 0;  // 'considerFieldNames' flag is not set.
    return leftSortKey.woCompare(rightSortKey, sortKeyPattern, rules);
}

}  // namespace

AsyncResultsMerger::AsyncResultsMerger(OperationContext* opCtx,
                                       std::shared_ptr<executor::TaskExecutor> executor,
                                       AsyncResultsMergerParams params)
    : _opCtx(opCtx),
      _executor(std::move(executor)),
      _params(std::move(params)),
      // This strange initialization is to work around the fact that the IDL does not currently
      // support a default value for an enum. The default tailable mode should be 'kNormal', but
      // since that is not supported we treat boost::none (unspecified) to mean 'kNormal'.
      _tailableMode(_params.getTailableMode().value_or(TailableModeEnum::kNormal)),
      _mergeQueue(MergingComparator(
          _remotes, _params.getSort().value_or(BSONObj()), _params.getCompareWholeSortKey())),
      _promisedMinSortKeys(PromisedMinSortKeyComparator(_params.getSort().value_or(BSONObj()))) {
    if (_params.getTxnNumber()) {
        invariant(_params.getSessionId());
    }

    size_t remoteIndex = 0;
    for (const auto& remote : _params.getRemotes()) {
        _remotes.emplace_back(remote.getHostAndPort(),
                              remote.getCursorResponse().getNSS(),
                              remote.getCursorResponse().getCursorId(),
                              remote.getCursorResponse().getPartialResultsReturned());

        // A remote cannot be flagged as 'partialResultsReturned' if 'allowPartialResults' is false.
        invariant(!(_remotes.back().partialResultsReturned && !_params.getAllowPartialResults()));

        // For the first batch, cursor should never be invalidated.
        tassert(
            5493704, "Found invalidated cursor on the first batch", !_remotes.back().invalidated);

        _remotes.back().shardId = remote.getShardId().toString();

        // We don't check the return value of _addBatchToBuffer here; if there was an error,
        // it will be stored in the remote and the first call to ready() will return true.
        _addBatchToBuffer(WithLock::withoutLock(), remoteIndex, remote.getCursorResponse());
        ++remoteIndex;
    }
    // If this is a change stream, then we expect to have already received PBRTs from every shard.
    invariant(_promisedMinSortKeys.empty() || _promisedMinSortKeys.size() == _remotes.size());
    _setInitialHighWaterMark();
}

void AsyncResultsMerger::_setInitialHighWaterMark() {
    // If we do not have any minimum promised sort keys, this is not a change stream. Return early.
    if (_promisedMinSortKeys.empty()) {
        return;
    }
    // Find the minimum promised sort key whose remote is eligible to contribute a high water mark.
    for (auto&& [minSortKey, remoteId] : _promisedMinSortKeys) {
        if (_remotes[remoteId].eligibleForHighWaterMark) {
            _highWaterMark = minSortKey;
            break;
        }
    }
    // We should always be guaranteed to find an eligible remote, if this is a change stream.
    invariant(!_highWaterMark.isEmpty());
}

AsyncResultsMerger::~AsyncResultsMerger() {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_remotesExhausted(lk) || _lifecycleState == kKillComplete);
}

bool AsyncResultsMerger::remotesExhausted() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _remotesExhausted(lk);
}

bool AsyncResultsMerger::_remotesExhausted(WithLock) const {
    for (const auto& remote : _remotes) {
        // If any remote has been invalidated, we must force the batch-building code to make another
        // attempt to retrieve more results. This will (correctly) throw via _assertNotInvalidated.
        if (!remote.exhausted() || remote.invalidated) {
            return false;
        }
    }

    return true;
}

Status AsyncResultsMerger::setAwaitDataTimeout(Milliseconds awaitDataTimeout) {
    stdx::lock_guard<Latch> lk(_mutex);

    if (_tailableMode != TailableModeEnum::kTailableAndAwaitData) {
        return Status(ErrorCodes::BadValue,
                      "maxTimeMS can only be used with getMore for tailable, awaitData cursors");
    }

    // For sorted tailable awaitData cursors on multiple shards, cap the getMore timeout at 1000ms.
    // This is to ensure that we get a continuous stream of updates from each shard with their most
    // recent optimes, which allows us to return sorted $changeStream results even if some shards
    // are yet to provide a batch of data. If the timeout specified by the client is greater than
    // 1000ms, then it will be enforced elsewhere.
    _awaitDataTimeout =
        (_params.getSort() && _remotes.size() > 1u ? std::min(awaitDataTimeout, Milliseconds{1000})
                                                   : awaitDataTimeout);

    return Status::OK();
}

bool AsyncResultsMerger::ready() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _ready(lk);
}

void AsyncResultsMerger::detachFromOperationContext() {
    stdx::lock_guard<Latch> lk(_mutex);
    _opCtx = nullptr;
    // If we were about ready to return a boost::none because a tailable cursor reached the end of
    // the batch, that should no longer apply to the next use - when we are reattached to a
    // different OperationContext, it signals that the caller is ready for a new batch, and wants us
    // to request a new batch from the tailable cursor.
    _eofNext = false;
}

void AsyncResultsMerger::reattachToOperationContext(OperationContext* opCtx) {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(!_opCtx);
    _opCtx = opCtx;
}

void AsyncResultsMerger::addNewShardCursors(std::vector<RemoteCursor>&& newCursors) {
    stdx::lock_guard<Latch> lk(_mutex);
    // Create a new entry in the '_remotes' list for each new shard, and add the first cursor batch
    // to its buffer. This ensures the shard's initial high water mark is respected, if it exists.
    for (auto&& remote : newCursors) {
        const auto newIndex = _remotes.size();
        _remotes.emplace_back(remote.getHostAndPort(),
                              remote.getCursorResponse().getNSS(),
                              remote.getCursorResponse().getCursorId(),
                              remote.getCursorResponse().getPartialResultsReturned());
        _addBatchToBuffer(lk, newIndex, remote.getCursorResponse());
    }
}

bool AsyncResultsMerger::partialResultsReturned() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return std::any_of(_remotes.begin(), _remotes.end(), [](const auto& remote) {
        return remote.partialResultsReturned;
    });
}

std::size_t AsyncResultsMerger::getNumRemotes() const {
    // Take the lock to guard against shard additions or disconnections.
    stdx::lock_guard<Latch> lk(_mutex);

    // If 'allowPartialResults' is false, the number of participating remotes is constant.
    if (!_params.getAllowPartialResults()) {
        return _remotes.size();
    }
    // Otherwise, discount remotes which failed to connect or disconnected prematurely.
    return std::count_if(_remotes.begin(), _remotes.end(), [](const auto& remote) {
        return !remote.partialResultsReturned;
    });
}

BSONObj AsyncResultsMerger::getHighWaterMark() {
    stdx::lock_guard<Latch> lk(_mutex);
    // At this point, the high water mark may be the resume token of the last document we returned.
    // If no further results are eligible for return, we advance to the minimum promised sort key.
    // If the remote associated with the minimum promised sort key is not currently eligible to
    // provide a high water mark, then we do not advance even if no further results are ready.
    if (auto minPromisedSortKey = _getMinPromisedSortKey(lk); minPromisedSortKey && !_ready(lk)) {
        if (_remotes[minPromisedSortKey->second].eligibleForHighWaterMark) {
            _highWaterMark = minPromisedSortKey->first;
        }
    }
    // The high water mark is stored in sort-key format: {"": <high watermark>}. We only return
    // the <high watermark> part of of the sort key, which looks like {_data: ..., _typeBits: ...}.
    invariant(_highWaterMark.isEmpty() || _highWaterMark.firstElement().type() == BSONType::Object);
    return _highWaterMark.isEmpty() ? BSONObj() : _highWaterMark.firstElement().Obj().getOwned();
}

boost::optional<AsyncResultsMerger::MinSortKeyRemoteIdPair>
AsyncResultsMerger::_getMinPromisedSortKey(WithLock) {
    // We cannot return the minimum promised sort key unless all shards have reported one.
    return _promisedMinSortKeys.size() < _remotes.size() ? boost::optional<MinSortKeyRemoteIdPair>{}
                                                         : *_promisedMinSortKeys.begin();
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

    return _params.getSort() ? _readySorted(lk) : _readyUnsorted(lk);
}

bool AsyncResultsMerger::_readySorted(WithLock lk) {
    if (_tailableMode == TailableModeEnum::kTailableAndAwaitData) {
        return _readySortedTailable(lk);
    }
    // Tailable non-awaitData cursors cannot have a sort.
    invariant(_tailableMode == TailableModeEnum::kNormal);

    for (const auto& remote : _remotes) {
        if (!remote.hasNext() && !remote.exhausted()) {
            return false;
        }
    }

    return true;
}

bool AsyncResultsMerger::_readySortedTailable(WithLock lk) {
    if (_mergeQueue.empty()) {
        return false;
    }

    auto smallestRemote = _mergeQueue.top();
    auto smallestResult = _remotes[smallestRemote].docBuffer.front();
    auto keyWeWantToReturn =
        extractSortKey(*smallestResult.getResult(), _params.getCompareWholeSortKey());
    // We should always have a minPromisedSortKey from every shard in the sorted tailable case.
    auto minPromisedSortKey = _getMinPromisedSortKey(lk);
    invariant(minPromisedSortKey);
    return compareSortKeys(keyWeWantToReturn, minPromisedSortKey->first, *_params.getSort()) <= 0;
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
    stdx::lock_guard<Latch> lk(_mutex);
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

    return _params.getSort() ? _nextReadySorted(lk) : _nextReadyUnsorted(lk);
}

ClusterQueryResult AsyncResultsMerger::_nextReadySorted(WithLock) {
    // Tailable non-awaitData cursors cannot have a sort.
    invariant(_tailableMode != TailableModeEnum::kTailable);

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

    // For sorted tailable awaitData cursors, update the high water mark to the document's sort key.
    if (_tailableMode == TailableModeEnum::kTailableAndAwaitData) {
        if (_remotes[smallestRemote].eligibleForHighWaterMark) {
            _highWaterMark =
                extractSortKey(*front.getResult(), _params.getCompareWholeSortKey()).getOwned();
        }
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

            if (_tailableMode == TailableModeEnum::kTailable &&
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
    auto adjustedBatchSize = _params.getBatchSize();
    if (_params.getBatchSize() && *_params.getBatchSize() > remote.fetchedCount) {
        adjustedBatchSize = *_params.getBatchSize() - remote.fetchedCount;
    }

    GetMoreCommandRequest getMoreRequest(remote.cursorId, remote.cursorNss.coll().toString());
    getMoreRequest.setBatchSize(adjustedBatchSize);
    if (_awaitDataTimeout) {
        getMoreRequest.setMaxTimeMS(
            static_cast<std::int64_t>(durationCount<Milliseconds>(*_awaitDataTimeout)));
    }

    if (_params.getRequestQueryStatsFromRemotes()) {
        getMoreRequest.setIncludeQueryStatsMetrics(true);
    }
    BSONObj cmdObj = getMoreRequest.toBSON({});

    if (_params.getSessionId()) {
        BSONObjBuilder newCmdBob(std::move(cmdObj));

        BSONObjBuilder lsidBob(
            newCmdBob.subobjStart(OperationSessionInfoFromClient::kSessionIdFieldName));
        _params.getSessionId()->serialize(&lsidBob);
        lsidBob.doneFast();

        if (_params.getTxnNumber()) {
            newCmdBob.append(OperationSessionInfoFromClient::kTxnNumberFieldName,
                             *_params.getTxnNumber());
        }

        if (_params.getAutocommit()) {
            newCmdBob.append(OperationSessionInfoFromClient::kAutocommitFieldName,
                             *_params.getAutocommit());
        }

        cmdObj = newCmdBob.obj();
    }

    executor::RemoteCommandRequest request(
        remote.getTargetHost(), remote.cursorNss.dbName(), cmdObj, _opCtx);

    auto callbackStatus =
        _executor->scheduleRemoteCommand(request, [this, remoteIndex](auto const& cbData) {
            stdx::lock_guard<Latch> lk(this->_mutex);
            this->_handleBatchResponse(lk, cbData, remoteIndex);
        });

    if (!callbackStatus.isOK()) {
        return callbackStatus.getStatus();
    }

    remote.cbHandle = callbackStatus.getValue();
    return Status::OK();
}

Status AsyncResultsMerger::scheduleGetMores() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _scheduleGetMores(lk);
}

Status AsyncResultsMerger::_scheduleGetMores(WithLock lk) {
    // Before scheduling more work, check whether the cursor has been invalidated.
    _assertNotInvalidated(lk);

    // Reveal opCtx errors (such as MaxTimeMSExpired) and reflect them in the remote status.
    invariant(_opCtx, "Cannot schedule a getMore without an OperationContext");
    auto interruptStatus = _opCtx->checkForInterruptNoAssert();
    if (!interruptStatus.isOK()) {
        for (size_t i = 0; i < _remotes.size(); ++i) {
            if (!_remotes[i].exhausted()) {
                _cleanUpFailedBatch(lk, interruptStatus, i);
            }
        }
        return interruptStatus;
    }

    // Schedule remote work on hosts for which we need more results.
    for (size_t i = 0; i < _remotes.size(); ++i) {
        auto& remote = _remotes[i];

        if (!remote.status.isOK()) {
            _cleanUpFailedBatch(lk, remote.status, i);
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
    stdx::lock_guard<Latch> lk(_mutex);

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

void AsyncResultsMerger::_assertNotInvalidated(WithLock lk) {
    if (auto minPromisedSortKey = _getMinPromisedSortKey(lk)) {
        const auto& minRemote = _remotes[minPromisedSortKey->second];
        uassert(ChangeStreamInvalidationInfo{minPromisedSortKey->first.firstElement().Obj()},
                "Change stream invalidated",
                !(minRemote.invalidated && !_ready(lk)));
    }
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

void AsyncResultsMerger::_updateRemoteMetadata(WithLock lk,
                                               size_t remoteIndex,
                                               const CursorResponse& response) {
    // Update the cursorId; it is sent as '0' when the cursor has been exhausted on the shard.
    auto& remote = _remotes[remoteIndex];
    remote.cursorId = response.getCursorId();

    // If the response indicates that the cursor has been invalidated, mark the corresponding
    // remote as invalidated. This also signifies that the shard cursor has been closed.
    remote.invalidated = response.getInvalidated();
    tassert(5493705,
            "Unexpectedly encountered invalidated cursor with non-zero ID",
            !(remote.invalidated && remote.cursorId > 0));

    if (response.getPostBatchResumeToken()) {
        // We only expect to see this for change streams.
        invariant(_params.getSort());
        invariant(SimpleBSONObjComparator::kInstance.evaluate(*_params.getSort() ==
                                                              change_stream_constants::kSortSpec));

        // The postBatchResumeToken should never be empty.
        invariant(!response.getPostBatchResumeToken()->isEmpty());

        // Note that the PBRT is an object of format {_data: ..., _typeBits: ...} that we must wrap
        // in a sort key so that it can compare correctly with sort keys from other streams.
        auto newMinSortKey = BSON("" << *response.getPostBatchResumeToken());

        // Determine whether the new batch is eligible to provide a high water mark resume token.
        remote.eligibleForHighWaterMark =
            _checkHighWaterMarkEligibility(lk, newMinSortKey, remote, response);

        // The most recent minimum sort key should never be smaller than the previous promised
        // minimum sort key for this remote, if a previous promised minimum sort key exists.
        if (auto& oldMinSortKey = remote.promisedMinSortKey) {
            invariant(compareSortKeys(newMinSortKey, *oldMinSortKey, *_params.getSort()) >= 0);
            invariant(_promisedMinSortKeys.size() <= _remotes.size());
            _promisedMinSortKeys.erase({*oldMinSortKey, remoteIndex});
        }
        _promisedMinSortKeys.insert({newMinSortKey, remoteIndex});
        remote.promisedMinSortKey = newMinSortKey;
    }
}

bool AsyncResultsMerger::_checkHighWaterMarkEligibility(WithLock,
                                                        BSONObj newMinSortKey,
                                                        const RemoteCursorData& remote,
                                                        const CursorResponse& response) {
    // If the cursor is not on the "config.shards" namespace, then it is a normal shard cursor.
    // These cursors are always eligible to provide a high water mark resume token.
    if (remote.cursorNss != NamespaceString::kConfigsvrShardsNamespace) {
        return true;
    }

    // If we are here, the cursor is on the "config.shards" namespace. This is an internal cursor
    // which monitors for the addition of new shards. There are two special cases which we must
    // handle for this cursor:
    //
    //   - The user specified a 'startAtOperationTime' in the future. This is a problem because the
    //     config cursor must always be opened at the current clusterTime, to ensure that it detects
    //     all shards that are added after the change stream is dispatched. We must make sure that
    //     the high water mark ignores the config cursor's minimum promised sort keys, otherwise we
    //     will end up returning a token that is earlier than the start time requested by the user.
    //
    //   - The cursor returns a "shard added" event. All events produced by the config cursor are
    //     handled and swallowed internally by the stream. We therefore do not want to allow their
    //     resume tokens to be exposed to the user via the postBatchResumeToken mechanism, since
    //     these token are not actually resumable. See SERVER-47810 for further details.

    // If the current high water mark is ahead of the config cursor, it implies that the client has
    // opened a stream with a startAtOperationTime in the future. We should hold the high water mark
    // at the user-specified start time until the config server catches up to it. The config cursor
    // is therefore not eligible to provide a new high water mark.
    if (compareSortKeys(newMinSortKey, _highWaterMark, *_params.getSort()) < 0) {
        return false;
    }
    // If the config server returns an event which indicates a change in the cluster topology, it
    // will be swallowed by the stream. It will not be returned to the user, and it should not be
    // eligible to become the high water mark either.
    if (!response.getBatch().empty()) {
        return false;
    }
    // If we are here, then the only remaining reason not to mark this batch as eligible is if the
    // current batch's sort key is the same as the last batch's, and the last batch was ineligible.
    // Therefore, if the previous batch was eligible, this batch is as well.
    if (remote.eligibleForHighWaterMark) {
        return true;
    }
    // If we are here, then either the last batch we received was ineligible for one of the reasons
    // outlined above, or this is the first batch we have ever received for this cursor. If this is
    // the first batch, then we always mark the config cursor as ineligible so that the initial high
    // water mark will be taken from one of the shards instead. If we received an ineligible batch
    // last time, then the current batch is only eligible if its sort key is greater than the last.
    return remote.promisedMinSortKey &&
        compareSortKeys(newMinSortKey, *remote.promisedMinSortKey, *_params.getSort()) > 0;
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

    // If this is the last callback to run then we are ready to free the ARM. We signal the
    // '_killCompleteInfo', which the caller of kill() may be waiting on.
    if (!_haveOutstandingBatchRequests(lk)) {
        invariant(_killCompleteInfo);
        _killCompleteInfo->signalFutures();

        _lifecycleState = kKillComplete;
    }
}

void AsyncResultsMerger::_cleanUpFailedBatch(WithLock lk, Status status, size_t remoteIndex) {
    auto& remote = _remotes[remoteIndex];
    remote.status = std::move(status);
    // Unreachable host errors are swallowed if the 'allowPartialResults' option is set. We
    // remove the unreachable host entirely from consideration by marking it as exhausted.
    //
    // The ExchangePassthrough error code is an internal-only error code used specifically to
    // communicate that an error has occurred, but some other thread is responsible for returning
    // the error to the user. In order to avoid polluting the user's error message, we ignore such
    // errors with the expectation that all outstanding cursors will be closed promptly.
    if (_params.getAllowPartialResults() || remote.status == ErrorCodes::ExchangePassthrough) {
        // Clear the results buffer and cursor id, and set 'partialResultsReturned' if appropriate.
        remote.partialResultsReturned = (remote.status != ErrorCodes::ExchangePassthrough);
        std::queue<ClusterQueryResult> emptyBuffer;
        std::swap(remote.docBuffer, emptyBuffer);
        remote.status = Status::OK();
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
        _cleanUpFailedBatch(lk,
                            cursorResponseStatus.getStatus().withContext(
                                "Error on remote shard " + remote.shardHostAndPort.toString()),
                            remoteIndex);
        return;
    }

    CursorResponse cursorResponse = std::move(cursorResponseStatus.getValue());

    // Update the cursorId; it is sent as '0' when the cursor has been exhausted on the shard.
    remote.cursorId = cursorResponse.getCursorId();

    // Aggregate remote cursor metrics (if any) into the OpDebug metrics.
    if (const auto& cursorMetrics = cursorResponse.getCursorMetrics(); _opCtx && cursorMetrics) {
        CurOp::get(_opCtx)->debug().aggregateCursorMetrics(*cursorMetrics);
    }

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
    if (_tailableMode == TailableModeEnum::kTailable && !remote.hasNext()) {
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
    _updateRemoteMetadata(lk, remoteIndex, response);
    for (const auto& obj : response.getBatch()) {
        // If there's a sort, we're expecting the remote node to have given us back a sort key.
        if (_params.getSort()) {
            auto key = obj[AsyncResultsMerger::kSortKeyField];
            if (!key) {
                remote.status =
                    Status(ErrorCodes::InternalError,
                           str::stream() << "Missing field '" << AsyncResultsMerger::kSortKeyField
                                         << "' in document: " << obj);
                return false;
            } else if (!_params.getCompareWholeSortKey() && !key.isABSONObj()) {
                remote.status =
                    Status(ErrorCodes::InternalError,
                           str::stream() << "Field '" << AsyncResultsMerger::kSortKeyField
                                         << "' was not of type Object in document: " << obj);
                return false;
            }
        }

        ClusterQueryResult result(obj, remote.shardId);
        remote.docBuffer.push(result);
        ++remote.fetchedCount;
    }

    // If we're doing a sorted merge, then we have to make sure to put this remote onto the merge
    // queue.
    if (_params.getSort() && !response.getBatch().empty()) {
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

void AsyncResultsMerger::_scheduleKillCursors(WithLock lk, OperationContext* opCtx) {
    invariant(_killCompleteInfo);

    for (const auto& remote : _remotes) {
        if (_shouldKillRemote(lk, remote)) {
            BSONObj cmdObj =
                KillCursorsCommandRequest(_params.getNss(), {remote.cursorId}).toBSON(BSONObj{});

            executor::RemoteCommandRequest::Options options;
            options.fireAndForget = true;
            executor::RemoteCommandRequest request(remote.getTargetHost(),
                                                   _params.getNss().dbName(),
                                                   cmdObj,
                                                   rpc::makeEmptyMetadata(),
                                                   opCtx,
                                                   options);
            // The 'RemoteCommandRequest' takes the remaining time from the 'opCtx' parameter. If
            // the cursor was killed due to a maxTimeMs timeout, the remaining time will be 0, and
            // the remote request will not be sent. To avoid this, we remove the timeout for the
            // remote 'killCursor' command.
            request.timeout = executor::RemoteCommandRequestBase::kNoTimeout;

            // Send kill request; discard callback handle, if any, or failure report, if not.
            _executor->scheduleRemoteCommand(request, [](auto const&) {}).getStatus().ignore();
        }
    }
}

bool AsyncResultsMerger::_shouldKillRemote(WithLock, const RemoteCursorData& remote) {
    static const std::set<ErrorCodes::Error> kCursorAlreadyDeadCodes = {
        ErrorCodes::QueryPlanKilled, ErrorCodes::CursorKilled, ErrorCodes::CursorNotFound};
    return remote.cursorId && !remote.exhausted() &&
        !kCursorAlreadyDeadCodes.count(remote.status.code());
}

stdx::shared_future<void> AsyncResultsMerger::kill(OperationContext* opCtx) {
    stdx::lock_guard<Latch> lk(_mutex);

    if (_killCompleteInfo) {
        invariant(_lifecycleState != kAlive);
        return _killCompleteInfo->getFuture();
    }

    invariant(_lifecycleState == kAlive);
    _lifecycleState = kKillStarted;

    // Create "_killCompleteInfo", which we will signal as soon as all of our callbacks have
    // finished running.
    _killCompleteInfo.emplace();

    _scheduleKillCursors(lk, opCtx);

    if (!_haveOutstandingBatchRequests(lk)) {
        _lifecycleState = kKillComplete;
        // Signal the future right now, as there's nothing to wait for.
        _killCompleteInfo->signalFutures();
        return _killCompleteInfo->getFuture();
    }

    // Cancel all of our callbacks. Once they all complete, the event will be signaled.
    for (const auto& remote : _remotes) {
        if (remote.cbHandle.isValid()) {
            _executor->cancel(remote.cbHandle);
        }
    }
    return _killCompleteInfo->getFuture();
}

//
// AsyncResultsMerger::RemoteCursorData
//

AsyncResultsMerger::RemoteCursorData::RemoteCursorData(HostAndPort hostAndPort,
                                                       NamespaceString cursorNss,
                                                       CursorId establishedCursorId,
                                                       bool partialResultsReturned)
    : cursorId(establishedCursorId),
      cursorNss(std::move(cursorNss)),
      shardHostAndPort(std::move(hostAndPort)),
      partialResultsReturned(partialResultsReturned) {
    // If the 'partialResultsReturned' flag is set, the cursorId must be zero (closed).
    invariant(!(partialResultsReturned && cursorId != 0));
}

const HostAndPort& AsyncResultsMerger::RemoteCursorData::getTargetHost() const {
    return shardHostAndPort;
}

bool AsyncResultsMerger::RemoteCursorData::hasNext() const {
    return !docBuffer.empty();
}

bool AsyncResultsMerger::RemoteCursorData::exhausted() const {
    return cursorId == 0;
}

//
// AsyncResultsMerger::MergingComparator
//

bool AsyncResultsMerger::MergingComparator::operator()(const size_t& lhs, const size_t& rhs) {
    const ClusterQueryResult& leftDoc = _remotes[lhs].docBuffer.front();
    const ClusterQueryResult& rightDoc = _remotes[rhs].docBuffer.front();

    return compareSortKeys(extractSortKey(*leftDoc.getResult(), _compareWholeSortKey),
                           extractSortKey(*rightDoc.getResult(), _compareWholeSortKey),
                           _sort) > 0;
}

bool AsyncResultsMerger::PromisedMinSortKeyComparator::operator()(
    const MinSortKeyRemoteIdPair& lhs, const MinSortKeyRemoteIdPair& rhs) const {
    auto sortKeyComp = compareSortKeys(lhs.first, rhs.first, _sort);
    return sortKeyComp < 0 || (sortKeyComp == 0 && lhs.second < rhs.second);
}

}  // namespace mongo
