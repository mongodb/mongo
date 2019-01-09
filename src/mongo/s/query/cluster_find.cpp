
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/s/query/cluster_find.h"

#include <set>
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connpool.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/platform/overflow_arithmetic.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/async_results_merger.h"
#include "mongo/s/query/cluster_client_cursor_impl.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/query/cluster_query_knobs.h"
#include "mongo/s/query/establish_cursors.h"
#include "mongo/s/query/store_possible_cursor.h"
#include "mongo/s/stale_exception.h"
#include "mongo/s/transaction_router.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

namespace {

static const BSONObj kSortKeyMetaProjection = BSON("$meta"
                                                   << "sortKey");
static const BSONObj kGeoNearDistanceMetaProjection = BSON("$meta"
                                                           << "geoNearDistance");

// We must allow some amount of overhead per result document, since when we make a cursor response
// the documents are elements of a BSONArray. The overhead is 1 byte/doc for the type + 1 byte/doc
// for the field name's null terminator + 1 byte per digit in the array index. The index can be no
// more than 8 decimal digits since the response is at most 16MB, and 16 * 1024 * 1024 < 1 * 10^8.
static const int kPerDocumentOverheadBytesUpperBound = 10;

const char kFindCmdName[] = "find";

/**
 * Given the QueryRequest 'qr' being executed by mongos, returns a copy of the query which is
 * suitable for forwarding to the targeted hosts.
 */
StatusWith<std::unique_ptr<QueryRequest>> transformQueryForShards(
    const QueryRequest& qr, bool appendGeoNearDistanceProjection) {
    // If there is a limit, we forward the sum of the limit and the skip.
    boost::optional<long long> newLimit;
    if (qr.getLimit()) {
        long long newLimitValue;
        if (mongoSignedAddOverflow64(*qr.getLimit(), qr.getSkip().value_or(0), &newLimitValue)) {
            return Status(
                ErrorCodes::Overflow,
                str::stream()
                    << "sum of limit and skip cannot be represented as a 64-bit integer, limit: "
                    << *qr.getLimit()
                    << ", skip: "
                    << qr.getSkip().value_or(0));
        }
        newLimit = newLimitValue;
    }

    // Similarly, if nToReturn is set, we forward the sum of nToReturn and the skip.
    boost::optional<long long> newNToReturn;
    if (qr.getNToReturn()) {
        // !wantMore and ntoreturn mean the same as !wantMore and limit, so perform the conversion.
        if (!qr.wantMore()) {
            long long newLimitValue;
            if (mongoSignedAddOverflow64(
                    *qr.getNToReturn(), qr.getSkip().value_or(0), &newLimitValue)) {
                return Status(ErrorCodes::Overflow,
                              str::stream()
                                  << "sum of ntoreturn and skip cannot be represented as a 64-bit "
                                     "integer, ntoreturn: "
                                  << *qr.getNToReturn()
                                  << ", skip: "
                                  << qr.getSkip().value_or(0));
            }
            newLimit = newLimitValue;
        } else {
            long long newNToReturnValue;
            if (mongoSignedAddOverflow64(
                    *qr.getNToReturn(), qr.getSkip().value_or(0), &newNToReturnValue)) {
                return Status(ErrorCodes::Overflow,
                              str::stream()
                                  << "sum of ntoreturn and skip cannot be represented as a 64-bit "
                                     "integer, ntoreturn: "
                                  << *qr.getNToReturn()
                                  << ", skip: "
                                  << qr.getSkip().value_or(0));
            }
            newNToReturn = newNToReturnValue;
        }
    }

    // If there is a sort other than $natural, we send a sortKey meta-projection to the remote node.
    BSONObj newProjection = qr.getProj();
    if (!qr.getSort().isEmpty() && !qr.getSort()["$natural"]) {
        BSONObjBuilder projectionBuilder;
        projectionBuilder.appendElements(qr.getProj());
        projectionBuilder.append(AsyncResultsMerger::kSortKeyField, kSortKeyMetaProjection);
        newProjection = projectionBuilder.obj();
    }

    if (appendGeoNearDistanceProjection) {
        invariant(qr.getSort().isEmpty());
        BSONObjBuilder projectionBuilder;
        projectionBuilder.appendElements(qr.getProj());
        projectionBuilder.append(AsyncResultsMerger::kSortKeyField, kGeoNearDistanceMetaProjection);
        newProjection = projectionBuilder.obj();
    }

    auto newQR = stdx::make_unique<QueryRequest>(qr);
    newQR->setProj(newProjection);
    newQR->setSkip(boost::none);
    newQR->setLimit(newLimit);
    newQR->setNToReturn(newNToReturn);

    // Even if the client sends us singleBatch=true (wantMore=false), we may need to retrieve
    // multiple batches from a shard in order to return the single requested batch to the client.
    // Therefore, we must always send singleBatch=false (wantMore=true) to the shards.
    newQR->setWantMore(true);

    invariant(newQR->validate());
    return std::move(newQR);
}

/**
 * Constructs the find commands sent to each targeted shard to establish cursors, attaching the
 * shardVersion and txnNumber, if necessary.
 */
std::vector<std::pair<ShardId, BSONObj>> constructRequestsForShards(
    OperationContext* opCtx,
    const CachedCollectionRoutingInfo& routingInfo,
    const std::set<ShardId>& shardIds,
    const CanonicalQuery& query,
    bool appendGeoNearDistanceProjection) {
    const auto qrToForward = uassertStatusOK(
        transformQueryForShards(query.getQueryRequest(), appendGeoNearDistanceProjection));

    auto shardRegistry = Grid::get(opCtx)->shardRegistry();
    std::vector<std::pair<ShardId, BSONObj>> requests;
    for (const auto& shardId : shardIds) {
        const auto shard = uassertStatusOK(shardRegistry->getShard(opCtx, shardId));
        invariant(!shard->isConfig() || shard->getConnString().type() != ConnectionString::INVALID);

        BSONObjBuilder cmdBuilder;
        qrToForward->asFindCommand(&cmdBuilder);

        if (routingInfo.cm()) {
            routingInfo.cm()->getVersion(shardId).appendToCommand(&cmdBuilder);
        } else if (!query.nss().isOnInternalDb()) {
            ChunkVersion::UNSHARDED().appendToCommand(&cmdBuilder);
        }

        if (opCtx->getTxnNumber()) {
            cmdBuilder.append(OperationSessionInfo::kTxnNumberFieldName, *opCtx->getTxnNumber());
        }

        requests.emplace_back(shardId, cmdBuilder.obj());
    }

    return requests;
}

CursorId runQueryWithoutRetrying(OperationContext* opCtx,
                                 const CanonicalQuery& query,
                                 const ReadPreferenceSetting& readPref,
                                 const CachedCollectionRoutingInfo& routingInfo,
                                 std::vector<BSONObj>* results) {
    // Get the set of shards on which we will run the query.
    auto shardIds = getTargetedShardsForQuery(opCtx,
                                              routingInfo,
                                              query.getQueryRequest().getFilter(),
                                              query.getQueryRequest().getCollation());

    // Construct the query and parameters.

    ClusterClientCursorParams params(query.nss(), readPref);
    params.originatingCommandObj = CurOp::get(opCtx)->opDescription().getOwned();
    params.limit = query.getQueryRequest().getLimit();
    params.batchSize = query.getQueryRequest().getEffectiveBatchSize();
    params.skip = query.getQueryRequest().getSkip();
    params.tailableMode = query.getQueryRequest().getTailableMode();
    params.isAllowPartialResults = query.getQueryRequest().isAllowPartialResults();
    params.lsid = opCtx->getLogicalSessionId();
    params.txnNumber = opCtx->getTxnNumber();

    if (TransactionRouter::get(opCtx)) {
        params.isAutoCommit = false;
    }

    // This is the batchSize passed to each subsequent getMore command issued by the cursor. We
    // usually use the batchSize associated with the initial find, but as it is illegal to send a
    // getMore with a batchSize of 0, we set it to use the default batchSize logic.
    if (params.batchSize && *params.batchSize == 0) {
        params.batchSize = boost::none;
    }

    // $natural sort is actually a hint to use a collection scan, and shouldn't be treated like a
    // sort on mongos. Including a $natural anywhere in the sort spec results in the whole sort
    // being considered a hint to use a collection scan.
    if (!query.getQueryRequest().getSort().hasField("$natural")) {
        params.sort = FindCommon::transformSortSpec(query.getQueryRequest().getSort());
    }

    bool appendGeoNearDistanceProjection = false;
    if (query.getQueryRequest().getSort().isEmpty() &&
        QueryPlannerCommon::hasNode(query.root(), MatchExpression::GEO_NEAR)) {
        // There is no specified sort, and there is a GEO_NEAR node. This means we should merge sort
        // by the geoNearDistance. Request the projection {$sortKey: <geoNearDistance>} from the
        // shards. Indicate to the AsyncResultsMerger that it should extract the sort key
        // {"$sortKey": <geoNearDistance>} and sort by the order {"$sortKey": 1}.
        params.sort = AsyncResultsMerger::kWholeSortKeySortPattern;
        params.compareWholeSortKey = true;
        appendGeoNearDistanceProjection = true;
    }

    // Tailable cursors can't have a sort, which should have already been validated.
    invariant(params.sort.isEmpty() || !query.getQueryRequest().isTailable());

    // Construct the requests that we will use to establish cursors on the targeted shards,
    // attaching the shardVersion and txnNumber, if necessary.

    auto requests = constructRequestsForShards(
        opCtx, routingInfo, shardIds, query, appendGeoNearDistanceProjection);

    // Establish the cursors with a consistent shardVersion across shards.

    params.remotes = establishCursors(opCtx,
                                      Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                                      query.nss(),
                                      readPref,
                                      requests,
                                      query.getQueryRequest().isAllowPartialResults());

    // Determine whether the cursor we may eventually register will be single- or multi-target.

    const auto cursorType = params.remotes.size() > 1
        ? ClusterCursorManager::CursorType::MultiTarget
        : ClusterCursorManager::CursorType::SingleTarget;

    // Transfer the established cursors to a ClusterClientCursor.

    auto ccc = ClusterClientCursorImpl::make(
        opCtx, Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(), std::move(params));

    // Retrieve enough data from the ClusterClientCursor for the first batch of results.

    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &waitInFindBeforeMakingBatch, opCtx, "waitInFindBeforeMakingBatch");

