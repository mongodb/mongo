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
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/client_cursor/kill_cursors_gen.h"
#include "mongo/db/query/getmore_command_gen.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/rpc/metadata.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/s/query/exec/async_results_merger.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

const BSONObj AsyncResultsMerger::kWholeSortKeySortPattern =
    BSON(AsyncResultsMerger::kSortKeyField << 1);

namespace {

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
    constexpr BSONObj::ComparisonRulesSet rules = 0;  // 'considerFieldNames' flag is not set.
    return leftSortKey.woCompare(rightSortKey, sortKeyPattern, rules);
}

void processAdditionalTransactionParticipantFromResponse(
    OperationContext* opCtx,
    const ShardId& shardId,
    const BSONObj& originalResponse,
    const ServerGlobalParams::FCVSnapshot& fcvSnapshot) {
    if (gFeatureFlagAllowAdditionalParticipants.isEnabled(fcvSnapshot)) {
        transaction_request_sender_details::processReplyMetadataForAsyncGetMore(
            opCtx, shardId, originalResponse);
    }
}

void processAdditionalTransactionParticipantFromResponse(OperationContext* opCtx,
                                                         const ShardId& shardId,
                                                         const BSONObj& originalResponse) {
    const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    processAdditionalTransactionParticipantFromResponse(
        opCtx, shardId, originalResponse, fcvSnapshot);
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
      _mergeQueue(MergingComparator(_params.getSort().value_or(BSONObj()),
                                    _params.getCompareWholeSortKey())),
      _promisedMinSortKeys(PromisedMinSortKeyComparator(_params.getSort().value_or(BSONObj()))) {
    if (_params.getTxnNumber()) {
        invariant(_params.getSessionId());
    }

    // Build a list of remote cursors from the parameters we got, and store them in '_remotes'.
    for (const auto& remoteCursor : _params.getRemotes()) {
        auto remote = _buildRemote(WithLock::withoutLock(), remoteCursor);

        // A remote cannot be flagged as 'partialResultsReturned' if 'allowPartialResults' is false.
        invariant(!(remote->partialResultsReturned && !_params.getAllowPartialResults()));
        // For the first batch, cursor should never be invalidated.
        tassert(5493704, "Found invalidated cursor on the first batch", !remote->invalidated);

        _remotes.push_back(std::move(remote));
    }
    // If this is a change stream, then we expect to have already received PBRTs from every shard.
    invariant(_promisedMinSortKeys.empty() || _promisedMinSortKeys.size() == _remotes.size());
    _setInitialHighWaterMark();
}

AsyncResultsMerger::~AsyncResultsMerger() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(_lifecycleState == kKillComplete || _remotesExhausted(lk));
}

const AsyncResultsMergerParams& AsyncResultsMerger::params() const {
    return _params;
}

bool AsyncResultsMerger::remotesExhausted() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _remotesExhausted(lk);
}

bool AsyncResultsMerger::_remotesExhausted(WithLock) const {
    return std::all_of(_remotes.begin(), _remotes.end(), [](const auto& remote) {
        // If any remote has been invalidated, we must force the batch-building code to make another
        // attempt to retrieve more results. This will (correctly) throw via _assertNotInvalidated.
        return remote->exhausted() && !remote->invalidated;
    });
}

Status AsyncResultsMerger::setAwaitDataTimeout(Milliseconds awaitDataTimeout) {
    if (_tailableMode != TailableModeEnum::kTailableAndAwaitData) {
        return Status(ErrorCodes::BadValue,
                      "maxTimeMS can only be used with getMore for tailable, awaitData cursors");
    }

    stdx::lock_guard<stdx::mutex> lk(_mutex);
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
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _ready(lk);
}

void AsyncResultsMerger::detachFromOperationContext() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    // Before we're done detaching we do a last attempt to process any additional responses
    // received. This ensures that the only possible state for ARM to have unprocessed responses is
    // when it's been stashed between cursor checkouts or after it's been marked as killed.
    _processAdditionalTransactionParticipants(_opCtx);

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

void AsyncResultsMerger::addNewShardCursors(std::vector<RemoteCursor>&& newCursors) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    // Create a new entry in the '_remotes' list for each new shard, and add the first cursor batch
    // to its buffer. This ensures the shard's initial high water mark is respected, if it exists.
    for (auto&& remoteCursor : newCursors) {
        _remotes.push_back(_buildRemote(lk, remoteCursor));
    }
}

