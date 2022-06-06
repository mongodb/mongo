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


#include "mongo/platform/basic.h"

#include "mongo/db/query/find_common.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/curop.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

MONGO_FAIL_POINT_DEFINE(waitInFindBeforeMakingBatch);

MONGO_FAIL_POINT_DEFINE(disableAwaitDataForGetMoreCmd);

MONGO_FAIL_POINT_DEFINE(waitAfterPinningCursorBeforeGetMoreBatch);

MONGO_FAIL_POINT_DEFINE(waitWithPinnedCursorDuringGetMoreBatch);

MONGO_FAIL_POINT_DEFINE(waitBeforeUnpinningOrDeletingCursorAfterGetMoreBatch);

MONGO_FAIL_POINT_DEFINE(failGetMoreAfterCursorCheckout);

const OperationContext::Decoration<AwaitDataState> awaitDataState =
    OperationContext::declareDecoration<AwaitDataState>();

const size_t FindCommon::kMaxBytesToReturnToClientAtOnce = BSONObjMaxUserSize;
const size_t FindCommon::kTailableGetMoreReplyBufferSize = BSONObjMaxUserSize / 2 - 1024;
const size_t FindCommon::kMinDocSizeForGetMorePreAllocation = 1024;
const size_t FindCommon::kInitReplyBufferSize = 32768;

bool FindCommon::enoughForFirstBatch(const FindCommandRequest& findCommand, long long numDocs) {
    auto batchSize = findCommand.getBatchSize();
    if (!batchSize) {
        // We enforce a default batch size for the initial find if no batch size is specified.
        return numDocs >= query_request_helper::kDefaultBatchSize;
    }

    return numDocs >= batchSize.value();
}

bool FindCommon::haveSpaceForNext(const BSONObj& nextDoc, long long numDocs, size_t bytesBuffered) {
    invariant(numDocs >= 0);
    if (!numDocs) {
        // Allow the first output document to exceed the limit to ensure we can always make
        // progress.
        return true;
    }

    return (bytesBuffered + nextDoc.objsize()) <= kMaxBytesToReturnToClientAtOnce;
}

void FindCommon::waitInFindBeforeMakingBatch(OperationContext* opCtx, const CanonicalQuery& cq) {
    auto whileWaitingFunc = [&, hasLogged = false]() mutable {
        if (!std::exchange(hasLogged, true)) {
            LOGV2(20908,
                  "Waiting in find before making batch for query",
                  "query"_attr = redact(cq.toStringShort()));
        }
    };

    CurOpFailpointHelpers::waitWhileFailPointEnabled(&mongo::waitInFindBeforeMakingBatch,
                                                     opCtx,
                                                     "waitInFindBeforeMakingBatch",
                                                     std::move(whileWaitingFunc),
                                                     cq.nss());
}

std::size_t FindCommon::getBytesToReserveForGetMoreReply(bool isTailable,
                                                         size_t firstResultSize,
                                                         size_t batchSize) {
#ifdef _WIN32
    // SERVER-22100: In Windows DEBUG builds, the CRT heap debugging overhead, in
    // conjunction with the additional memory pressure introduced by reply buffer
    // pre-allocation, causes the concurrency suite to run extremely slowly. As a workaround
    // we do not pre-allocate in Windows DEBUG builds.
    if (kDebugBuild)
        return 0;
#endif

    // A tailable cursor may often return 0 or very few results. Allocate a small initial
    // buffer. This buffer should be big-enough to accomodate for at least one document.
    if (isTailable) {
        return std::max(firstResultSize, kTailableGetMoreReplyBufferSize);
    }

    // A getMore with batchSize is likely to return limited results. Allocate a medium
    // initial buffer based on an estimate of document sizes and the given batch size.
    if (batchSize > 0) {
        size_t estmtObjSize = std::max(kMinDocSizeForGetMorePreAllocation, firstResultSize);
        return std::min(estmtObjSize * batchSize, kMaxBytesToReturnToClientAtOnce);
    }

    // Otherwise, reserve enough buffer to fit a full batch of results. The allocator will
    // assign a little more than the requested amount to avoid reallocation when we add
    // command metadata to the reply.
    return kMaxBytesToReturnToClientAtOnce;
}

}  // namespace mongo