    auto cursorState = ClusterCursorManager::CursorState::NotExhausted;
    int bytesBuffered = 0;

    while (!FindCommon::enoughForFirstBatch(query.getQueryRequest(), results->size())) {
        auto next = uassertStatusOK(ccc->next(RouterExecStage::ExecContext::kInitialFind));

        if (next.isEOF()) {
            // We reached end-of-stream. If the cursor is not tailable, then we mark it as
            // exhausted. If it is tailable, usually we keep it open (i.e. "NotExhausted") even
            // when we reach end-of-stream. However, if all the remote cursors are exhausted, there
            // is no hope of returning data and thus we need to close the mongos cursor as well.
            if (!ccc->isTailable() || ccc->remotesExhausted()) {
                cursorState = ClusterCursorManager::CursorState::Exhausted;
            }
            break;
        }

        auto nextObj = *next.getResult();

        // If adding this object will cause us to exceed the message size limit, then we stash it
        // for later.
        if (!FindCommon::haveSpaceForNext(nextObj, results->size(), bytesBuffered)) {
            ccc->queueResult(nextObj);
            break;
        }

        // Add doc to the batch. Account for the space overhead associated with returning this doc
        // inside a BSON array.
        bytesBuffered += (nextObj.objsize() + kPerDocumentOverheadBytesUpperBound);
        results->push_back(std::move(nextObj));
    }