void AsyncResultsMerger::closeShardCursors(const stdx::unordered_set<ShardId>& shardIds) {
    // Closing remote cursors is not supported in tailable mode.
    tassert(8456101,
            "closeShardCursors() can only be used for tailableAndAwaitData cursors.",
            _tailableMode != TailableModeEnum::kTailableAndAwaitData);

    stdx::lock_guard<stdx::mutex> lk(_mutex);

    ON_BLOCK_EXIT([this]() {
        // '_gettingRemote' contains an index into the '_remotes' vector. If the '_remotes' vector
        // gets resized because we remove remotes, then '_gettingFromRemote' may contain an index
        // outside the vector. In this case we can simply reset it to 0. This does not really
        // matter here as we can return results in any order.
        if (_gettingFromRemote >= _remotes.size()) {
            _gettingFromRemote = 0;
        }
    });

    // Erase-remove all remotes from the '_remotes' vector for any of the passed shardIds.
    // We cannot use 'std::erase_if(container, predicate)' here because it is not defined for the
    // 'absl::InlinedVector' type.
    _remotes.erase(std::remove_if(_remotes.begin(),
                                  _remotes.end(),
                                  [&](const RemoteCursorPtr& remote) {
                                      if (!shardIds.contains(remote->shardId)) {
                                          // This is a remote for a different shardId than what we
                                          // are looking for.
                                          return false;
                                      }

                                      LOGV2_DEBUG(8456103,
                                                  2,
                                                  "closing cursor for shard",
                                                  "remoteHost"_attr = remote->getTargetHost(),
                                                  "shardId"_attr = remote->shardId.toString());

                                      // Found a remote with one of the shardIds that we are looking
                                      // for. Now perform the following cleanup steps:
                                      // - Cancel any potential in-flight callbacks for the remote.
                                      // - Close the remote cursor if available.
                                      // - Remove the remote from the vector of promised minimum
                                      // sort keys.
                                      // - Set the remote's 'closed' flag to true. This prevents any
                                      // potential in-flight responses to be added to the merge
                                      // queue when they arrive later.
                                      // - Remove the remote from the '_remotes' vector.
                                      // - Reposition our loop iterator.
                                      _cancelCallbackForRemote(lk, remote);
                                      _scheduleKillCursorForRemote(lk, _opCtx, remote);
                                      _removeRemoteFromPromisedMinSortKeys(lk, remote);
                                      remote->closed = true;

                                      return true;
                                  }),
                   _remotes.end());
}

bool AsyncResultsMerger::partialResultsReturned() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return std::any_of(_remotes.begin(), _remotes.end(), [](const auto& remote) {
        return remote->partialResultsReturned;
    });
}

std::size_t AsyncResultsMerger::getNumRemotes() const {
    // Take the lock to guard against shard additions or disconnections.
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    // If 'allowPartialResults' is false, the number of participating remotes is constant.
    if (!_params.getAllowPartialResults()) {
        return _remotes.size();
    }
    // Otherwise, discount remotes which failed to connect or disconnected prematurely.
    return std::count_if(_remotes.begin(), _remotes.end(), [](const auto& remote) {
        return !remote->partialResultsReturned;
    });
}

bool AsyncResultsMerger::hasCursorForShard_forTest(const ShardId& shardId) const {
    // Take the lock to guard against shard additions or disconnections.
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return std::any_of(_remotes.begin(), _remotes.end(), [&shardId](const auto& remote) {
        return shardId == remote->shardId;
    });
}

BSONObj AsyncResultsMerger::getHighWaterMark() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    // At this point, the high water mark may be the resume token of the last document we returned.
    // If no further results are eligible for return, we advance to the minimum promised sort key.
    // If the remote associated with the minimum promised sort key is not currently eligible to
    // provide a high water mark, then we do not advance even if no further results are ready.
    if (auto minPromisedSortKey = _getMinPromisedSortKey(lk); minPromisedSortKey && !_ready(lk)) {
        const auto& minRemote = minPromisedSortKey->second;
        if (minRemote->eligibleForHighWaterMark) {
            _highWaterMark = minPromisedSortKey->first;
        }
    }
    // The high water mark is stored in sort-key format: {"": <high watermark>}. We only return
    // the <high watermark> part of of the sort key, which looks like {_data: ..., _typeBits: ...}.
    invariant(_highWaterMark.isEmpty() || _highWaterMark.firstElement().type() == BSONType::Object);
    return _highWaterMark.isEmpty() ? BSONObj() : _highWaterMark.firstElement().Obj().getOwned();
}

boost::optional<AsyncResultsMerger::MinSortKeyRemotePair>
AsyncResultsMerger::_getMinPromisedSortKey(WithLock) const {
    // We cannot return the minimum promised sort key unless all shards have reported one.
    return _promisedMinSortKeys.size() < _remotes.size() ? boost::optional<MinSortKeyRemotePair>{}
                                                         : *_promisedMinSortKeys.begin();
}

