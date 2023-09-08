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

#pragma once

#include <string>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/keys_collection_document_gen.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_index_catalog_gen.h"
#include "mongo/s/catalog/type_namespace_placement_gen.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/chunk_version.h"
#include "mongo/util/duration.h"
#include "mongo/util/uuid.h"

namespace mongo {

/**
 * A dummy implementation of ShardingCatalogClient for testing purposes.
 */
class ShardingCatalogClientMock : public ShardingCatalogClient {
public:
    ShardingCatalogClientMock();
    ~ShardingCatalogClientMock();

    std::vector<BSONObj> runCatalogAggregation(OperationContext* opCtx,
                                               AggregateCommandRequest& aggRequest,
                                               const repl::ReadConcernArgs& readConcern,
                                               const Milliseconds& maxTimeout) override;

    DatabaseType getDatabase(OperationContext* opCtx,
                             const DatabaseName& db,
                             repl::ReadConcernLevel readConcernLevel) override;

    std::vector<DatabaseType> getAllDBs(OperationContext* opCtx,
                                        repl::ReadConcernLevel readConcern) override;

    CollectionType getCollection(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 repl::ReadConcernLevel readConcernLevel) override;

    CollectionType getCollection(OperationContext* opCtx,
                                 const UUID& uuid,
                                 repl::ReadConcernLevel readConcernLevel) override;

    std::vector<CollectionType> getShardedCollections(OperationContext* opCtx,
                                                      const DatabaseName& db,
                                                      repl::ReadConcernLevel readConcernLevel,
                                                      const BSONObj& sort) override;

    std::vector<CollectionType> getCollections(OperationContext* opCtx,
                                               const DatabaseName& db,
                                               repl::ReadConcernLevel readConcernLevel,
                                               const BSONObj& sort) override;

    std::vector<NamespaceString> getCollectionNamespacesForDb(OperationContext* opCtx,
                                                              const DatabaseName& dbName,
                                                              repl::ReadConcernLevel readConcern,
                                                              const BSONObj& sort) override;

    std::vector<NamespaceString> getShardedCollectionNamespacesForDb(
        OperationContext* opCtx,
        const DatabaseName& dbName,
        repl::ReadConcernLevel readConcern,
        const BSONObj& sort = BSONObj()) override;

    std::vector<NamespaceString> getUnsplittableCollectionNamespacesForDb(
        OperationContext* opCtx,
        const DatabaseName& dbName,
        repl::ReadConcernLevel readConcern,
        const BSONObj& sort = BSONObj()) override;

    StatusWith<std::vector<DatabaseName>> getDatabasesForShard(OperationContext* opCtx,
                                                               const ShardId& shardName) override;

    StatusWith<std::vector<ChunkType>> getChunks(OperationContext* opCtx,
                                                 const BSONObj& filter,
                                                 const BSONObj& sort,
                                                 boost::optional<int> limit,
                                                 repl::OpTime* opTime,
                                                 const OID& epoch,
                                                 const Timestamp& timestamp,
                                                 repl::ReadConcernLevel readConcern,
                                                 const boost::optional<BSONObj>& hint) override;

    std::pair<CollectionType, std::vector<ChunkType>> getCollectionAndChunks(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const ChunkVersion& sinceVersion,
        const repl::ReadConcernArgs& readConcern) override;

    std::pair<CollectionType, std::vector<IndexCatalogType>>
    getCollectionAndShardingIndexCatalogEntries(OperationContext* opCtx,
                                                const NamespaceString& nss,
                                                const repl::ReadConcernArgs& readConcern) override;

    StatusWith<std::vector<TagsType>> getTagsForCollection(OperationContext* opCtx,
                                                           const NamespaceString& nss) override;

    std::vector<NamespaceString> getAllNssThatHaveZonesForDatabase(
        OperationContext* opCtx, const DatabaseName& dbName) override;

    StatusWith<repl::OpTimeWith<std::vector<ShardType>>> getAllShards(
        OperationContext* opCtx, repl::ReadConcernLevel readConcern) override;

    Status runUserManagementWriteCommand(OperationContext* opCtx,
                                         StringData commandName,
                                         const DatabaseName& dbname,
                                         const BSONObj& cmdObj,
                                         BSONObjBuilder* result) override {
        return Status::OK();
    }

    bool runUserManagementReadCommand(OperationContext* opCtx,
                                      const DatabaseName& dbname,
                                      const BSONObj& cmdObj,
                                      BSONObjBuilder* result) override;

    StatusWith<BSONObj> getGlobalSettings(OperationContext* opCtx, StringData key) override;

    StatusWith<VersionType> getConfigVersion(OperationContext* opCtx,
                                             repl::ReadConcernLevel readConcern) override;

    Status insertConfigDocument(OperationContext* opCtx,
                                const NamespaceString& nss,
                                const BSONObj& doc,
                                const WriteConcernOptions& writeConcern) override;

    StatusWith<bool> updateConfigDocument(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const BSONObj& query,
                                          const BSONObj& update,
                                          bool upsert,
                                          const WriteConcernOptions& writeConcern) override;

    StatusWith<bool> updateConfigDocument(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const BSONObj& query,
                                          const BSONObj& update,
                                          bool upsert,
                                          const WriteConcernOptions& writeConcern,
                                          Milliseconds maxTimeMs) override;

    Status removeConfigDocuments(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 const BSONObj& query,
                                 const WriteConcernOptions& writeConcern,
                                 boost::optional<BSONObj> hint) override;

    Status createDatabase(OperationContext* opCtx, StringData dbName, ShardId primaryShard);

    StatusWith<std::vector<KeysCollectionDocument>> getNewInternalKeys(
        OperationContext* opCtx,
        StringData purpose,
        const LogicalTime& newerThanThis,
        repl::ReadConcernLevel readConcernLevel) override;

    StatusWith<std::vector<ExternalKeysCollectionDocument>> getAllExternalKeys(
        OperationContext* opCtx,
        StringData purpose,
        repl::ReadConcernLevel readConcernLevel) override;

    HistoricalPlacement getShardsThatOwnDataForCollAtClusterTime(
        OperationContext* opCtx,
        const NamespaceString& collName,
        const Timestamp& clusterTime) override;

    HistoricalPlacement getShardsThatOwnDataForDbAtClusterTime(
        OperationContext* opCtx,
        const NamespaceString& dbName,
        const Timestamp& clusterTime) override;

    HistoricalPlacement getShardsThatOwnDataAtClusterTime(OperationContext* opCtx,
                                                          const Timestamp& clusterTime) override;

    HistoricalPlacement getHistoricalPlacement(
        OperationContext* opCtx,
        const Timestamp& atClusterTime,
        const boost::optional<NamespaceString>& nss) override;


private:
    StatusWith<repl::OpTimeWith<std::vector<BSONObj>>> _exhaustiveFindOnConfig(
        OperationContext* opCtx,
        const ReadPreferenceSetting& readPref,
        const repl::ReadConcernLevel& readConcern,
        const NamespaceString& nss,
        const BSONObj& query,
        const BSONObj& sort,
        boost::optional<long long> limit,
        const boost::optional<BSONObj>& hint) override;
};

}  // namespace mongo