    ccc->detachFromOperationContext();

    if (!query.getQueryRequest().wantMore() && !ccc->isTailable()) {
        cursorState = ClusterCursorManager::CursorState::Exhausted;
    }

    // Fill out query exec properties.
    CurOp::get(opCtx)->debug().nShards = ccc->getNumRemotes();
    CurOp::get(opCtx)->debug().nreturned = results->size();

    // If the cursor is exhausted, then there are no more results to return and we don't need to
    // allocate a cursor id.
    if (cursorState == ClusterCursorManager::CursorState::Exhausted) {
        CurOp::get(opCtx)->debug().cursorExhausted = true;
        return CursorId(0);
    }

    // Register the cursor with the cursor manager for subsequent getMore's.

    auto cursorManager = Grid::get(opCtx)->getCursorManager();
    const auto cursorLifetime = query.getQueryRequest().isNoCursorTimeout()
        ? ClusterCursorManager::CursorLifetime::Immortal
        : ClusterCursorManager::CursorLifetime::Mortal;
    auto authUsers = AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserNames();
    ccc->incNBatches();

    auto cursorId = uassertStatusOK(cursorManager->registerCursor(
        opCtx, ccc.releaseCursor(), query.nss(), cursorType, cursorLifetime, authUsers));

    // Record the cursorID in CurOp.
    CurOp::get(opCtx)->debug().cursorid = cursorId;
    return cursorId;
}

