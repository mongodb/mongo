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
#include "mongo/db/commands.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/platform/overflow_arithmetic.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/commands/cluster_commands_common.h"
#include "mongo/s/config.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_client_cursor_impl.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/query/store_possible_cursor.h"
#include "mongo/s/stale_exception.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

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
    invariantOK(newQR->validate());
    return std::move(newQR);
}

StatusWith<CursorId> runQueryWithoutRetrying(OperationContext* txn,
                                             const CanonicalQuery& query,
                                             const ReadPreferenceSetting& readPref,
                                             ChunkManager* chunkManager,
                                             std::shared_ptr<Shard> primary,
                                             std::vector<BSONObj>* results) {
    auto shardRegistry = grid.shardRegistry();

    // Get the set of shards on which we will run the query.
    std::vector<std::shared_ptr<Shard>> shards;
    if (primary) {
        shards.emplace_back(std::move(primary));
    } else {
        invariant(chunkManager);

        std::set<ShardId> shardIds;
        chunkManager->getShardIdsForQuery(txn, query.getQueryRequest().getFilter(), &shardIds);

        for (auto id : shardIds) {
            auto shard = shardRegistry->getShard(txn, id);
            if (!shard) {
                return {ErrorCodes::ShardNotFound,
                        str::stream() << "Shard with id:  " << id << " is not found."};
            }
            shards.emplace_back(shard);
        }
    }

    ClusterClientCursorParams params(query.nss(), readPref);
    params.limit = query.getQueryRequest().getLimit();
    params.batchSize = query.getQueryRequest().getEffectiveBatchSize();
    params.skip = query.getQueryRequest().getSkip();
    params.isTailable = query.getQueryRequest().isTailable();
    params.isAwaitData = query.getQueryRequest().isAwaitData();
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
    invariant(params.sort.isEmpty() || !params.isTailable);

    const auto qrToForward = transformQueryForShards(query.getQueryRequest());
    if (!qrToForward.isOK()) {
        return qrToForward.getStatus();
    }

    // Use read pref to target a particular host from each shard. Also construct the find command
    // that we will forward to each shard.
    for (const auto& shard : shards) {
        invariant(!shard->isConfig() || shard->getConnString().type() != ConnectionString::INVALID);

        // Build the find command, and attach shard version if necessary.
        BSONObjBuilder cmdBuilder;
        qrToForward.getValue()->asFindCommand(&cmdBuilder);

        if (chunkManager) {
            ChunkVersion version(chunkManager->getVersion(shard->getId()));
            version.appendForCommands(&cmdBuilder);
        } else if (!query.nss().isOnInternalDb()) {
            ChunkVersion version(ChunkVersion::UNSHARDED());
            version.appendForCommands(&cmdBuilder);
        }

        params.remotes.emplace_back(shard->getId(), cmdBuilder.obj());
    }

    auto ccc = ClusterClientCursorImpl::make(
        Grid::get(txn)->getExecutorPool()->getArbitraryExecutor(), std::move(params));

    auto cursorState = ClusterCursorManager::CursorState::NotExhausted;
    int bytesBuffered = 0;
    while (!FindCommon::enoughForFirstBatch(query.getQueryRequest(), results->size())) {
        auto next = ccc->next();
        if (!next.isOK()) {
            return next.getStatus();
        }

        if (!next.getValue()) {
            // We reached end-of-stream. If the cursor is not tailable, then we mark it as
            // exhausted. If it is tailable, usually we keep it open (i.e. "NotExhausted") even
            // when we reach end-of-stream. However, if all the remote cursors are exhausted, there
            // is no hope of returning data and thus we need to close the mongos cursor as well.
            if (!ccc->isTailable() || ccc->remotesExhausted()) {
                cursorState = ClusterCursorManager::CursorState::Exhausted;
            }
            break;
        }

        // If adding this object will cause us to exceed the message size limit, then we stash it
        // for later.
        if (!FindCommon::haveSpaceForNext(*next.getValue(), results->size(), bytesBuffered)) {
            ccc->queueResult(*next.getValue());
            break;
        }

        // Add doc to the batch. Account for the space overhead associated with returning this doc
        // inside a BSON array.
        bytesBuffered += (next.getValue()->objsize() + kPerDocumentOverheadBytesUpperBound);
        results->push_back(std::move(*next.getValue()));
    }

    if (!query.getQueryRequest().wantMore() && !ccc->isTailable()) {
        cursorState = ClusterCursorManager::CursorState::Exhausted;
    }

    // If the cursor is exhausted, then there are no more results to return and we don't need to
    // allocate a cursor id.
    if (cursorState == ClusterCursorManager::CursorState::Exhausted) {
        return CursorId(0);
    }

    // Register the cursor with the cursor manager.
    auto cursorManager = grid.getCursorManager();
    const auto cursorType = chunkManager ? ClusterCursorManager::CursorType::NamespaceSharded
                                         : ClusterCursorManager::CursorType::NamespaceNotSharded;
    const auto cursorLifetime = query.getQueryRequest().isNoCursorTimeout()
        ? ClusterCursorManager::CursorLifetime::Immortal
        : ClusterCursorManager::CursorLifetime::Mortal;
    return cursorManager->registerCursor(
        ccc.releaseCursor(), query.nss(), cursorType, cursorLifetime);
}

}  // namespace