AsyncResultsMerger::RemoteCursorPtr AsyncResultsMerger::_buildRemote(WithLock lk,
                                                                     const RemoteCursor& rc) {
    RemoteCursorData::IdType id = ++_nextId;

    auto remote =
        make_intrusive<RemoteCursorData>(id,
                                         rc.getHostAndPort(),
                                         rc.getCursorResponse().getNSS(),
                                         rc.getCursorResponse().getCursorId(),
                                         rc.getShardId().toString(),
                                         rc.getCursorResponse().getPartialResultsReturned());

    // We don't check the return value of _addBatchToBuffer here; if there was an error, it will be
    // stored in the remote and the first call to ready() will return true.
    _addBatchToBuffer(lk, remote, rc.getCursorResponse());
    return remote;
}

void AsyncResultsMerger::_cancelCallbackForRemote(WithLock lk, const RemoteCursorPtr& remote) {
    if (!remote->cbHandle.isValid()) {
        return;
    }
    _executor->cancel(remote->cbHandle);
}

void AsyncResultsMerger::_removeRemoteFromPromisedMinSortKeys(WithLock lk,
                                                              const RemoteCursorPtr& remote) {
    if (remote->promisedMinSortKey) {
        std::size_t erased = _promisedMinSortKeys.erase({*remote->promisedMinSortKey, remote});
        tassert(8456114, "Expected to find the promised min sort key in the set.", erased == 1);
    }
}

void AsyncResultsMerger::_setInitialHighWaterMark() {
    // If we do not have any minimum promised sort keys, this is not a change stream. Return early.
    if (_promisedMinSortKeys.empty()) {
        return;
    }
    // Find the minimum promised sort key whose remote is eligible to contribute a high water mark.
    for (auto&& [minSortKey, remote] : _promisedMinSortKeys) {
        if (remote->eligibleForHighWaterMark) {
            _highWaterMark = minSortKey;
            break;
        }
    }
    // We should always be guaranteed to find an eligible remote, if this is a change stream.
    tassert(8456104,
            "There should always be an eligible remote with a valid high watermark.",
            !_highWaterMark.isEmpty());
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
        if (!remote->status.isOK()) {
            _status = remote->status;
            return true;
        }
    }

    return _params.getSort() ? _readySorted(lk) : _readyUnsorted(lk);
}

bool AsyncResultsMerger::_readySorted(WithLock lk) const {
    if (_tailableMode == TailableModeEnum::kTailableAndAwaitData) {
        return _readySortedTailable(lk);
    }
    // Tailable non-awaitData cursors cannot have a sort.
    tassert(8456108,
            "_readySorted() should only be called for tailable cursors.",
            _tailableMode == TailableModeEnum::kNormal);

    return std::all_of(_remotes.begin(), _remotes.end(), [](const auto& remote) {
        return remote->hasNext() || remote->exhausted();
    });
}

bool AsyncResultsMerger::_readySortedTailable(WithLock lk) const {
    if (_mergeQueue.empty()) {
        return false;
    }

    // Read the top element from the merge queue.
    const auto& smallestRemote = _mergeQueue.top();
    const BSONObj& smallestResult = smallestRemote->docBuffer.front();
    auto keyWeWantToReturn = extractSortKey(smallestResult, _params.getCompareWholeSortKey());
    // We should always have a minPromisedSortKey from every shard in the sorted tailable case.
    auto minPromisedSortKey = _getMinPromisedSortKey(lk);
    invariant(minPromisedSortKey);
    return compareSortKeys(keyWeWantToReturn, minPromisedSortKey->first, *_params.getSort()) <= 0;
}

