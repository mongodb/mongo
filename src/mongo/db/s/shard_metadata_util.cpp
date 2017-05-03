/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/s/shard_metadata_util.h"

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_shard_collection.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/stdx/memory.h"

namespace mongo {
namespace shardmetadatautil {

namespace {

const WriteConcernOptions kLocalWriteConcern(1,
                                             WriteConcernOptions::SyncMode::UNSET,
                                             Milliseconds(0));

/**
 * Structure representing the generated query and sort order for a chunk diffing operation.
 */
struct QueryAndSort {
    const BSONObj query;
    const BSONObj sort;
};

/**
 * Returns the query needed to find incremental changes to the chunks collection on a shard server.
 *
 * The query has to find all the chunks $gte the current max version. Currently, any splits, merges
 * and moves will increment the current max version. Querying by lastmod is essential because we
 * want to use the {lastmod} index on the chunks collection. This makes potential cursor yields to
 * apply split/merge/move updates safe: updates always move or insert documents at the end of the
 * index (because the document updates always have higher lastmod), so changed always come *after*
 * our current cursor position and are seen when the cursor recommences.
 *
 * The sort must be by ascending version so that the updates can be applied in-memory in order. This
 * is important because it is possible for a cursor to read updates to the same _id document twice,
 * due to the yield described above. If updates are applied in ascending version order, the later
 * update is applied last and remains.
 */
QueryAndSort createShardChunkDiffQuery(const ChunkVersion& collectionVersion) {
    return {BSON(ChunkType::DEPRECATED_lastmod() << GTE << Timestamp(collectionVersion.toLong())),
            BSON(ChunkType::DEPRECATED_lastmod() << 1)};
}

}  // namespace

bool RefreshState::operator==(RefreshState& other) {
    return (other.epoch == epoch) && (other.refreshing == refreshing) &&
        (other.sequenceNumber == sequenceNumber);
}

Status setPersistedRefreshFlags(OperationContext* opCtx, const NamespaceString& nss) {
    // Set 'refreshing' to true.
    BSONObj update = BSON(ShardCollectionType::refreshing() << true);
    return updateShardCollectionsEntry(
        opCtx, BSON(ShardCollectionType::uuid() << nss.ns()), update, BSONObj(), false /*upsert*/);
}

Status unsetPersistedRefreshFlags(OperationContext* opCtx, const NamespaceString& nss) {
    // Set 'refreshing' to false and increment the sequence number so it's differs from the last
    // stable state. Note: incrementing a non-existent field sets the field to the increment value,
    // so such a situation is safe.
    BSONObj update = BSON(ShardCollectionType::refreshing()
                          << false
                          << "$inc"
                          << BSON(ShardCollectionType::refreshSequenceNumber() << 1));

    return updateShardCollectionsEntry(opCtx,
                                       BSON(ShardCollectionType::uuid() << nss.ns()),
                                       BSON(ShardCollectionType::refreshing() << false),
                                       BSON(ShardCollectionType::refreshSequenceNumber() << 1),
                                       false /*upsert*/);
}

StatusWith<RefreshState> getPersistedRefreshFlags(OperationContext* opCtx,
                                                  const NamespaceString& nss) {
    auto statusWithCollectionEntry = readShardCollectionsEntry(opCtx, nss);
    if (!statusWithCollectionEntry.isOK()) {
        return statusWithCollectionEntry.getStatus();
    }
    ShardCollectionType entry = statusWithCollectionEntry.getValue();

    return RefreshState{entry.getEpoch(),
                        entry.hasRefreshing() ? entry.getRefreshing() : false,
                        entry.hasRefreshSequenceNumber() ? entry.getRefreshSequenceNumber() : 0LL};
}

StatusWith<ShardCollectionType> readShardCollectionsEntry(OperationContext* opCtx,
                                                          const NamespaceString& nss) {
    Query fullQuery(BSON(ShardCollectionType::uuid() << nss.ns()));
    fullQuery.readPref(ReadPreference::SecondaryOnly, BSONArray());
    try {
        DBDirectClient client(opCtx);
        std::unique_ptr<DBClientCursor> cursor =
            client.query(ShardCollectionType::ConfigNS.c_str(), fullQuery, 1);
        if (!cursor) {
            return Status(ErrorCodes::OperationFailed,
                          str::stream() << "Failed to establish a cursor for reading "
                                        << ShardCollectionType::ConfigNS
                                        << " from local storage");
        }

        if (!cursor->more()) {
            // The collection has been dropped.
            return Status(ErrorCodes::NamespaceNotFound,
                          str::stream() << "collection " << nss.ns() << " not found");
        }

        BSONObj document = cursor->nextSafe();
        auto statusWithCollectionEntry = ShardCollectionType::fromBSON(document);
        if (!statusWithCollectionEntry.isOK()) {
            return statusWithCollectionEntry.getStatus();
        }

        return statusWithCollectionEntry.getValue();
    } catch (const DBException& ex) {
        return {ex.toStatus().code(),
                str::stream() << "Failed to read the '" << nss.ns()
                              << "' entry locally from config.collections"
                              << causedBy(ex.toStatus())};
    }
}

Status updateShardCollectionsEntry(OperationContext* opCtx,
                                   const BSONObj& query,
                                   const BSONObj& update,
                                   const BSONObj& inc,
                                   const bool upsert) {
    invariant(query.hasField("_id"));
    if (upsert) {
        // If upserting, this should be an update from the config server that does not have shard
        // refresh information.
        invariant(!update.hasField(ShardCollectionType::refreshing()));
        invariant(!update.hasField(ShardCollectionType::refreshSequenceNumber()));
        invariant(inc.isEmpty());
    }

    // Want to modify the document, not replace it.
    BSONObjBuilder updateBuilder;
    updateBuilder.append("$set", update);
    if (!inc.isEmpty()) {
        updateBuilder.append("$inc", inc);
    }

    std::unique_ptr<BatchedUpdateDocument> updateDoc(new BatchedUpdateDocument());
    updateDoc->setQuery(query);
    updateDoc->setUpdateExpr(updateBuilder.obj());
    updateDoc->setUpsert(upsert);

    std::unique_ptr<BatchedUpdateRequest> updateRequest(new BatchedUpdateRequest());
    updateRequest->addToUpdates(updateDoc.release());

    BatchedCommandRequest request(updateRequest.release());
    request.setNS(NamespaceString(ShardCollectionType::ConfigNS));
    request.setWriteConcern(kLocalWriteConcern.toBSON());
    BSONObj cmdObj = request.toBSON();

    try {
        DBDirectClient client(opCtx);

        rpc::UniqueReply commandResponse = client.runCommandWithMetadata(
            "config", cmdObj.firstElementFieldName(), rpc::makeEmptyMetadata(), cmdObj);
        BSONObj responseReply = commandResponse->getCommandReply().getOwned();

        Status commandStatus = getStatusFromCommandResult(commandResponse->getCommandReply());
        if (!commandStatus.isOK()) {
            return commandStatus;
        }

        return Status::OK();
    } catch (const DBException& ex) {
        return {ex.toStatus().code(),
                str::stream() << "Failed to apply the update '" << request.toString()
                              << "' to config.collections"
                              << causedBy(ex.toStatus())};
    }
}

StatusWith<std::vector<ChunkType>> readShardChunks(OperationContext* opCtx,
                                                   const NamespaceString& nss,
                                                   const ChunkVersion& collectionVersion) {
    // Query to retrieve the chunks.
    QueryAndSort diffQuery = createShardChunkDiffQuery(collectionVersion);
    Query fullQuery(diffQuery.query);
    fullQuery.sort(diffQuery.sort);
    fullQuery.readPref(ReadPreference::SecondaryOnly, BSONArray());

    try {
        DBDirectClient client(opCtx);

        std::string chunkMetadataNs = ChunkType::ShardNSPrefix + nss.ns();
        std::unique_ptr<DBClientCursor> cursor = client.query(chunkMetadataNs, fullQuery, 0LL);

        if (!cursor) {
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Failed to establish a cursor for reading " << chunkMetadataNs
                                  << " from local storage"};
        }

        std::vector<ChunkType> chunks;
        while (cursor->more()) {
            BSONObj document = cursor->nextSafe().getOwned();
            auto statusWithChunk = ChunkType::fromShardBSON(document, collectionVersion.epoch());
            if (!statusWithChunk.isOK()) {
                return {statusWithChunk.getStatus().code(),
                        str::stream() << "Failed to parse chunk '" << document.toString()
                                      << "' due to "
                                      << statusWithChunk.getStatus().reason()};
            }
            chunks.push_back(std::move(statusWithChunk.getValue()));
        }

        return chunks;
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

Status updateShardChunks(OperationContext* opCtx,
                         const NamespaceString& nss,
                         const std::vector<ChunkType>& chunks,
                         const OID& currEpoch) {
    invariant(!chunks.empty());

    NamespaceString chunkMetadataNss(ChunkType::ShardNSPrefix + nss.ns());

    try {
        DBDirectClient client(opCtx);

        /**
         * Here are examples of the operations that can happen on the config server to update
         * the config.chunks collection. 'chunks' only includes the chunks that result from the
         * operations, which can be read from the config server, not any that were removed, so
         * we must delete any chunks that overlap with the new 'chunks'.
         *
         * CollectionVersion = 10.3
         *
         * moveChunk
         * {_id: 3, max: 5, version: 10.1} --> {_id: 3, max: 5, version: 11.0}
         *
         * splitChunk
         * {_id: 3, max: 9, version 10.3} --> {_id: 3, max: 5, version 10.4}
         *                                    {_id: 5, max: 8, version 10.5}
         *                                    {_id: 8, max: 9, version 10.6}
         *
         * mergeChunk
         * {_id: 10, max: 14, version 4.3} --> {_id: 10, max: 22, version 10.4}
         * {_id: 14, max: 19, version 7.1}
         * {_id: 19, max: 22, version 2.0}
         *
         */
        for (auto& chunk : chunks) {
            // Check for a different epoch.
            if (!chunk.getVersion().hasEqualEpoch(currEpoch)) {
                // This means the collection was dropped and recreated. Drop the chunk metadata
                // and return.
                Status status = dropChunksAndDeleteCollectionsEntry(opCtx, nss);
                if (!status.isOK()) {
                    return status;
                }

                return Status{ErrorCodes::ConflictingOperationInProgress,
                              str::stream() << "Invalid chunks found when reloading '"
                                            << nss.toString()
                                            << "'. Previous collection epoch was '"
                                            << currEpoch.toString()
                                            << "', but unexpectedly found a new epoch '"
                                            << chunk.getVersion().epoch().toString()
                                            << "'. Collection was dropped and recreated."};
            }

            // Delete any overlapping chunk ranges. Overlapping chunks will have a min value
            // ("_id") between (chunk.min, chunk.max].
            //
            // query: { "_id" : {"$gte": chunk.min, "$lt": chunk.max}}
            auto deleteDocs(stdx::make_unique<BatchedDeleteDocument>());
            deleteDocs->setQuery(BSON(ChunkType::minShardID << BSON(
                                          "$gte" << chunk.getMin() << "$lt" << chunk.getMax())));
            deleteDocs->setLimit(0);

            auto deleteRequest(stdx::make_unique<BatchedDeleteRequest>());
            deleteRequest->addToDeletes(deleteDocs.release());

            BatchedCommandRequest batchedDeleteRequest(deleteRequest.release());
            batchedDeleteRequest.setNS(chunkMetadataNss);
            const BSONObj deleteCmdObj = batchedDeleteRequest.toBSON();

            rpc::UniqueReply deleteCommandResponse =
                client.runCommandWithMetadata(chunkMetadataNss.db().toString(),
                                              deleteCmdObj.firstElementFieldName(),
                                              rpc::makeEmptyMetadata(),
                                              deleteCmdObj);
            auto deleteStatus =
                getStatusFromCommandResult(deleteCommandResponse->getCommandReply());

            if (!deleteStatus.isOK()) {
                return deleteStatus;
            }

            // Now the document can be expected to cleanly insert without overlap.
            auto insert(stdx::make_unique<BatchedInsertRequest>());
            insert->addToDocuments(chunk.toShardBSON());

            BatchedCommandRequest insertRequest(insert.release());
            insertRequest.setNS(chunkMetadataNss);
            const BSONObj insertCmdObj = insertRequest.toBSON();

            rpc::UniqueReply commandResponse =
                client.runCommandWithMetadata(chunkMetadataNss.db().toString(),
                                              insertCmdObj.firstElementFieldName(),
                                              rpc::makeEmptyMetadata(),
                                              insertCmdObj);
            auto insertStatus = getStatusFromCommandResult(commandResponse->getCommandReply());

            if (!insertStatus.isOK()) {
                return insertStatus;
            }
        }

        return Status::OK();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

Status dropChunksAndDeleteCollectionsEntry(OperationContext* opCtx, const NamespaceString& nss) {
    NamespaceString chunkMetadataNss(ChunkType::ShardNSPrefix + nss.ns());

    try {
        DBDirectClient client(opCtx);

        // Delete the collections collection entry matching 'nss'.
        auto deleteDocs(stdx::make_unique<BatchedDeleteDocument>());
        deleteDocs->setQuery(BSON(ShardCollectionType::uuid << nss.ns()));
        deleteDocs->setLimit(0);

        auto deleteRequest(stdx::make_unique<BatchedDeleteRequest>());
        deleteRequest->addToDeletes(deleteDocs.release());

        BatchedCommandRequest batchedDeleteRequest(deleteRequest.release());
        batchedDeleteRequest.setNS(NamespaceString(ShardCollectionType::ConfigNS));
        const BSONObj deleteCmdObj = batchedDeleteRequest.toBSON();

        rpc::UniqueReply deleteCommandResponse = client.runCommandWithMetadata(
            "config", deleteCmdObj.firstElementFieldName(), rpc::makeEmptyMetadata(), deleteCmdObj);
        auto deleteStatus = getStatusFromCommandResult(deleteCommandResponse->getCommandReply());

        if (!deleteStatus.isOK()) {
            return deleteStatus;
        }

        // Drop the config.chunks.ns collection specified by 'chunkMetadataNss'.
        BSONObj result;
        bool isOK = client.dropCollection(chunkMetadataNss.ns(), kLocalWriteConcern, &result);
        if (!isOK) {
            Status status = getStatusFromCommandResult(result);
            if (!status.isOK() && status.code() != ErrorCodes::NamespaceNotFound) {
                return status;
            }
        }

        return Status::OK();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

}  // namespace shardmetadatautil
}  // namespace mongo