/**
 * Populates or re-populates some state of the OperationContext from what's stored on the cursor
 * and/or what's specified on the request.
 */
Status setUpOperationContextStateForGetMore(OperationContext* opCtx,
                                            const GetMoreRequest& request,
                                            ClusterCursorManager::PinnedCursor* cursor) {
    if (auto readPref = cursor->getReadPreference()) {
        ReadPreferenceSetting::get(opCtx) = *readPref;
    }

    if (cursor->isTailableAndAwaitData()) {
        // For tailable + awaitData cursors, the request may have indicated a maximum amount of time
        // to wait for new data. If not, default it to 1 second.  We track the deadline instead via
        // the 'waitForInsertsDeadline' decoration.
        auto timeout = request.awaitDataTimeout.value_or(Milliseconds{1000});
        awaitDataState(opCtx).waitForInsertsDeadline =
            opCtx->getServiceContext()->getPreciseClockSource()->now() + timeout;

        invariant(cursor->setAwaitDataTimeout(timeout).isOK());
    } else if (request.awaitDataTimeout) {
        return {ErrorCodes::BadValue,
                "maxTimeMS can only be used with getMore for tailable, awaitData cursors"};
    } else if (cursor->getLeftoverMaxTimeMicros() < Microseconds::max()) {
        // Be sure to do this only for non-tailable cursors.
        opCtx->setDeadlineAfterNowBy(cursor->getLeftoverMaxTimeMicros(),
                                     ErrorCodes::MaxTimeMSExpired);
    }
    return Status::OK();
}

}  // namespace

const size_t ClusterFind::kMaxRetries = 10;