bool AsyncResultsMerger::_readyUnsorted(WithLock) const {
    bool allExhausted = true;
    for (const auto& remote : _remotes) {
        if (!remote->exhausted()) {
            allExhausted = false;
        }

        if (remote->hasNext()) {
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

    // We process additional transaction participants that have been put in the queue for
    // processing. This can happen at any time since receiving a response is asynchronous in nature.
    _processAdditionalTransactionParticipants(_opCtx);

    if (!_status.isOK()) {
        return _status;
    }

    if (_eofNext) {
        _eofNext = false;
        return {ClusterQueryResult()};
    }

    return _params.getSort() ? _nextReadySorted(lk) : _nextReadyUnsorted(lk);
}

void AsyncResultsMerger::_processAdditionalTransactionParticipants(OperationContext* opCtx) {
    const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    while (!_remoteResponses.empty()) {
        const auto& response = _remoteResponses.front();
        processAdditionalTransactionParticipantFromResponse(
            opCtx, response.shardId, response.originalResponse, fcvSnapshot);
        _remoteResponses.pop();
    }
}

ClusterQueryResult AsyncResultsMerger::_nextReadySorted(WithLock) {
    // Tailable non-awaitData cursors cannot have a sort.
    invariant(_tailableMode != TailableModeEnum::kTailable);

    if (_mergeQueue.empty()) {
        return {};
    }

    // Take the remote with the smallest result from the merge queue.
    RemoteCursorPtr smallestRemote = std::move(_mergeQueue.top());
    _mergeQueue.pop();

    tassert(8456109,
            "Expected to see a valid pointer for the remote with the smallest merge key.",
            smallestRemote);
    tassert(8456110,
            "Expected to still have buffered documents for the remote with the smallest merge key.",
            !smallestRemote->docBuffer.empty());
    tassert(8456111,
            "Expected to have an OK status for the remote with the smallest merge key.",
            smallestRemote->status.isOK());

    // Take the next document returned by the remote from the queue.
    BSONObj front = std::move(smallestRemote->docBuffer.front());
    smallestRemote->docBuffer.pop();

    // Re-populate the merging queue with the next result from 'smallestRemote', if it has a next
    // result.
    if (!smallestRemote->docBuffer.empty()) {
        _mergeQueue.push(smallestRemote);
    }

    // For sorted tailable awaitData cursors, update the high water mark to the document's sort key.
    if (_tailableMode == TailableModeEnum::kTailableAndAwaitData &&
        smallestRemote->eligibleForHighWaterMark) {
        _highWaterMark = extractSortKey(front, _params.getCompareWholeSortKey()).getOwned();
    }

    return {std::move(front), smallestRemote->shardId};
}

ClusterQueryResult AsyncResultsMerger::_nextReadyUnsorted(WithLock) {
    size_t remotesAttempted = 0;
    while (remotesAttempted < _remotes.size()) {
        auto& remote = _remotes[_gettingFromRemote];
        // It is illegal to call this method if there is an error received from any shard.
        tassert(8456112, "Expected to have an OK status for every remote.", remote->status.isOK());

        if (remote->hasNext()) {
            BSONObj front = std::move(remote->docBuffer.front());
            remote->docBuffer.pop();

            if (_tailableMode == TailableModeEnum::kTailable && !remote->hasNext()) {
                // The cursor is tailable and we're about to return the last buffered result.
                // This means that the next value returned should be boost::none to indicate the
                // end of the batch.
                _eofNext = true;
            }

            return {std::move(front), remote->shardId};
        }

        // Nothing from the current remote so move on to the next one.
        ++remotesAttempted;
        if (++_gettingFromRemote == _remotes.size()) {
            _gettingFromRemote = 0;
        }
    }

    return {};
}

BSONObj AsyncResultsMerger::_makeRequest(WithLock,
                                         const RemoteCursorData& remote,
                                         const ServerGlobalParams::FCVSnapshot& fcvSnapshot) const {
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
    BSONObjBuilder bob;
    getMoreRequest.serialize(&bob);
    if (_params.getSessionId()) {
        BSONObjBuilder lsidBob{
            bob.subobjStart(OperationSessionInfoFromClient::kSessionIdFieldName)};
        _params.getSessionId()->serialize(&lsidBob);
        lsidBob.doneFast();

        // When the feature flag is enabled the command gets processed with
        // transaction_request_sender_details::attachTxnDetails. Using that method would add
        // startOrContinue which is unrecognized by versions earlier than 8.0 as it is part of the
        // feature flag.
        if (!gFeatureFlagAllowAdditionalParticipants.isEnabled(fcvSnapshot)) {
            if (_params.getTxnNumber()) {
                bob.append(OperationSessionInfoFromClient::kTxnNumberFieldName,
                           *_params.getTxnNumber());
            }

            if (_params.getAutocommit()) {
                bob.append(OperationSessionInfoFromClient::kAutocommitFieldName,
                           *_params.getAutocommit());
            }
        }
    }
    return bob.obj();
}

Status AsyncResultsMerger::scheduleGetMores() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _scheduleGetMores(lk);
}

Status AsyncResultsMerger::_scheduleGetMores(WithLock lk) {
    // Before scheduling more work, check whether the cursor has been invalidated.
    _assertNotInvalidated(lk);

    // Reveal opCtx errors (such as MaxTimeMSExpired) and reflect them in the remote status.
    invariant(_opCtx, "Cannot schedule a getMore without an OperationContext");
    if (auto interruptStatus = _opCtx->checkForInterruptNoAssert(); !interruptStatus.isOK()) {
        for (auto&& remote : _remotes) {
            if (!remote->exhausted()) {
                _cleanUpFailedBatch(lk, interruptStatus, *remote);
            }
        }
        return interruptStatus;
    }

    // Schedule remote work on hosts for which we need more results.
    std::vector<RemoteCursorPtr> remotes;
    for (auto& remote : _remotes) {
        if (!remote->status.isOK()) {
            _cleanUpFailedBatch(lk, remote->status, *remote);
            return remote->status;
        }

        if (!remote->hasNext() && !remote->exhausted() && !remote->cbHandle.isValid()) {
            // If this remote is not exhausted and there is no outstanding request for it, schedule
            // work to retrieve the next batch.
            remotes.emplace_back(remote);
        }
    }

    return _scheduleGetMoresForRemotes(lk, _opCtx, remotes);
}

Status AsyncResultsMerger::_scheduleGetMoresForRemotes(
    WithLock lk, OperationContext* opCtx, const std::vector<RemoteCursorPtr>& remotes) {
    tassert(8456105, "An OperationContext is required for scheduling getMores.", opCtx);

    if (remotes.empty()) {
        return Status::OK();
    }

    const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();

    std::vector<AsyncRequestsSender::Request> asyncRequests;
    asyncRequests.reserve(remotes.size());
    for (const auto& remote : remotes) {
        asyncRequests.emplace_back(remote->shardId, _makeRequest(lk, *remote, fcvSnapshot));
    }

    // Build the batch of requests to send if inside a transaction.
    auto txnRequests = [&] {
        if (gFeatureFlagAllowAdditionalParticipants.isEnabled(fcvSnapshot)) {
            return transaction_request_sender_details::attachTxnDetails(opCtx,
                                                                        std::move(asyncRequests));
        }
        return std::move(asyncRequests);
    }();
    std::vector<executor::RemoteCommandRequest> executorRequests;
    executorRequests.reserve(txnRequests.size());
    for (size_t i = 0; i < txnRequests.size(); i++) {
        const auto& remote = remotes[i];
        executorRequests.emplace_back(remote->getTargetHost(),
                                      remote->cursorNss.dbName(),
                                      std::move(txnRequests[i].cmdObj),
                                      opCtx);
    }

    for (size_t i = 0; i < executorRequests.size(); i++) {
        auto& remote = remotes[i];
        auto callbackStatus = _executor->scheduleRemoteCommand(
            executorRequests[i], [this, remote /* intrusive_ptr copy! */](auto const& cbData) {
                stdx::lock_guard<stdx::mutex> lk(this->_mutex);
                this->_handleBatchResponse(lk, cbData, remote);
            });

        if (!callbackStatus.isOK()) {
            return callbackStatus.getStatus();
        }

        remote->cbHandle = callbackStatus.getValue();
    }

    return Status::OK();
}

/*
 * Note: When nextEvent() is called to do retries, only the remotes with retriable errors will be
 * rescheduled because:
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

    if (auto getMoresStatus = _scheduleGetMores(lk); !getMoresStatus.isOK()) {
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
        const auto& minRemote = minPromisedSortKey->second;
        uassert(ChangeStreamInvalidationInfo{minPromisedSortKey->first.firstElement().Obj()},
                "Change stream invalidated",
                !(minRemote->invalidated && !_ready(lk)));
    }
}

StatusWith<CursorResponse> AsyncResultsMerger::_parseCursorResponse(const BSONObj& responseObj,
                                                                    CursorId expectedCursorId) {
    auto getMoreParseStatus = CursorResponse::parseFromBSON(responseObj);
    if (!getMoreParseStatus.isOK()) {
        return getMoreParseStatus.getStatus();
    }

    auto cursorResponse = std::move(getMoreParseStatus.getValue());

    // If we get a non-zero cursor id that is not equal to the established cursor id, we will fail
    // the operation.
    if (cursorResponse.getCursorId() != 0 && expectedCursorId != cursorResponse.getCursorId()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Expected cursorid " << expectedCursorId << " but received "
                                    << cursorResponse.getCursorId());
    }

    return std::move(cursorResponse);
}

void AsyncResultsMerger::_updateRemoteMetadata(WithLock lk,
                                               const RemoteCursorPtr& remote,
                                               const CursorResponse& response) {
    // Update the cursorId; it is sent as '0' when the cursor has been exhausted on the shard.
    remote->cursorId = response.getCursorId();

    // If the response indicates that the cursor has been invalidated, mark the corresponding remote
    // as invalidated. This also signifies that the shard cursor has been closed.
    remote->invalidated = response.getInvalidated();
    tassert(5493705,
            "Unexpectedly encountered invalidated cursor with non-zero ID",
            !(remote->invalidated && remote->cursorId > 0));

    if (auto postBatchResumeToken = response.getPostBatchResumeToken()) {
        // We only expect to see this for change streams.
        invariant(_params.getSort());
        invariant(SimpleBSONObjComparator::kInstance.evaluate(*_params.getSort() ==
                                                              change_stream_constants::kSortSpec));

        // The postBatchResumeToken should never be empty.
        invariant(!postBatchResumeToken->isEmpty());

        // Note that the PBRT is an object of format {_data: ..., _typeBits: ...} that we must wrap
        // in a sort key so that it can compare correctly with sort keys from other streams.
        auto newMinSortKey = BSON("" << *postBatchResumeToken);

        // Determine whether the new batch is eligible to provide a high water mark resume
        // token.
        remote->eligibleForHighWaterMark =
            _checkHighWaterMarkEligibility(lk, newMinSortKey, *remote, response);

        // The most recent minimum sort key should never be smaller than the previous promised
        // minimum sort key for this remote, if a previous promised minimum sort key exists.
        if (auto& oldMinSortKey = remote->promisedMinSortKey) {
            invariant(compareSortKeys(newMinSortKey, *oldMinSortKey, *_params.getSort()) >= 0);
            invariant(_promisedMinSortKeys.size() <= _remotes.size());
            std::size_t erased = _promisedMinSortKeys.erase({*oldMinSortKey, remote});
            tassert(8456106, "Expected to find the promised min sort key in the set.", erased == 1);
        }
        _promisedMinSortKeys.insert({newMinSortKey, remote});
        remote->promisedMinSortKey = std::move(newMinSortKey);
    }
}

bool AsyncResultsMerger::_checkHighWaterMarkEligibility(WithLock,
                                                        BSONObj newMinSortKey,
                                                        const RemoteCursorData& remote,
                                                        const CursorResponse& response) const {
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
    //     config cursor must always be opened at the current clusterTime, to ensure that it
    //     detects all shards that are added after the change stream is dispatched. We must make
    //     sure that the high water mark ignores the config cursor's minimum promised sort keys,
    //     otherwise we will end up returning a token that is earlier than the start time
    //     requested by the user.
    //
    //   - The cursor returns a "shard added" event. All events produced by the config cursor are
    //     handled and swallowed internally by the stream. We therefore do not want to allow
    //     their resume tokens to be exposed to the user via the postBatchResumeToken mechanism,
    //     since these token are not actually resumable. See SERVER-47810 for further details.

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
                                              const RemoteCursorPtr& remote) {
    // Got a response from remote, so indicate we are no longer waiting for one.
    remote->cbHandle = executor::TaskExecutor::CallbackHandle();

    if (cbData.response.isOK()) {
        // We store the original unprocessed response in order to process additional transaction
        // participants when reading it. Additional transaction participants processing cannot occur
        // here since access to the underlying transaction router is not thread-safe.
        //
        // To avoid memory issues we delay processing until the actual owner thread of the
        // TransactionRouter reads the responses. As this operation can occur after a while the BSON
        // must be owned since otherwise we would be pointing to invalid memory.
        invariant(cbData.response.data.isOwned());
        _remoteResponses.emplace(remote->shardId, cbData.response.data);
    }

    //  On shutdown, there is no need to process the response.
    if (_lifecycleState != kAlive) {
        _signalCurrentEventIfReady(lk);  // First, wake up anyone waiting on '_currentEvent'.
        _cleanUpKilledBatch(lk);
        return;
    }
    try {
        // If the remote for which we received a result has been closed via 'closeShardCursors()'
        // before, we do not add the batch results.
        if (!remote->closed) {
            _processBatchResults(lk, cbData.response, remote);
        }
    } catch (DBException const& e) {
        remote->status = e.toStatus();
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

void AsyncResultsMerger::_cleanUpFailedBatch(WithLock, Status status, RemoteCursorData& remote) {
    remote.cleanUpFailedBatch(status, _params.getAllowPartialResults());
}

void AsyncResultsMerger::_processBatchResults(WithLock lk,
                                              CbResponse const& response,
                                              const RemoteCursorPtr& remote) {
    if (!response.isOK()) {
        _cleanUpFailedBatch(lk, response.status, *remote);
        return;
    }

    auto cursorResponseStatus = _parseCursorResponse(response.data, remote->cursorId);
    if (!cursorResponseStatus.isOK()) {
        _cleanUpFailedBatch(lk,
                            cursorResponseStatus.getStatus().withContext(
                                "Error on remote shard " + remote->shardHostAndPort.toString()),
                            *remote);
        return;
    }

    CursorResponse cursorResponse = std::move(cursorResponseStatus.getValue());
    if (const auto& remoteMetrics = cursorResponse.getCursorMetrics()) {
        _metrics.aggregateCursorMetrics(*remoteMetrics);
    }

    // Save the batch in the remote's buffer.
    if (!_addBatchToBuffer(lk, remote, cursorResponse)) {
        return;
    }

    // If the cursor is tailable and we just received an empty batch, the next return value should
    // be boost::none in order to indicate the end of the batch. We do not ask for the next batch if
    // the cursor is tailable, as batches received from remote tailable cursors should be passed
    // through to the client as-is. (Note: tailable cursors are only valid on unsharded collections,
    // so the end of the batch from one shard means the end of the overall batch).
    if (_tailableMode == TailableModeEnum::kTailable && !remote->hasNext()) {
        invariant(_remotes.size() == 1);
        _eofNext = true;
    }
}

bool AsyncResultsMerger::_addBatchToBuffer(WithLock lk,
                                           const RemoteCursorPtr& remote,
                                           const CursorResponse& response) {
    tassert(8456107,
            "Expected _addBatchToBuffer() to be called for not-yet closed remote.",
            !remote->closed);

    _updateRemoteMetadata(lk, remote, response);
    for (const auto& obj : response.getBatch()) {
        // If there's a sort, we're expecting the remote node to have given us back a sort key.
        if (_params.getSort()) {
            auto key = obj[AsyncResultsMerger::kSortKeyField];
            if (!key) {
                remote->status =
                    Status(ErrorCodes::InternalError,
                           str::stream() << "Missing field '" << AsyncResultsMerger::kSortKeyField
                                         << "' in document: " << obj);
                return false;
            } else if (!_params.getCompareWholeSortKey() && !key.isABSONObj()) {
                remote->status =
                    Status(ErrorCodes::InternalError,
                           str::stream() << "Field '" << AsyncResultsMerger::kSortKeyField
                                         << "' was not of type Object in document: " << obj);
                return false;
            }
        }

        remote->docBuffer.push(obj);
        ++remote->fetchedCount;
    }

    // If we're doing a sorted merge, then we have to make sure to put this remote onto the merge
    // queue.
    if (_params.getSort() && !response.getBatch().empty()) {
        _mergeQueue.push(remote);
    }
    return true;
}

void AsyncResultsMerger::_signalCurrentEventIfReady(WithLock lk) {
    // We signal if we're ready to respond or if there are no more pending requests to be received.
    // In the latter case we expect the caller to schedule more getMore requests.
    if (_currentEvent.isValid() && (_ready(lk) || !_haveOutstandingBatchRequests(lk))) {
        // To prevent ourselves from signalling the event twice, we set '_currentEvent' as invalid
        // after signalling it.
        _executor->signalEvent(_currentEvent);
        _currentEvent = executor::TaskExecutor::EventHandle();
    }
}

bool AsyncResultsMerger::_haveOutstandingBatchRequests(WithLock) {
    return std::any_of(_remotes.begin(), _remotes.end(), [](const auto& remote) {
        return remote->cbHandle.isValid();
    });
}

void AsyncResultsMerger::_scheduleKillCursors(WithLock lk, OperationContext* opCtx) {
    invariant(_killCompleteInfo);

    for (const auto& remote : _remotes) {
        _scheduleKillCursorForRemote(lk, opCtx, remote);
    }
}

void AsyncResultsMerger::_scheduleKillCursorForRemote(WithLock lk,
                                                      OperationContext* opCtx,
                                                      const RemoteCursorPtr& remote) {
    if (!_shouldKillRemote(lk, *remote)) {
        return;
    }

    BSONObj cmdObj = KillCursorsCommandRequest(_params.getNss(), {remote->cursorId}).toBSON();
    executor::RemoteCommandRequest request(remote->getTargetHost(),
                                           _params.getNss().dbName(),
                                           cmdObj,
                                           rpc::makeEmptyMetadata(),
                                           opCtx,
                                           true /* fireAndForget */);
    // The 'RemoteCommandRequest' takes the remaining time from the 'opCtx' parameter. If the cursor
    // was killed due to a maxTimeMs timeout, the remaining time will be 0, and the remote request
    // will not be sent. To avoid this, we remove the timeout for the remote 'killCursor' command.
    request.timeout = executor::RemoteCommandRequest::kNoTimeout;

    LOGV2_DEBUG(8456102,
                2,
                "scheduling killCursors command",
                "remoteHost"_attr = remote->getTargetHost(),
                "shardId"_attr = remote->shardId.toString());

    // Send kill request; discard callback handle, if any, or failure report, if not.
    _executor
        ->scheduleRemoteCommand(request,
                                [host = remote->getTargetHost()](auto const& args) {
                                    if (args.response.isOK()) {
                                        LOGV2_DEBUG(8928416,
                                                    2,
                                                    "killCursors succeeded",
                                                    "remoteHost"_attr = host.toString());
                                    } else {
                                        LOGV2_DEBUG(8928417,
                                                    2,
                                                    "killCursors failed",
                                                    "remoteHost"_attr = host.toString(),
                                                    "error"_attr = args.response);
                                    }
                                })
        .getStatus()
        .ignore();
}

