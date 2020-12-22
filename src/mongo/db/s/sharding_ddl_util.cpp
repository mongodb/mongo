/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/sharding_ddl_util.h"

#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/grid.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"

namespace mongo {
namespace {

/** Returns batch of delete operations to be attached to a transaction */
BatchedCommandRequest buildDeleteOp(const NamespaceString& nss,
                                    const BSONObj& query,
                                    bool multiDelete) {
    BatchedCommandRequest request([&] {
        write_ops::Delete deleteOp(nss);
        deleteOp.setDeletes({[&] {
            write_ops::DeleteOpEntry entry;
            entry.setQ(query);
            entry.setMulti(multiDelete);
            return entry;
        }()});
        return deleteOp;
    }());

    return request;
}

/** Returns batch of insert operations to be attached to a transaction */
BatchedCommandRequest buildInsertOp(const NamespaceString& nss, const std::vector<BSONObj> docs) {
    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setDocuments(docs);
        return insertOp;
    }());

    return request;
}

/** Returns batch of update operations to be attached to a transaction */
BatchedCommandRequest buildUpdateOp(const NamespaceString& nss,
                                    const BSONObj& query,
                                    const BSONObj& update,
                                    bool upsert,
                                    bool multi) {
    BatchedCommandRequest request([&] {
        write_ops::Update updateOp(nss);
        updateOp.setUpdates({[&] {
            write_ops::UpdateOpEntry entry;
            entry.setQ(query);
            entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(update));
            entry.setUpsert(upsert);
            entry.setMulti(multi);
            return entry;
        }()});
        return updateOp;
    }());

    return request;
}

}  // namespace

namespace sharding_ddl_util {

OID shardedRenameMetadata(OperationContext* opCtx,
                          const NamespaceString& fromNss,
                          const NamespaceString& toNss) {
    auto newEpoch = OID::gen();

    auto updateCollectionAndChunksFn = [&](OperationContext* opCtx, TxnNumber txnNumber) {
        auto collType = Grid::get(opCtx)->catalogClient()->getCollection(opCtx, fromNss);
        const auto oldEpoch = collType.getEpoch();
        const auto shardingCatalogManager = ShardingCatalogManager::get(opCtx);

        // Delete the FROM collection entry
        shardingCatalogManager->writeToConfigDocumentInTxn(
            opCtx,
            ChunkType::ConfigNS,
            buildDeleteOp(CollectionType::ConfigNS,
                          BSON(CollectionType::kNssFieldName << fromNss.ns()),
                          false /* multi */),
            txnNumber);

        // Insert the TO collection entry
        collType.setNss(toNss);
        collType.setEpoch(newEpoch);
        shardingCatalogManager->writeToConfigDocumentInTxn(
            opCtx,
            ChunkType::ConfigNS,
            buildInsertOp(CollectionType::ConfigNS, {collType.toBSON()}),
            txnNumber);

        // Update all config.chunks entries for the given collection by setting new epoch and nss
        shardingCatalogManager->writeToConfigDocumentInTxn(
            opCtx,
            ChunkType::ConfigNS,
            buildUpdateOp(ChunkType::ConfigNS,
                          BSON(ChunkType::epoch(oldEpoch)),
                          // TODO SERVER-53105 don't set ns field
                          BSON("$set" << BSON(ChunkType::epoch(newEpoch)) << "$set"
                                      << BSON(ChunkType::ns(toNss.ns()))),
                          false, /* upsert */
                          true   /* useMultiUpdate */
                          ),
            txnNumber);
    };

    try {
        ShardingCatalogManager::get(opCtx)->withTransaction(
            opCtx, CollectionType::ConfigNS, updateCollectionAndChunksFn);
    } catch (const DBException& ex) {
        LOGV2_ERROR(5338400,
                    "Failed to rename collection metadata in transaction",
                    "fromNamespace"_attr = fromNss,
                    "toNamespace"_attr = toNss,
                    "reason"_attr = ex);
        throw;
    }

    return newEpoch;
}

}  // namespace sharding_ddl_util
}  // namespace mongo
