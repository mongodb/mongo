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

#include "mongo/s/query/cluster_find.h"

#include <set>
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connpool.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/platform/overflow_arithmetic.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_client_cursor_impl.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/query/establish_cursors.h"
#include "mongo/s/query/store_possible_cursor.h"
#include "mongo/s/stale_exception.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

namespace {

static const BSONObj kSortKeyMetaProjection = BSON("$meta"
                                                   << "sortKey");

// We must allow some amount of overhead per result document, since when we make a cursor response
// the documents are elements of a BSONArray. The overhead is 1 byte/doc for the type + 1 byte/doc
// for the field name's null terminator + 1 byte per digit in the array index. The index can be no
// more than 8 decimal digits since the response is at most 16MB, and 16 * 1024 * 1024 < 1 * 10^8.
static const int kPerDocumentOverheadBytesUpperBound = 10;

/**
 * Given the QueryRequest 'qr' being executed by mongos, returns a copy of the query which is
 * suitable for forwarding to the targeted hosts.
 */
StatusWith<std::unique_ptr<QueryRequest>> transformQueryForShards(const QueryRequest& qr) {
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
        projectionBuilder.append(ClusterClientCursorParams::kSortKeyField, kSortKeyMetaProjection);
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

    invariantOK(newQR->validate());
    return std::move(newQR);
}

StatusWith<CursorId> runQueryWithoutRetrying(OperationContext* opCtx,
                                             const CanonicalQuery& query,
                                             const ReadPreferenceSetting& readPref,
                                             ChunkManager* chunkManager,
                                             std::shared_ptr<Shard> primary,
                                             std::vector<BSONObj>* results,
                                             BSONObj* viewDefinition) {
    auto shardRegistry = Grid::get(opCtx)->shardRegistry();

    // Get the set of shards on which we will run the query.

    std::vector<std::shared_ptr<Shard>> shards;
    if (primary) {
        shards.emplace_back(std::move(primary));
    } else {
        invariant(chunkManager);

        std::set<ShardId> shardIds;
        chunkManager->getShardIdsForQuery(opCtx,
                                          query.getQueryRequest().getFilter(),
                                          query.getQueryRequest().getCollation(),
                                          &shardIds);

        for (auto id : shardIds) {
            auto shardStatus = shardRegistry->getShard(opCtx, id);
            if (!shardStatus.isOK()) {
                return shardStatus.getStatus();
            }
            shards.emplace_back(shardStatus.getValue());
        }
    }

    // Construct the query and parameters.

    ClusterClientCursorParams params(
        query.nss(),
        AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserNames(),
        readPref);
    params.limit = query.getQueryRequest().getLimit();
    params.batchSize = query.getQueryRequest().getEffectiveBatchSize();
    params.skip = query.getQueryRequest().getSkip();
    params.tailableMode = query.getQueryRequest().getTailableMode();
    params.isAllowPartialResults = query.getQueryRequest().isAllowPartialResults();

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

    // Tailable cursors can't have a sort, which should have already been validated.
    invariant(params.sort.isEmpty() || !query.getQueryRequest().isTailable());

    const auto qrToForward = transformQueryForShards(query.getQueryRequest());
    if (!qrToForward.isOK()) {
        return qrToForward.getStatus();
    }

    // Construct the find command that we will use to establish cursors, attaching the shardVersion.

    std::vector<std::pair<ShardId, BSONObj>> requests;
    for (const auto& shard : shards) {
        invariant(!shard->isConfig() || shard->getConnString().type() != ConnectionString::INVALID);

        BSONObjBuilder cmdBuilder;
        qrToForward.getValue()->asFindCommand(&cmdBuilder);

        if (chunkManager) {
            ChunkVersion version(chunkManager->getVersion(shard->getId()));
            version.appendForCommands(&cmdBuilder);
        } else if (!query.nss().isOnInternalDb()) {
            ChunkVersion version(ChunkVersion::UNSHARDED());
            version.appendForCommands(&cmdBuilder);
        }

        requests.emplace_back(shard->getId(), cmdBuilder.obj());
    }

    // Establish the cursors with a consistent shardVersion across shards.

    auto swCursors = establishCursors(opCtx,
                                      Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                                      query.nss(),
                                      readPref,
                                      requests,
                                      query.getQueryRequest().isAllowPartialResults(),
                                      viewDefinition);
    if (!swCursors.isOK()) {
        // Make sure a viewDefinition was set if the find was on a view.
        if (ErrorCodes::CommandOnShardedViewNotSupportedOnMongod == swCursors.getStatus().code()) {
            if (!viewDefinition) {
                return {ErrorCodes::InternalError,
                        str::stream() << "Missing resolved view definition, but remote returned "
                                      << ErrorCodes::errorString(swCursors.getStatus().code())};
            }
        }
        return swCursors.getStatus();
    }

    // Determine whether the cursor we may eventually register will be single- or multi-target.

    const auto cursorType = swCursors.getValue().size() > 1
        ? ClusterCursorManager::CursorType::MultiTarget
        : ClusterCursorManager::CursorType::SingleTarget;

    // Transfer the established cursors to a ClusterClientCursor.

    params.remotes = std::move(swCursors.getValue());
    auto ccc = ClusterClientCursorImpl::make(
        opCtx, Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(), std::move(params));

    // Retrieve enough data from the ClusterClientCursor for the first batch of results.

    auto cursorState = ClusterCursorManager::CursorState::NotExhausted;
    int bytesBuffered = 0;

    while (!FindCommon::enoughForFirstBatch(query.getQueryRequest(), results->size())) {
        auto next = ccc->next(RouterExecStage::ExecContext::kInitialFind);

        if (!next.isOK()) {
            return next.getStatus();
        }

        if (next.getValue().isEOF()) {
            // We reached end-of-stream. If the cursor is not tailable, then we mark it as
            // exhausted. If it is tailable, usually we keep it open (i.e. "NotExhausted") even
            // when we reach end-of-stream. However, if all the remote cursors are exhausted, there
            // is no hope of returning data and thus we need to close the mongos cursor as well.
            if (!ccc->isTailable() || ccc->remotesExhausted()) {
                cursorState = ClusterCursorManager::CursorState::Exhausted;
            }
            break;
        }

        auto nextObj = *next.getValue().getResult();

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

    // If the cursor is exhausted, then there are no more results to return and we don't need to
    // allocate a cursor id.
    if (cursorState == ClusterCursorManager::CursorState::Exhausted) {
        return CursorId(0);
    }

    // Register the cursor with the cursor manager for subsequent getMore's.

    auto cursorManager = Grid::get(opCtx)->getCursorManager();
    const auto cursorLifetime = query.getQueryRequest().isNoCursorTimeout()
        ? ClusterCursorManager::CursorLifetime::Immortal
        : ClusterCursorManager::CursorLifetime::Mortal;
    return cursorManager->registerCursor(
        opCtx, ccc.releaseCursor(), query.nss(), cursorType, cursorLifetime);
}

}  // namespace

const size_t ClusterFind::kMaxStaleConfigRetries = 10;

StatusWith<CursorId> ClusterFind::runQuery(OperationContext* opCtx,
                                           const CanonicalQuery& query,
                                           const ReadPreferenceSetting& readPref,
                                           std::vector<BSONObj>* results,
                                           BSONObj* viewDefinition) {
    invariant(results);

    // Projection on the reserved sort key field is illegal in mongos.
    if (query.getQueryRequest().getProj().hasField(ClusterClientCursorParams::kSortKeyField)) {
        return {ErrorCodes::BadValue,
                str::stream() << "Projection contains illegal field '"
                              << ClusterClientCursorParams::kSortKeyField
                              << "': "
                              << query.getQueryRequest().getProj()};
    }

    auto const catalogCache = Grid::get(opCtx)->catalogCache();

    // Re-target and re-send the initial find command to the shards until we have established the
    // shard version.
    for (size_t retries = 1; retries <= kMaxStaleConfigRetries; ++retries) {
        auto routingInfoStatus = catalogCache->getCollectionRoutingInfo(opCtx, query.nss());
        if (routingInfoStatus == ErrorCodes::NamespaceNotFound) {
            // If the database doesn't exist, we successfully return an empty result set without
            // creating a cursor.
            return CursorId(0);
        } else if (!routingInfoStatus.isOK()) {
            return routingInfoStatus.getStatus();
        }

        auto& routingInfo = routingInfoStatus.getValue();

        auto cursorId = runQueryWithoutRetrying(opCtx,
                                                query,
                                                readPref,
                                                routingInfo.cm().get(),
                                                routingInfo.primary(),
                                                results,
                                                viewDefinition);
        if (cursorId.isOK()) {
            return cursorId;
        }

        const auto& status = cursorId.getStatus();

        if (!ErrorCodes::isStaleShardingError(status.code()) &&
            status != ErrorCodes::ShardNotFound) {
            // Errors other than trying to reach a non existent shard or receiving a stale
            // metadata message from MongoD are fatal to the operation. Network errors and
            // replication retries happen at the level of the AsyncResultsMerger.
            return status;
        }

        LOG(1) << "Received error status for query " << redact(query.toStringShort())
               << " on attempt " << retries << " of " << kMaxStaleConfigRetries << ": "
               << redact(status);

        catalogCache->onStaleConfigError(std::move(routingInfo));
    }

    return {ErrorCodes::StaleShardVersion,
            str::stream() << "Retried " << kMaxStaleConfigRetries
                          << " times without successfully establishing shard version."};
}

StatusWith<CursorResponse> ClusterFind::runGetMore(OperationContext* opCtx,
                                                   const GetMoreRequest& request) {
    auto cursorManager = Grid::get(opCtx)->getCursorManager();

    auto pinnedCursor = cursorManager->checkOutCursor(request.nss, request.cursorid, opCtx);
    if (!pinnedCursor.isOK()) {
        return pinnedCursor.getStatus();
    }
    invariant(request.cursorid == pinnedCursor.getValue().getCursorId());

    // If the fail point is enabled, busy wait until it is disabled.
    while (MONGO_FAIL_POINT(keepCursorPinnedDuringGetMore)) {
    }

    // A user can only call getMore on their own cursor. If there were multiple users authenticated
    // when the cursor was created, then at least one of them must be authenticated in order to run
    // getMore on the cursor.
    if (!AuthorizationSession::get(opCtx->getClient())
             ->isCoauthorizedWith(pinnedCursor.getValue().getAuthenticatedUsers())) {
        // Return the cursor so that it's not killed.
        pinnedCursor.getValue().returnCursor(ClusterCursorManager::CursorState::NotExhausted);
        return {ErrorCodes::Unauthorized,
                str::stream() << "cursor id " << request.cursorid
                              << " was not created by the authenticated user"};
    }

    if (auto readPref = pinnedCursor.getValue().getReadPreference()) {
        ReadPreferenceSetting::get(opCtx) = *readPref;
    }
    if (pinnedCursor.getValue().isTailableAndAwaitData()) {
        // A maxTimeMS specified on a tailable, awaitData cursor is special. Instead of imposing a
        // deadline on the operation, it is used to communicate how long the server should wait for
        // new results. Here we clear any deadline set during command processing and track the
        // deadline instead via the 'waitForInsertsDeadline' decoration. This deadline defaults to
        // 1 second if the user didn't specify a maxTimeMS.
        opCtx->clearDeadline();
        auto timeout = request.awaitDataTimeout.value_or(Milliseconds{1000});
        awaitDataState(opCtx).waitForInsertsDeadline =
            opCtx->getServiceContext()->getPreciseClockSource()->now() + timeout;

        invariant(pinnedCursor.getValue().setAwaitDataTimeout(timeout).isOK());
    } else if (request.awaitDataTimeout) {
        return {ErrorCodes::BadValue,
                "maxTimeMS can only be used with getMore for tailable, awaitData cursors"};
    }

    std::vector<BSONObj> batch;
    int bytesBuffered = 0;
    long long batchSize = request.batchSize.value_or(0);
    long long startingFrom = pinnedCursor.getValue().getNumReturnedSoFar();
    auto cursorState = ClusterCursorManager::CursorState::NotExhausted;

    pinnedCursor.getValue().reattachToOperationContext(opCtx);

    // A pinned cursor will not be destroyed immediately if an exception is thrown. Instead it will
    // be marked as killed, then reaped by a background thread later. If this happens, we want to be
    // sure the cursor does not have a pointer to this OperationContext, since it will be destroyed
    // as soon as we return, but the cursor will live on a bit longer.
    ScopeGuard cursorDetach =
        MakeGuard([&pinnedCursor]() { pinnedCursor.getValue().detachFromOperationContext(); });

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
            break;
        }

        // Add doc to the batch. Account for the space overhead associated with returning this doc
        // inside a BSON array.
        bytesBuffered +=
            (next.getValue().getResult()->objsize() + kPerDocumentOverheadBytesUpperBound);
        batch.push_back(std::move(*next.getValue().getResult()));
    }

    // Upon successful completion, we need to detach from the operation and transfer ownership of
    // the cursor back to the cursor manager.
    cursorDetach.Dismiss();
    pinnedCursor.getValue().detachFromOperationContext();
    pinnedCursor.getValue().returnCursor(cursorState);

    CursorId idToReturn = (cursorState == ClusterCursorManager::CursorState::Exhausted)
        ? CursorId(0)
        : request.cursorid;
    return CursorResponse(request.nss, idToReturn, std::move(batch), startingFrom);
}

}  // namespace mongo
