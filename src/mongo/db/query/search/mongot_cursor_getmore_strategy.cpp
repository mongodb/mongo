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
    bool preFetchNextBatch,
    std::function<boost::optional<long long>()> calcDocsNeededFn,
    int64_t startingBatchSize)
    : _preFetchNextBatch(preFetchNextBatch),
      _calcDocsNeededFn(calcDocsNeededFn),
      _useBatchSizeTuning(feature_flags::gFeatureFlagSearchBatchSizeTuning.isEnabled(
          serverGlobalParams.featureCompatibility.acquireFCVSnapshot())),
      _currentBatchSize(startingBatchSize),
      _batchSizeHistory({_currentBatchSize}) {}

BSONObj MongotTaskExecutorCursorGetMoreStrategy::createGetMoreRequest(const CursorId& cursorId,
                                                                      const NamespaceString& nss) {
    GetMoreCommandRequest getMoreRequest(cursorId, nss.coll().toString());

    boost::optional<long long> docsNeeded = _calcDocsNeededFn ? _calcDocsNeededFn() : boost::none;
    // (Ignore FCV check): This feature is enabled on an earlier FCV.
    // TODO SERVER-74941 Remove featureFlagSearchBatchSizeLimit.
    // TODO SERVER-89530 Follow the same pattern as the featureFlagSearchBatchSizeTuning.
    bool needsSetDocsRequested = !_useBatchSizeTuning &&
        feature_flags::gFeatureFlagSearchBatchSizeLimit.isEnabledAndIgnoreFCVUnsafe() &&
        docsNeeded.has_value();

    // If the cursor is tuning batchSizes, set the batchSize field. Otherwise, set the docsRequested
    // field if we are performing limit optimization for this request.
    // TODO SERVER-74941 Remove docsRequested alongside featureFlagSearchBatchSizeLimit.
    if (_useBatchSizeTuning || needsSetDocsRequested) {
        BSONObjBuilder getMoreBob;
        getMoreRequest.serialize({}, &getMoreBob);
        BSONObjBuilder cursorOptionsBob(getMoreBob.subobjStart(mongot_cursor::kCursorOptionsField));
        if (_useBatchSizeTuning) {
            cursorOptionsBob.append(mongot_cursor::kBatchSizeField, _getNextBatchSize());
        } else {
            cursorOptionsBob.append(mongot_cursor::kDocsRequestedField, docsNeeded.get());
        }
        cursorOptionsBob.doneFast();
        return getMoreBob.obj();
    }
    return getMoreRequest.toBSON({});
}

int64_t MongotTaskExecutorCursorGetMoreStrategy::_getNextBatchSize() {
    // In case the growth factor is small enough that the next batchSize rounds back to the
    // previous batchSize, we use std::ceil to make sure it always grows by at least 1,
    // unless the growth factor is exactly 1.
    _currentBatchSize = std::ceil(_currentBatchSize * kInternalSearchBatchSizeGrowthFactor);
    _batchSizeHistory.emplace_back(_currentBatchSize);
    return _currentBatchSize;
}

}  // namespace executor
}  // namespace mongo