const size_t ClusterFind::kMaxStaleConfigRetries = 10;

StatusWith<CursorId> ClusterFind::runQuery(OperationContext* txn,
                                           const CanonicalQuery& query,
                                           const ReadPreferenceSetting& readPref,
                                           std::vector<BSONObj>* results) {
    invariant(results);

    // Projection on the reserved sort key field is illegal in mongos.
    if (query.getQueryRequest().getProj().hasField(ClusterClientCursorParams::kSortKeyField)) {
        return {ErrorCodes::BadValue,
                str::stream() << "Projection contains illegal field '"
                              << ClusterClientCursorParams::kSortKeyField
                              << "': "
                              << query.getQueryRequest().getProj()};
    }

    auto dbConfig = grid.catalogCache()->getDatabase(txn, query.nss().db().toString());
    if (dbConfig.getStatus() == ErrorCodes::NamespaceNotFound) {
        // If the database doesn't exist, we successfully return an empty result set without
        // creating a cursor.
        return CursorId(0);
    } else if (!dbConfig.isOK()) {
        return dbConfig.getStatus();
    }

    std::shared_ptr<ChunkManager> chunkManager;
    std::shared_ptr<Shard> primary;
    dbConfig.getValue()->getChunkManagerOrPrimary(txn, query.nss().ns(), chunkManager, primary);

    // Re-target and re-send the initial find command to the shards until we have established the
    // shard version.
    for (size_t retries = 1; retries <= kMaxStaleConfigRetries; ++retries) {
        auto cursorId = runQueryWithoutRetrying(
            txn, query, readPref, chunkManager.get(), std::move(primary), results);
        if (cursorId.isOK()) {
            return cursorId;
        }
        auto status = std::move(cursorId.getStatus());

        if (!ErrorCodes::isStaleShardingError(status.code()) &&
            status != ErrorCodes::ShardNotFound) {
            // Errors other than trying to reach a non existent shard or receiving a stale
            // metadata message from MongoD are fatal to the operation. Network errors and
            // replication retries happen at the level of the AsyncResultsMerger.
            return status;
        }

        LOG(1) << "Received error status for query " << query.toStringShort() << " on attempt "
               << retries << " of " << kMaxStaleConfigRetries << ": " << status;

        const bool staleEpoch = (status == ErrorCodes::StaleEpoch);
        if (staleEpoch) {
            if (!dbConfig.getValue()->reload(txn)) {
                // If the reload failed that means the database wasn't found, so successfully return
                // an empty result set without creating a cursor.
                return CursorId(0);
            }
        }
        chunkManager =
            dbConfig.getValue()->getChunkManagerIfExists(txn, query.nss().ns(), true, staleEpoch);
        if (!chunkManager) {
            dbConfig.getValue()->getChunkManagerOrPrimary(
                txn, query.nss().ns(), chunkManager, primary);
        }
    }

    return {ErrorCodes::StaleShardVersion,
            str::stream() << "Retried " << kMaxStaleConfigRetries
                          << " times without successfully establishing shard version."};
}

