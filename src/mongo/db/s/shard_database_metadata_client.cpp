/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/s/shard_database_metadata_client.h"

#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/grid.h"

namespace mongo {

namespace {

template <typename OpType>
void executeMetadataOp(OperationContext* opCtx, const ShardId& shardId, OpType op) {
    auto shard = uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId));
    auto response = shard->runBatchWriteCommand(opCtx,
                                                Milliseconds::max(),
                                                BatchedCommandRequest{op},
                                                ShardingCatalogClient::kMajorityWriteConcern,
                                                Shard::RetryPolicy::kIdempotent);
    uassertStatusOK(response.toStatus());
}

write_ops::UpdateCommandRequest buildUpdateOp(const std::string& dbNameStr,
                                              const DatabaseType& db) {
    const auto query = BSON(DatabaseType::kDbNameFieldName << dbNameStr);

    BSONObjBuilder updateBuilder;
    updateBuilder.append(DatabaseType::kPrimaryFieldName, db.getPrimary());
    updateBuilder.append(DatabaseType::kVersionFieldName, db.getVersion().toBSON());

    write_ops::UpdateCommandRequest updateOp(NamespaceString::kConfigShardDatabasesNamespace);
    updateOp.setUpdates({[&]() {
        write_ops::UpdateOpEntry entry;
        entry.setQ(query);
        entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(
            BSON("$set" << updateBuilder.obj())));
        entry.setUpsert(true);
        return entry;
    }()});

    return updateOp;
}

write_ops::DeleteCommandRequest buildDeleteOp(const std::string& dbNameStr) {
    const auto query = BSON(DatabaseType::kDbNameFieldName << dbNameStr);

    write_ops::DeleteCommandRequest deleteOp(NamespaceString::kConfigShardDatabasesNamespace);
    deleteOp.setDeletes({[&]() {
        write_ops::DeleteOpEntry entry;
        entry.setQ(query);
        entry.setMulti(false);
        return entry;
    }()});

    return deleteOp;
}

}  // namespace

void ShardDatabaseMetadataClient::insert(const DatabaseType& db) {
    const auto dbNameStr =
        DatabaseNameUtil::serialize(db.getDbName(), SerializationContext::stateDefault());

    auto updateOp = buildUpdateOp(dbNameStr, db);

    executeMetadataOp(_opCtx, _shardId, std::move(updateOp));
}

void ShardDatabaseMetadataClient::remove(const DatabaseName& dbName) {
    const auto dbNameStr =
        DatabaseNameUtil::serialize(dbName, SerializationContext::stateDefault());

    auto deleteOp = buildDeleteOp(dbNameStr);

    executeMetadataOp(_opCtx, _shardId, std::move(deleteOp));
}

}  // namespace mongo
