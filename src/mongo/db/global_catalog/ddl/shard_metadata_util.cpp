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

#include "mongo/db/global_catalog/ddl/shard_metadata_util.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_shard_collection.h"
#include "mongo/db/global_catalog/type_shard_collection_gen.h"
#include "mongo/db/global_catalog/type_shard_database.h"
#include "mongo/db/global_catalog/type_shard_database_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/write_ops/write_ops_exec.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/sharding_environment/sharding_runtime_d_params_gen.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace shardmetadatautil {
namespace {

const WriteConcernOptions kLocalWriteConcern(1,
                                             WriteConcernOptions::SyncMode::UNSET,
                                             Milliseconds(0));

/**
 * Processes a command result for errors, including write concern errors.
 */
Status getStatusFromWriteCommandResponse(const BSONObj& commandResult) {
    BatchedCommandResponse batchResponse;
    std::string errmsg;
    if (!batchResponse.parseBSON(commandResult, &errmsg)) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "Failed to parse write response: " << errmsg);
    }

    return batchResponse.toStatus();
}

Status setPersistedRefreshFlags(OperationContext* opCtx, const NamespaceString& nss) {
    return updateShardCollectionsEntry(
        opCtx,
        BSON(ShardCollectionType::kNssFieldName
             << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())),
        BSON("$set" << BSON(ShardCollectionType::kRefreshingFieldName << true)),
        false /*upsert*/);
}

}  // namespace

QueryAndSort createShardChunkDiffQuery(const ChunkVersion& collectionPlacementVersion) {
    return {BSON(ChunkType::lastmod()
                 << BSON("$gte" << Timestamp(collectionPlacementVersion.toLong()))),
            BSON(ChunkType::lastmod() << 1)};
}

bool RefreshState::operator==(const RefreshState& other) const {
    return generation.isSameCollection(other.generation) && (refreshing == other.refreshing) &&
        (lastRefreshedCollectionPlacementVersion == other.lastRefreshedCollectionPlacementVersion);
}

std::string RefreshState::toString() const {
    return str::stream() << "generation: " << generation.toString()
                         << ", refreshing: " << (refreshing ? "true" : "false")
                         << ", lastRefreshedCollectionPlacementVersion: "
                         << lastRefreshedCollectionPlacementVersion.toString();
}

Status unsetPersistedRefreshFlags(OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  const ChunkVersion& refreshedVersion) {
    // Set 'refreshing' to false and update the last refreshed collection placement version.
    BSONObjBuilder updateBuilder;
    updateBuilder.append(ShardCollectionType::kRefreshingFieldName, false);
    updateBuilder.appendTimestamp(
        ShardCollectionType::kLastRefreshedCollectionMajorMinorVersionFieldName,
        refreshedVersion.toLong());

    return updateShardCollectionsEntry(
        opCtx,
        BSON(ShardCollectionType::kNssFieldName
             << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())),
        BSON("$set" << updateBuilder.obj()),
        false /*upsert*/);
}

StatusWith<RefreshState> getPersistedRefreshFlags(OperationContext* opCtx,
                                                  const NamespaceString& nss) {
    auto statusWithCollectionEntry = readShardCollectionsEntry(opCtx, nss);
    if (!statusWithCollectionEntry.isOK()) {
        return statusWithCollectionEntry.getStatus();
    }
    ShardCollectionType entry = statusWithCollectionEntry.getValue();

    // Ensure the results have not been incorrectly set somehow.
    if (entry.getRefreshing()) {
        // If 'refreshing' is present and false, a refresh must have occurred (otherwise the field
        // would never have been added to the document) and there should always be a refresh
        // version.
        invariant(*entry.getRefreshing() ? true
                                         : !!entry.getLastRefreshedCollectionPlacementVersion());
    } else {
        // If 'refreshing' is not present, no refresh version should exist.
        invariant(!entry.getLastRefreshedCollectionPlacementVersion());
    }

    return RefreshState{CollectionGeneration(entry.getEpoch(), entry.getTimestamp()),
                        // If the refreshing field has not yet been added, this means that the first
                        // refresh has started, but no chunks have ever yet been applied, around
                        // which these flags are set. So default to refreshing true because the
                        // chunk metadata is being updated and is not yet ready to be read.
                        entry.getRefreshing() ? *entry.getRefreshing() : true,
                        entry.getLastRefreshedCollectionPlacementVersion()
                            ? *entry.getLastRefreshedCollectionPlacementVersion()
                            : ChunkVersion({entry.getEpoch(), entry.getTimestamp()}, {0, 0})};
}

