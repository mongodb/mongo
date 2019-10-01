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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/fail_point_service.h"

namespace mongo {

/**
 * The state associated with tailable cursors.
 */
struct AwaitDataState {
    /**
     * The deadline for how long we wait on the tail of capped collection before returning IS_EOF.
     */
    Date_t waitForInsertsDeadline;

    /**
     * If true, when no results are available from a plan, then instead of returning immediately,
     * the system should wait up to the length of the operation deadline for data to be inserted
     * which causes results to become available.
     */
    bool shouldWaitForInserts;
};

extern const OperationContext::Decoration<AwaitDataState> awaitDataState;

class BSONObj;
class CanonicalQuery;
class QueryRequest;

// Failpoint for making find hang.
extern FailPoint waitInFindBeforeMakingBatch;

// Failpoint for making getMore not wait for an awaitdata cursor. Allows us to avoid waiting during
// tests.
extern FailPoint disableAwaitDataForGetMoreCmd;

// Enabling this fail point will cause getMores to busy wait after pinning the cursor
// but before we have started building the batch, until the fail point is disabled.
extern FailPoint waitAfterPinningCursorBeforeGetMoreBatch;

// Enabling this fail point will cause getMores to busy wait after setting up the plan executor and
// before beginning the batch.
extern FailPoint waitWithPinnedCursorDuringGetMoreBatch;

// Enabling this failpoint will cause getMores to wait just before it unpins its cursor after it
// has completed building the current batch.
extern FailPoint waitBeforeUnpinningOrDeletingCursorAfterGetMoreBatch;

/**
 * Suite of find/getMore related functions used in both the mongod and mongos query paths.
 */
class FindCommon {
public:
    // The maximum amount of user data to return to a client in a single batch.
    //
    // This max may be exceeded by epsilon for output documents that approach the maximum user
    // document size. That is, if we must return a BSONObjMaxUserSize document, then the total
    // response size will be BSONObjMaxUserSize plus the amount of size required for the message
    // header and the cursor response "envelope". (The envolope contains namespace and cursor id
    // info.)
    static const int kMaxBytesToReturnToClientAtOnce = BSONObjMaxUserSize;

    // The initial size of the query response buffer.
    static const int kInitReplyBufferSize = 32768;

    /**
     * Returns true if the batchSize for the initial find has been satisfied.
     *
     * If 'qr' does not have a batchSize, the default batchSize is respected.
     */
    static bool enoughForFirstBatch(const QueryRequest& qr, long long numDocs);

    /**
     * Returns true if the batchSize for the getMore has been satisfied.
     *
     * An 'effectiveBatchSize' value of zero is interpreted as the absence of a batchSize, in which
     * case this method returns false.
     */
    static bool enoughForGetMore(long long effectiveBatchSize, long long numDocs) {
        return effectiveBatchSize && numDocs >= effectiveBatchSize;
    }

    /**
     * Given the number of docs ('numDocs') and bytes ('bytesBuffered') currently buffered as a
     * response to a cursor-generating command, returns true if there are enough remaining bytes in
     * our budget to fit 'nextDoc'.
     */
    static bool haveSpaceForNext(const BSONObj& nextDoc, long long numDocs, int bytesBuffered);

    /**
     * This function wraps waitWhileFailPointEnabled() on waitInFindBeforeMakingBatch.
     *
     * Since query processing happens in three different places, this function makes it easier to
     * check the failpoint for a query's namespace and log a helpful diagnostic message when the
     * failpoint is active.
     */
    static void waitInFindBeforeMakingBatch(OperationContext* opCtx, const CanonicalQuery& cq);
};

}  // namespace mongo