CursorId ClusterFind::runQuery(OperationContext* opCtx,
                               const CanonicalQuery& query,
                               const ReadPreferenceSetting& readPref,
                               std::vector<BSONObj>* results) {
    invariant(results);

    // Projection on the reserved sort key field is illegal in mongos.
    if (query.getQueryRequest().getProj().hasField(AsyncResultsMerger::kSortKeyField)) {
        uasserted(ErrorCodes::BadValue,
                  str::stream() << "Projection contains illegal field '"
                                << AsyncResultsMerger::kSortKeyField
                                << "': "
                                << query.getQueryRequest().getProj());
    }

    auto const catalogCache = Grid::get(opCtx)->catalogCache();

    // Re-target and re-send the initial find command to the shards until we have established the
    // shard version.
    for (size_t retries = 1; retries <= kMaxRetries; ++retries) {
        auto routingInfoStatus = getCollectionRoutingInfoForTxnCmd(opCtx, query.nss());
        if (routingInfoStatus == ErrorCodes::NamespaceNotFound) {
            // If the database doesn't exist, we successfully return an empty result set without
            // creating a cursor.
            return CursorId(0);
        }

        auto routingInfo = uassertStatusOK(routingInfoStatus);

        try {
            return runQueryWithoutRetrying(opCtx, query, readPref, routingInfo, results);
        } catch (DBException& ex) {
            if (retries >= kMaxRetries) {
                // Check if there are no retries remaining, so the last received error can be
                // propagated to the caller.
                ex.addContext(str::stream() << "Failed to run query after " << kMaxRetries
                                            << " retries");
                throw;
            } else if (!ErrorCodes::isStaleShardVersionError(ex.code()) &&
                       ex.code() != ErrorCodes::ShardNotFound) {
                // Errors other than stale metadata or from trying to reach a non existent shard are
                // fatal to the operation. Network errors and replication retries happen at the
                // level of the AsyncResultsMerger.
                ex.addContext("Encountered non-retryable error during query");
                throw;
            }

            LOG(1) << "Received error status for query " << redact(query.toStringShort())
                   << " on attempt " << retries << " of " << kMaxRetries << ": " << redact(ex);

            catalogCache->onStaleShardVersion(std::move(routingInfo));

            if (auto txnRouter = TransactionRouter::get(opCtx)) {
                // A transaction can always continue on a stale version error during find because
                // the operation must be idempotent. Reset the default global read timestamp so the
                // retry's routing table reflects the chunk placement after the refresh (no-op if
                // the transaction is not running with snapshot read concern).
                txnRouter->onStaleShardOrDbError(kFindCmdName, ex.toStatus());
                txnRouter->setDefaultAtClusterTime(opCtx);
            }
        }
    }

    MONGO_UNREACHABLE
}

/**
 * Validates that the lsid on the OperationContext matches that on the cursor, returning it to the
 * ClusterClusterCursor manager if it does not.
 */
void validateLSID(OperationContext* opCtx,
                  const GetMoreRequest& request,
                  ClusterCursorManager::PinnedCursor* cursor) {
    if (opCtx->getLogicalSessionId() && !cursor->getLsid()) {
        uasserted(50799,
                  str::stream() << "Cannot run getMore on cursor " << request.cursorid
                                << ", which was not created in a session, in session "
                                << *opCtx->getLogicalSessionId());
    }

    if (!opCtx->getLogicalSessionId() && cursor->getLsid()) {
        uasserted(50800,
                  str::stream() << "Cannot run getMore on cursor " << request.cursorid
                                << ", which was created in session "
                                << *cursor->getLsid()
                                << ", without an lsid");
    }

    if (opCtx->getLogicalSessionId() && cursor->getLsid() &&
        (*opCtx->getLogicalSessionId() != *cursor->getLsid())) {
        uasserted(50801,
                  str::stream() << "Cannot run getMore on cursor " << request.cursorid
                                << ", which was created in session "
                                << *cursor->getLsid()
                                << ", in session "
                                << *opCtx->getLogicalSessionId());
    }
}

/**
 * Validates that the txnNumber on the OperationContext matches that on the cursor, returning it to
 * the ClusterClusterCursor manager if it does not.
 */