StatusWith<ShardCollectionType> readShardCollectionsEntry(OperationContext* opCtx,
                                                          const NamespaceString& nss) {
    BSONObj document;
    try {
        DBDirectClient client(opCtx);
        FindCommandRequest findRequest{NamespaceString::kShardConfigCollectionsNamespace};
        findRequest.setFilter(
            BSON(ShardCollectionType::kNssFieldName
                 << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())));
        findRequest.setLimit(1);
        std::unique_ptr<DBClientCursor> cursor = client.find(std::move(findRequest));
        if (!cursor) {
            return Status(ErrorCodes::OperationFailed,
                          str::stream() << "Failed to establish a cursor for reading "
                                        << NamespaceString::kShardConfigCollectionsNamespace
                                               .toStringForErrorMsg()
                                        << " from local storage");
        }

        if (!cursor->more()) {
            // The collection has been dropped.
            return Status(ErrorCodes::NamespaceNotFound,
                          str::stream()
                              << "collection " << nss.toStringForErrorMsg() << " not found");
        }

        document = cursor->nextSafe();
        return ShardCollectionType(document);
    } catch (const DBException& ex) {
        const auto docDetails = document.isEmpty() ? "" : ": " + document.toString();
        return ex.toStatus(str::stream()
                           << "Failed to read the '" << nss.toStringForErrorMsg()
                           << "' entry locally from config.collections" << docDetails);
    }
}

StatusWith<ShardDatabaseType> readShardDatabasesEntry(OperationContext* opCtx,
                                                      const DatabaseName& dbName) {
    try {
        DBDirectClient client(opCtx);
        FindCommandRequest findRequest{NamespaceString::kConfigCacheDatabasesNamespace};
        findRequest.setFilter(
            BSON(ShardDatabaseType::kDbNameFieldName
                 << DatabaseNameUtil::serialize(dbName, findRequest.getSerializationContext())));
        findRequest.setLimit(1);
        std::unique_ptr<DBClientCursor> cursor = client.find(std::move(findRequest));
        if (!cursor) {
            return Status(ErrorCodes::OperationFailed,
                          str::stream() << "Failed to establish a cursor for reading "
                                        << NamespaceString::kConfigCacheDatabasesNamespace
                                               .toStringForErrorMsg()
                                        << " from local storage");
        }

        if (!cursor->more()) {
            // The database has been dropped.
            return Status(ErrorCodes::NamespaceNotFound,
                          str::stream()
                              << "database " << dbName.toStringForErrorMsg() << " not found");
        }

        BSONObj document = cursor->nextSafe();
        return ShardDatabaseType(document);
    } catch (const DBException& ex) {
        return ex.toStatus(
            str::stream() << "Failed to read the '" << dbName.toStringForErrorMsg()
                          << "' entry locally from "
                          << NamespaceString::kConfigCacheDatabasesNamespace.toStringForErrorMsg());
    }
}