StatusWith<CursorResponse> ClusterFind::runGetMore(OperationContext* txn,
                                                   const GetMoreRequest& request) {
    auto cursorManager = grid.getCursorManager();

    auto pinnedCursor = cursorManager->checkOutCursor(request.nss, request.cursorid);
    if (!pinnedCursor.isOK()) {
        return pinnedCursor.getStatus();
    }
    invariant(request.cursorid == pinnedCursor.getValue().getCursorId());

    // If the fail point is enabled, busy wait until it is disabled.
    while (MONGO_FAIL_POINT(keepCursorPinnedDuringGetMore)) {
    }

    if (request.awaitDataTimeout) {
        auto status = pinnedCursor.getValue().setAwaitDataTimeout(*request.awaitDataTimeout);
        if (!status.isOK()) {
            return status;
        }
    }

    std::vector<BSONObj> batch;
    int bytesBuffered = 0;
    long long batchSize = request.batchSize.value_or(0);
    long long startingFrom = pinnedCursor.getValue().getNumReturnedSoFar();
    auto cursorState = ClusterCursorManager::CursorState::NotExhausted;
    while (!FindCommon::enoughForGetMore(batchSize, batch.size())) {
        auto next = pinnedCursor.getValue().next();
        if (!next.isOK()) {
            return next.getStatus();
        }

        if (!next.getValue()) {
            // We reached end-of-stream.
            if (!pinnedCursor.getValue().isTailable()) {
                cursorState = ClusterCursorManager::CursorState::Exhausted;
            }
            break;
        }

        if (!FindCommon::haveSpaceForNext(*next.getValue(), batch.size(), bytesBuffered)) {
            pinnedCursor.getValue().queueResult(*next.getValue());
            break;
        }

        // Add doc to the batch. Account for the space overhead associated with returning this doc
        // inside a BSON array.
        bytesBuffered += (next.getValue()->objsize() + kPerDocumentOverheadBytesUpperBound);
        batch.push_back(std::move(*next.getValue()));
    }

    // Transfer ownership of the cursor back to the cursor manager.
    pinnedCursor.getValue().returnCursor(cursorState);

    CursorId idToReturn = (cursorState == ClusterCursorManager::CursorState::Exhausted)
        ? CursorId(0)
        : request.cursorid;
    return CursorResponse(request.nss, idToReturn, std::move(batch), startingFrom);
}

StatusWith<ReadPreferenceSetting> ClusterFind::extractUnwrappedReadPref(const BSONObj& cmdObj,
                                                                        const bool isSlaveOk) {
    BSONElement queryOptionsElt;
    auto status = bsonExtractTypedField(
        cmdObj, QueryRequest::kUnwrappedReadPrefField, BSONType::Object, &queryOptionsElt);
    if (status.isOK()) {
        // There must be a nested object containing the read preference if there is a queryOptions
        // field.
        BSONObj queryOptionsObj = queryOptionsElt.Obj();
        invariant(queryOptionsObj[QueryRequest::kWrappedReadPrefField].type() == BSONType::Object);
        BSONObj readPrefObj = queryOptionsObj[QueryRequest::kWrappedReadPrefField].Obj();

        auto readPref = ReadPreferenceSetting::fromBSON(readPrefObj);
        if (!readPref.isOK()) {
            return readPref.getStatus();
        }
        return readPref.getValue();
    } else if (status != ErrorCodes::NoSuchKey) {
        return status;
    }

    // If there is no explicit read preference, the value we use depends on the setting of the slave
    // ok bit.
    ReadPreference pref =
        isSlaveOk ? mongo::ReadPreference::SecondaryPreferred : mongo::ReadPreference::PrimaryOnly;
    return ReadPreferenceSetting(pref, TagSet());
}

}  // namespace mongo
