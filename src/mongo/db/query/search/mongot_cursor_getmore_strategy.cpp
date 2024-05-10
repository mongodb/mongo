/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/query/search/mongot_cursor_getmore_strategy.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/getmore_command_gen.h"
#include "mongo/db/query/search/mongot_cursor.h"

namespace mongo {
namespace executor {

MongotTaskExecutorCursorGetMoreStrategy::MongotTaskExecutorCursorGetMoreStrategy(
    std::function<boost::optional<long long>()> calcDocsNeededFn,
    boost::optional<long long> startingBatchSize,
    DocsNeededBounds minDocsNeededBounds,
    DocsNeededBounds maxDocsNeededBounds)
    : _calcDocsNeededFn(calcDocsNeededFn),
      _currentBatchSize(startingBatchSize),
      _minDocsNeededBounds(minDocsNeededBounds),
      _maxDocsNeededBounds(maxDocsNeededBounds),
      _batchSizeHistory({}) {
    if (startingBatchSize.has_value()) {
        _batchSizeHistory.emplace_back(*startingBatchSize);
    }
    tassert(8953000,
            "Only one of docsRequested or batchSize should be enabled for mongot getMore requests.",
            _calcDocsNeededFn == nullptr || !_currentBatchSize.has_value());
}

BSONObj MongotTaskExecutorCursorGetMoreStrategy::createGetMoreRequest(
    const CursorId& cursorId, const NamespaceString& nss, long long prevBatchNumReceived) {
    GetMoreCommandRequest getMoreRequest(cursorId, nss.coll().toString());

    boost::optional<long long> docsNeeded = _calcDocsNeededFn ? _calcDocsNeededFn() : boost::none;

    // TODO SERVER-74941 Remove docsRequested alongside featureFlagSearchBatchSizeLimit.
    if (_currentBatchSize.has_value() || docsNeeded.has_value()) {
        BSONObjBuilder getMoreBob;
        getMoreRequest.serialize(&getMoreBob);
        BSONObjBuilder cursorOptionsBob(getMoreBob.subobjStart(mongot_cursor::kCursorOptionsField));
        if (_currentBatchSize.has_value()) {
            cursorOptionsBob.append(mongot_cursor::kBatchSizeField,
                                    _getNextBatchSize(prevBatchNumReceived));
        } else {
            cursorOptionsBob.append(mongot_cursor::kDocsRequestedField, docsNeeded.get());
        }
        cursorOptionsBob.doneFast();
        return getMoreBob.obj();
    }
    return getMoreRequest.toBSON();
}

long long MongotTaskExecutorCursorGetMoreStrategy::_getNextBatchSize(
    long long prevBatchNumReceived) {
    tassert(8953002,
            "_getNextBatchSize() should only be called when using the batchSize field.",
            _currentBatchSize.has_value());

    // Don't increase batchSize if the previous batch was returned without filling to the requested
    // batchSize. That indicates the BSON limit was reached, which likely could happen again.
    if (prevBatchNumReceived == *_currentBatchSize) {
        // In case the growth factor is small enough that the next batchSize rounds back to the
        // previous batchSize, we use std::ceil to make sure it always grows by at least 1,
        // unless the growth factor is exactly 1.
        _currentBatchSize = std::ceil(*_currentBatchSize * kInternalSearchBatchSizeGrowthFactor);
    }
    _batchSizeHistory.emplace_back(*_currentBatchSize);
    return *_currentBatchSize;
}

bool MongotTaskExecutorCursorGetMoreStrategy::shouldPrefetch() const {
    // If we aren't sending batchSize to mongot, then we prefetch the next batch, unless we have a
    // discrete maximum bounds used to set docsRequested. When docsRequested is set, we
    // optimistically assume that we will only need a single batch and attempt to avoid doing
    // unnecessary work on mongot. If $idLookup filters out enough documents such that we are not
    // able to satisfy the limit, then we will fetch the next batch syncronously on the subsequent
    // 'getNext()' call.
    if (!_currentBatchSize.has_value()) {
        return !std::holds_alternative<long long>(_maxDocsNeededBounds);
    }

    // TODO SERVER-86739 Enable pre-fetching for queries that use batchSize tuning.
    return false;
}

}  // namespace executor
}  // namespace mongo