bool AsyncResultsMerger::_shouldKillRemote(WithLock, const RemoteCursorData& remote) {
    static const std::set<ErrorCodes::Error> kCursorAlreadyDeadCodes = {
        ErrorCodes::QueryPlanKilled, ErrorCodes::CursorKilled, ErrorCodes::CursorNotFound};
    return remote.cursorId && !remote.exhausted() &&
        !kCursorAlreadyDeadCodes.count(remote.status.code());
}

stdx::shared_future<void> AsyncResultsMerger::kill(OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

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

    // We do a last attempt to process the pending additional transaction participants if executing
    // under the same transaction. Processing additional participants on a different transaction
    // would result in modifying the list of participants of a transaction that has nothing to do
    // with the original one.
    if (opCtx->inMultiDocumentTransaction() &&
        opCtx->getLogicalSessionId() == _params.getSessionId().map([&](const auto& lsid) {
            return makeLogicalSessionId(lsid, opCtx);
        }) &&
        opCtx->getTxnNumber() == _params.getTxnNumber()) {
        _processAdditionalTransactionParticipants(opCtx);
    }

    if (!_haveOutstandingBatchRequests(lk)) {
        _lifecycleState = kKillComplete;
        // Signal the future right now, as there's nothing to wait for.
        _killCompleteInfo->signalFutures();
        return _killCompleteInfo->getFuture();
    }

    // Cancel all of our callbacks. Once they all complete, the event will be signaled.
    for (const auto& remote : _remotes) {
        _cancelCallbackForRemote(lk, remote);
    }
    return _killCompleteInfo->getFuture();
}

