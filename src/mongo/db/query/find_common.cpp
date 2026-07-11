// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/query/find_common.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <string>
#include <utility>


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

MONGO_FAIL_POINT_DEFINE(shardWaitInFindBeforeMakingBatch);
MONGO_FAIL_POINT_DEFINE(routerWaitInFindBeforeMakingBatch);

MONGO_FAIL_POINT_DEFINE(disableAwaitDataForGetMoreCmd);

MONGO_FAIL_POINT_DEFINE(waitAfterPinningCursorBeforeGetMoreBatch);

MONGO_FAIL_POINT_DEFINE(waitWithPinnedCursorDuringGetMoreBatch);

MONGO_FAIL_POINT_DEFINE(waitBeforeUnpinningOrDeletingCursorAfterGetMoreBatch);

MONGO_FAIL_POINT_DEFINE(failGetMoreAfterCursorCheckout);

const OperationContext::Decoration<AwaitDataState> awaitDataState =
    OperationContext::declareDecoration<AwaitDataState>();

void setAwaitDataDeadline(OperationContext* opCtx, Date_t deadline) {
    awaitDataState(opCtx).waitForInsertsDeadline = deadline;
}

const size_t FindCommon::kMaxBytesToReturnToClientAtOnce = BSONObjMaxUserSize;
const size_t FindCommon::kTailableGetMoreReplyBufferSize = BSONObjMaxUserSize / 2 - 1024;
const size_t FindCommon::kMinDocSizeForGetMorePreAllocation = 1024;
const size_t FindCommon::kInitReplyBufferSize = 32768;

bool FindCommon::enoughForFirstBatch(const FindCommandRequest& findCommand, long long numDocs) {
    auto batchSize = findCommand.getBatchSize();
    if (!batchSize) {
        // We enforce a default batch size for the initial find if no batch size is specified.
        return numDocs >= query_request_helper::getDefaultBatchSize();
    }

    return numDocs >= batchSize.value();
}

bool FindCommon::haveSpaceForNext(const BSONObj& nextDoc, long long numDocs, size_t bytesBuffered) {
    tassert(
        11320918, fmt::format("'numDocs' cannot be negative, but found {}", numDocs), numDocs >= 0);
    if (!numDocs) {
        // Allow the first output document to exceed the limit to ensure we can always make
        // progress.
        return true;
    }

    return fitsInBatch(bytesBuffered, nextDoc.objsize());
}

void FindCommon::waitInFindBeforeMakingBatch(OperationContext* opCtx,
                                             const CanonicalQuery& cq,
                                             FailPoint* fp) {
    auto whileWaitingFunc = [&, hasLogged = false]() mutable {
        if (!std::exchange(hasLogged, true)) {
            LOGV2(20908,
                  "Waiting in find before making batch for query",
                  "query"_attr = redact(cq.toStringShort()));
        }
    };

    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        fp, opCtx, "waitInFindBeforeMakingBatch", whileWaitingFunc, cq.nss());
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
bool FindCommon::BSONArrayResponseSizeTracker::haveSpaceForNext(const BSONObj& document) {
    return FindCommon::haveSpaceForNext(document, _numberOfDocuments, _bsonArraySizeInBytes);
}
void FindCommon::BSONArrayResponseSizeTracker::add(const BSONObj& document) {
    dassert(haveSpaceForNext(document));
    ++_numberOfDocuments;
    _bsonArraySizeInBytes += (document.objsize() + kPerDocumentOverheadBytesUpperBound);
}

// Upper bound of BSON array element overhead. The overhead is 1 byte/doc for the type + 1 byte/doc
// for the field name's null terminator + 1 byte per digit of the maximum array index value.
const size_t FindCommon::BSONArrayResponseSizeTracker::kPerDocumentOverheadBytesUpperBound{
    2 + std::to_string(BSONObjMaxUserSize / BSONObj::kMinBSONLength).length()};
}  // namespace mongo