Status updateShardCollectionsEntry(OperationContext* opCtx,
                                   const BSONObj& query,
                                   const BSONObj& update,
                                   const bool upsert) {
    invariant(query.hasField("_id"));
    if (upsert) {
        // If upserting, this should be an update from the config server that does not have shard
        // refresh / migration inc signal information.
        invariant(!update.hasField(
            ShardCollectionType::kLastRefreshedCollectionMajorMinorVersionFieldName));
    }

    try {
        DBDirectClient client(opCtx);
        auto commandResponse = client.runCommand([&] {
            write_ops::UpdateCommandRequest updateOp(
                NamespaceString::kShardConfigCollectionsNamespace);
            updateOp.setUpdates({[&] {
                write_ops::UpdateOpEntry entry;
                entry.setQ(query);
                entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(update));
                entry.setUpsert(upsert);
                return entry;
            }()});
            return updateOp.serialize();
        }());
        uassertStatusOK(getStatusFromWriteCommandResponse(commandResponse->getCommandReply()));

        return Status::OK();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

Status updateShardDatabasesEntry(OperationContext* opCtx,
                                 const BSONObj& query,
                                 const BSONObj& update,
                                 const BSONObj& inc,
                                 const bool upsert) {
    invariant(query.hasField("_id"));
    if (upsert) {
        // If upserting, this should be an update from the config server that does not have shard
        // migration inc signal information.
        invariant(inc.isEmpty());
    }

    try {
        DBDirectClient client(opCtx);

        BSONObjBuilder builder;
        if (!update.isEmpty()) {
            // Want to modify the document if it already exists, not replace it.
            builder.append("$set", update);
        }
        if (!inc.isEmpty()) {
            builder.append("$inc", inc);
        }

        auto commandResponse = client.runCommand([&] {
            write_ops::UpdateCommandRequest updateOp(
                NamespaceString::kConfigCacheDatabasesNamespace);
            updateOp.setUpdates({[&] {
                write_ops::UpdateOpEntry entry;
                entry.setQ(query);
                entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(builder.obj()));
                entry.setUpsert(upsert);
                return entry;
            }()});
            return updateOp.serialize();
        }());
        uassertStatusOK(getStatusFromWriteCommandResponse(commandResponse->getCommandReply()));

        return Status::OK();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

StatusWith<std::vector<ChunkType>> readShardChunks(OperationContext* opCtx,
                                                   const NamespaceString& nss,
                                                   const BSONObj& query,
                                                   const BSONObj& sort,
                                                   boost::optional<long long> limit,
                                                   const OID& epoch,
                                                   const Timestamp& timestamp) {
    const auto sc = SerializationContext::stateDefault();
    const auto chunksNss = NamespaceStringUtil::deserialize(
        nss.tenantId(), ChunkType::ShardNSPrefix + NamespaceStringUtil::serialize(nss, sc), sc);

    try {
        DBDirectClient client(opCtx);

        FindCommandRequest findRequest{chunksNss};
        findRequest.setFilter(query);
        findRequest.setSort(sort);
        if (limit) {
            findRequest.setLimit(*limit);
        }
        std::unique_ptr<DBClientCursor> cursor = client.find(std::move(findRequest));
        uassert(ErrorCodes::OperationFailed,
                str::stream() << "Failed to establish a cursor for reading "
                              << chunksNss.toStringForErrorMsg() << " from local storage",
                cursor);

        std::vector<ChunkType> chunks;
        while (cursor->more()) {
            BSONObj document = cursor->nextSafe().getOwned();
            auto statusWithChunk = ChunkType::parseFromShardBSON(document, epoch, timestamp);
            if (!statusWithChunk.isOK()) {
                return statusWithChunk.getStatus().withContext(
                    str::stream() << "Failed to parse chunk '" << document.toString() << "'");
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
    const auto sc = SerializationContext::stateDefault();
    const auto chunksNss = NamespaceStringUtil::deserialize(
        nss.tenantId(), ChunkType::ShardNSPrefix + NamespaceStringUtil::serialize(nss, sc), sc);
    try {
        DBDirectClient client(opCtx);

        // This may be the first update, so the first opportunity to create an index.
        // If the index already exists, this is a no-op.
        client.createIndex(chunksNss, BSON(ChunkType::lastmod() << 1));
        /**
         * Here are examples of the operations that can happen on the config server to update
         * the config.cache.chunks collection. 'chunks' only includes the chunks that result from
         * the operations, which can be read from the config server, not any that were removed, so
         * we must delete any chunks that overlap with the new 'chunks'.
         *
         * collectionPlacementVersion = 10.3
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

        auto chunkIt = chunks.cbegin();
        while (chunkIt != chunks.cend()) {
            const auto currBatchSize =
                std::min(static_cast<ptrdiff_t>(persistedChunkCacheUpdateMaxBatchSize.load()),
                         std::distance(chunkIt, chunks.end()));

            {
                // Delete any overlapping chunk ranges. Overlapping chunks will have a min value
                // ("_id") between (chunk.min, chunk.max].
                //
                // query: { "_id" : {"$gte": chunk.min, "$lt": chunk.max}}
                std::vector<write_ops::DeleteOpEntry> deletes;
                deletes.reserve(currBatchSize);
                for (auto it = chunkIt; it < chunkIt + currBatchSize; it++) {
                    const auto& chunk = *it;
                    invariant(chunk.getVersion().epoch() == currEpoch);
                    auto query = BSON(ChunkType::minShardID
                                      << BSON("$gte" << chunk.getMin() << "$lt" << chunk.getMax()));
                    deletes.emplace_back(std::move(query), true /* multi */);
                }
                write_ops::DeleteCommandRequest deleteReq{chunksNss, std::move(deletes)};
                deleteReq.getWriteCommandRequestBase().setOrdered(true);
                deleteReq.getWriteCommandRequestBase().setBypassDocumentValidation(true);
                auto deleteResp =
                    write_ops_exec::performDeletes(opCtx, deleteReq, /*preConditions=*/boost::none);
                // Since the writes are ordered, it's ok to check just the last writeOp result.
                uassertStatusOK(deleteResp.results.back());
            }

            // Now the documents can be expected to cleanly insert without overlap
            {
                std::vector<BSONObj> docs;
                docs.reserve(currBatchSize);
                for (auto it = chunkIt; it < chunkIt + currBatchSize; it++) {
                    const auto& chunk = *it;
                    docs.emplace_back(chunk.toShardBSON());
                }
                write_ops::InsertCommandRequest insertReq{chunksNss, std::move(docs)};
                insertReq.getWriteCommandRequestBase().setOrdered(true);
                insertReq.getWriteCommandRequestBase().setBypassDocumentValidation(true);
                auto insertResp = write_ops_exec::performInserts(opCtx, insertReq);
                // Since the writes are ordered, it's ok to check just the last writeOp result.
                uassertStatusOK(insertResp.results.back());
            }

            std::advance(chunkIt, currBatchSize);
        }

        return Status::OK();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

Status dropChunksAndDeleteCollectionsEntry(OperationContext* opCtx, const NamespaceString& nss) {
    try {

        // Delete the collection entry from 'config.cache.collections'.
        DBDirectClient client(opCtx);
        auto deleteCommandResponse = client.runCommand([&] {
            write_ops::DeleteCommandRequest deleteOp(
                NamespaceString::kShardConfigCollectionsNamespace);
            deleteOp.setDeletes({[&] {
                write_ops::DeleteOpEntry entry;
                entry.setQ(BSON(
                    ShardCollectionType::kNssFieldName
                    << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())));
                entry.setMulti(true);
                return entry;
            }()});
            return deleteOp.serialize();
        }());
        uassertStatusOK(
            getStatusFromWriteCommandResponse(deleteCommandResponse->getCommandReply()));

        // Drop the 'config.cache.chunks.<ns>' collection.
        dropChunks(opCtx, nss);
    } catch (const DBException& ex) {
        LOGV2_ERROR(5966301,
                    "Failed to drop persisted chunk metadata and collection entry",
                    logAttrs(nss),
                    "error"_attr = redact(ex.toStatus()));

        return ex.toStatus();
    }

    LOGV2_DEBUG(5966302, 1, "Dropped persisted chunk metadata and collection entry", logAttrs(nss));

    return Status::OK();
}

void dropChunks(OperationContext* opCtx, const NamespaceString& nss) {
    DBDirectClient client(opCtx);

    // Drop the 'config.cache.chunks.<ns>' collection.
    BSONObj result;
    const auto sc = SerializationContext::stateDefault();
    if (!client.dropCollection(
            NamespaceStringUtil::deserialize(boost::none,
                                             ChunkType::ShardNSPrefix +
                                                 NamespaceStringUtil::serialize(nss, sc),
                                             sc),
            kLocalWriteConcern,
            &result)) {
        auto status = getStatusFromCommandResult(result);
        if (status != ErrorCodes::NamespaceNotFound) {
            uassertStatusOK(status);
        }
    }

    LOGV2_DEBUG(22091, 1, "Dropped persisted chunk metadata", logAttrs(nss));
}

Status deleteDatabasesEntry(OperationContext* opCtx, const DatabaseName& dbName) {
    try {
        DBDirectClient client(opCtx);
        auto deleteCommandResponse = client.runCommand([&] {
            write_ops::DeleteCommandRequest deleteOp(
                NamespaceString::kConfigCacheDatabasesNamespace);
            deleteOp.setDeletes({[&] {
                write_ops::DeleteOpEntry entry;
                entry.setQ(BSON(ShardDatabaseType::kDbNameFieldName << DatabaseNameUtil::serialize(
                                    dbName, SerializationContext::stateDefault())));
                entry.setMulti(false);
                return entry;
            }()});
            return deleteOp.serialize();
        }());
        uassertStatusOK(
            getStatusFromWriteCommandResponse(deleteCommandResponse->getCommandReply()));

        LOGV2_DEBUG(22092, 1, "Successfully cleared persisted metadata for db", "db"_attr = dbName);
        return Status::OK();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

}  // namespace shardmetadatautil
}  // namespace mongo