query_stats::DataBearingNodeMetrics AsyncResultsMerger::takeMetrics() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    auto metrics = _metrics;
    _metrics = {};
    return metrics;
}

//
// AsyncResultsMerger::RemoteCursorData
//

AsyncResultsMerger::RemoteCursorData::RemoteCursorData(
    AsyncResultsMerger::RemoteCursorData::IdType id,
    HostAndPort hostAndPort,
    NamespaceString cursorNss,
    CursorId establishedCursorId,
    std::string shardId,
    bool partialResultsReturned)
    : id(id),
      cursorId(establishedCursorId),
      cursorNss(std::move(cursorNss)),
      shardHostAndPort(std::move(hostAndPort)),
      shardId(std::move(shardId)),
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

void AsyncResultsMerger::RemoteCursorData::cleanUpFailedBatch(Status status,
                                                              bool allowPartialResults) {
    this->status = std::move(status);
    // Unreachable host errors are swallowed if the 'allowPartialResults' option is set. We remove
    // the unreachable host entirely from consideration by marking it as exhausted.
    //
    // The ExchangePassthrough error code is an internal-only error code used specifically to
    // communicate that an error has occurred, but some other thread is responsible for returning
    // the error to the user. In order to avoid polluting the user's error message, we ignore such
    // errors with the expectation that all outstanding cursors will be closed promptly.
    if (allowPartialResults || this->status == ErrorCodes::ExchangePassthrough) {
        // Clear the results buffer and cursor id, and set 'partialResultsReturned' if appropriate.
        partialResultsReturned = (this->status != ErrorCodes::ExchangePassthrough);
        // std::queue<> does not have a 'clear()' method. Instead, create an empty queue object and
        // swap it in.
        std::queue<BSONObj> emptyBuffer;
        std::swap(docBuffer, emptyBuffer);
        this->status = Status::OK();
        cursorId = 0;
    }
}

//
// AsyncResultsMerger::MergingComparator
//

bool AsyncResultsMerger::MergingComparator::operator()(const RemoteCursorPtr& lhs,
                                                       const RemoteCursorPtr& rhs) const {
    const BSONObj& leftDoc = lhs->docBuffer.front();
    const BSONObj& rightDoc = rhs->docBuffer.front();

    return compareSortKeys(extractSortKey(leftDoc, _compareWholeSortKey),
                           extractSortKey(rightDoc, _compareWholeSortKey),
                           _sort) > 0;
}

bool AsyncResultsMerger::PromisedMinSortKeyComparator::operator()(
    const MinSortKeyRemotePair& lhs, const MinSortKeyRemotePair& rhs) const {
    auto sortKeyComp = compareSortKeys(lhs.first, rhs.first, _sort);
    // First compare by sort keys, and if sort key values compare the same, compare by id value to
    // get a deterministic tie breaker.
    return sortKeyComp < 0 || (sortKeyComp == 0 && lhs.second->id < rhs.second->id);
}

}  // namespace mongo