void validateTxnNumber(OperationContext* opCtx,
                       const GetMoreRequest& request,
                       ClusterCursorManager::PinnedCursor* cursor) {
    if (opCtx->getTxnNumber() && !cursor->getTxnNumber()) {
        uasserted(50802,
                  str::stream() << "Cannot run getMore on cursor " << request.cursorid
                                << ", which was not created in a transaction, in transaction "
                                << *opCtx->getTxnNumber());
    }

    if (!opCtx->getTxnNumber() && cursor->getTxnNumber()) {
        uasserted(50803,
                  str::stream() << "Cannot run getMore on cursor " << request.cursorid
                                << ", which was created in transaction "
                                << *cursor->getTxnNumber()
                                << ", without a txnNumber");
    }

    if (opCtx->getTxnNumber() && cursor->getTxnNumber() &&
        (*opCtx->getTxnNumber() != *cursor->getTxnNumber())) {
        uasserted(50804,
                  str::stream() << "Cannot run getMore on cursor " << request.cursorid
                                << ", which was created in transaction "
                                << *cursor->getTxnNumber()
                                << ", in transaction "
                                << *opCtx->getTxnNumber());
    }
}

/**
 * Validates that the OperationSessionInfo (i.e. txnNumber and lsid) on the OperationContext match
 * that stored on the cursor. The cursor is returned to the ClusterCursorManager if it does not.
 */
void validateOperationSessionInfo(OperationContext* opCtx,
                                  const GetMoreRequest& request,
                                  ClusterCursorManager::PinnedCursor* cursor) {
    auto returnCursorGuard = makeGuard(
        [cursor] { cursor->returnCursor(ClusterCursorManager::CursorState::NotExhausted); });
    validateLSID(opCtx, request, cursor);
    validateTxnNumber(opCtx, request, cursor);
    returnCursorGuard.dismiss();
}

StatusWith<CursorResponse> ClusterFind::runGetMore(OperationContext* opCtx,
                                                   const GetMoreRequest& request) {
    auto cursorManager = Grid::get(opCtx)->getCursorManager();

    auto authzSession = AuthorizationSession::get(opCtx->getClient());
    auto authChecker = [&authzSession](UserNameIterator userNames) -> Status {
        return authzSession->isCoauthorizedWith(userNames)
            ? Status::OK()
            : Status(ErrorCodes::Unauthorized, "User not authorized to access cursor");
    };

    auto pinnedCursor =
        cursorManager->checkOutCursor(request.nss, request.cursorid, opCtx, authChecker);
    if (!pinnedCursor.isOK()) {
        return pinnedCursor.getStatus();
    }
    invariant(request.cursorid == pinnedCursor.getValue().getCursorId());

    validateOperationSessionInfo(opCtx, request, &pinnedCursor.getValue());

    // Set the originatingCommand object and the cursorID in CurOp.
    {
        CurOp::get(opCtx)->debug().nShards = pinnedCursor.getValue().getNumRemotes();
        CurOp::get(opCtx)->debug().cursorid = request.cursorid;
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setOriginatingCommand_inlock(
            pinnedCursor.getValue().getOriginatingCommand());
        CurOp::get(opCtx)->setGenericCursor_inlock(pinnedCursor.getValue().toGenericCursor());
    }

    // If the 'waitAfterPinningCursorBeforeGetMoreBatch' fail point is enabled, set the 'msg'
    // field of this operation's CurOp to signal that we've hit this point.
    if (MONGO_FAIL_POINT(waitAfterPinningCursorBeforeGetMoreBatch)) {
        CurOpFailpointHelpers::waitWhileFailPointEnabled(
            &waitAfterPinningCursorBeforeGetMoreBatch,
            opCtx,
            "waitAfterPinningCursorBeforeGetMoreBatch");
    }

    auto opCtxSetupStatus =
        setUpOperationContextStateForGetMore(opCtx, request, &pinnedCursor.getValue());
    if (!opCtxSetupStatus.isOK()) {
        return opCtxSetupStatus;
    }

    std::vector<BSONObj> batch;
    int bytesBuffered = 0;
    long long batchSize = request.batchSize.value_or(0);
    long long startingFrom = pinnedCursor.getValue().getNumReturnedSoFar();
    auto cursorState = ClusterCursorManager::CursorState::NotExhausted;
    BSONObj postBatchResumeToken;
    bool stashedResult = false;

    while (!FindCommon::enoughForGetMore(batchSize, batch.size())) {
        auto context = batch.empty()
            ? RouterExecStage::ExecContext::kGetMoreNoResultsYet
            : RouterExecStage::ExecContext::kGetMoreWithAtLeastOneResultInBatch;

        StatusWith<ClusterQueryResult> next =
            Status{ErrorCodes::InternalError, "uninitialized cluster query result"};
        try {
            next = pinnedCursor.getValue().next(context);
        } catch (const ExceptionFor<ErrorCodes::CloseChangeStream>&) {
            // This exception is thrown when a $changeStream stage encounters an event
            // that invalidates the cursor. We should close the cursor and return without
            // error.
            cursorState = ClusterCursorManager::CursorState::Exhausted;
            break;
        }

        if (!next.isOK()) {
            return next.getStatus();
        }

        if (next.getValue().isEOF()) {
            // We reached end-of-stream. If the cursor is not tailable, then we mark it as
            // exhausted. If it is tailable, usually we keep it open (i.e. "NotExhausted") even when
            // we reach end-of-stream. However, if all the remote cursors are exhausted, there is no
            // hope of returning data and thus we need to close the mongos cursor as well.
            if (!pinnedCursor.getValue().isTailable() ||
                pinnedCursor.getValue().remotesExhausted()) {
                cursorState = ClusterCursorManager::CursorState::Exhausted;
            }
            break;
        }

        if (!FindCommon::haveSpaceForNext(
                *next.getValue().getResult(), batch.size(), bytesBuffered)) {
            pinnedCursor.getValue().queueResult(*next.getValue().getResult());
            stashedResult = true;
            break;
        }

        // Add doc to the batch. Account for the space overhead associated with returning this doc
        // inside a BSON array.
        bytesBuffered +=
            (next.getValue().getResult()->objsize() + kPerDocumentOverheadBytesUpperBound);
        batch.push_back(std::move(*next.getValue().getResult()));

        // Update the postBatchResumeToken. For non-$changeStream aggregations, this will be empty.
        postBatchResumeToken = pinnedCursor.getValue().getPostBatchResumeToken();
    }

    // For empty batches, or in the case where the final result was added to the batch rather than
    // being stashed, we update the PBRT here to ensure that it is the most recent available.
    if (!stashedResult) {
        postBatchResumeToken = pinnedCursor.getValue().getPostBatchResumeToken();
    }

    pinnedCursor.getValue().setLeftoverMaxTimeMicros(opCtx->getRemainingMaxTimeMicros());
    pinnedCursor.getValue().incNBatches();
    // Upon successful completion, transfer ownership of the cursor back to the cursor manager. If
    // the cursor has been exhausted, the cursor manager will clean it up for us.
    pinnedCursor.getValue().returnCursor(cursorState);

    CursorId idToReturn = (cursorState == ClusterCursorManager::CursorState::Exhausted)
        ? CursorId(0)
        : request.cursorid;

    // Set nReturned and whether the cursor has been exhausted.
    CurOp::get(opCtx)->debug().cursorExhausted = (idToReturn == 0);
    CurOp::get(opCtx)->debug().nreturned = batch.size();

    if (MONGO_FAIL_POINT(waitBeforeUnpinningOrDeletingCursorAfterGetMoreBatch)) {
        CurOpFailpointHelpers::waitWhileFailPointEnabled(
            &waitBeforeUnpinningOrDeletingCursorAfterGetMoreBatch,
            opCtx,
            "waitBeforeUnpinningOrDeletingCursorAfterGetMoreBatch");
    }

    return CursorResponse(
        request.nss, idToReturn, std::move(batch), startingFrom, boost::none, postBatchResumeToken);
}

}  // namespace mongo
