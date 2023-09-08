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

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/connection_string.h"
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
#include "mongo/platform/mutex.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_index_catalog_gen.h"
#include "mongo/s/catalog/type_namespace_placement_gen.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/request_types/placement_history_commands_gen.h"
#include "mongo/util/duration.h"
#include "mongo/util/uuid.h"

namespace mongo {

class NamespaceString;
struct ReadPreferenceSetting;
class VersionType;

namespace executor {
class TaskExecutor;
}  // namespace executor

/**
 * Implements the catalog client for reading from replica set config servers.
 */
class ShardingCatalogClientImpl final : public ShardingCatalogClient {

public:
    ShardingCatalogClientImpl(std::shared_ptr<Shard> overrideConfigShard);
    virtual ~ShardingCatalogClientImpl();

    /*
     * Updates (or if "upsert" is true, creates) catalog data for the sharded collection "collNs" by
     * writing a document to the "config.collections" collection with the catalog information
     * described by "coll."
     */
    Status updateShardingCatalogEntryForCollection(OperationContext* opCtx,
                                                   const NamespaceString& nss,
                                                   const CollectionType& coll,
                                                   bool upsert);

    std::vector<BSONObj> runCatalogAggregation(
        OperationContext* opCtx,
        AggregateCommandRequest& aggRequest,
        const repl::ReadConcernArgs& readConcern,
        const Milliseconds& maxTimeout = Shard::kDefaultConfigCommandTimeout) override;

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

    std::vector<NamespaceString> getShardedCollectionNamespacesForDb(
        OperationContext* opCtx,
        const DatabaseName& dbName,
        repl::ReadConcernLevel readConcern,
        const BSONObj& sort = BSONObj()) override;

    std::vector<NamespaceString> getCollectionNamespacesForDb(
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

    StatusWith<std::vector<ChunkType>> getChunks(
        OperationContext* opCtx,
        const BSONObj& query,
        const BSONObj& sort,
        boost::optional<int> limit,
        repl::OpTime* opTime,
        const OID& epoch,
        const Timestamp& timestamp,
        repl::ReadConcernLevel readConcern,
        const boost::optional<BSONObj>& hint = boost::none) override;

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
                                         BSONObjBuilder* result) override;

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
                                 boost::optional<BSONObj> hint = boost::none) override;

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
    /**
     * Updates a single document (if useMultiUpdate is false) or multiple documents (if
     * useMultiUpdate is true) in the specified namespace on the config server. Must only be used
     * for updates to the 'config' database.
     *
     * This method retries the operation on NotPrimary or network errors, so it should only be used
     * with modifications which are idempotent.
     *
     * Returns non-OK status if the command failed to run for some reason. If the command was
     * successful, returns true if a document was actually modified (that is, it did not exist and
     * was upserted or it existed and any of the fields changed) and false otherwise (basically
     * returns whether the update command's response update.n value is > 0).
     */
    StatusWith<bool> _updateConfigDocument(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           const BSONObj& query,
                                           const BSONObj& update,
                                           bool upsert,
                                           const WriteConcernOptions& writeConcern,
                                           Milliseconds maxTimeMs);

    StatusWith<repl::OpTimeWith<std::vector<BSONObj>>> _exhaustiveFindOnConfig(
        OperationContext* opCtx,
        const ReadPreferenceSetting& readPref,
        const repl::ReadConcernLevel& readConcern,
        const NamespaceString& nss,
        const BSONObj& query,
        const BSONObj& sort,
        boost::optional<long long> limit,
        const boost::optional<BSONObj>& hint = boost::none) override;

    /**
     * Queries the config servers for the database metadata for the given database, using the
     * given read preference.  Returns NamespaceNotFound if no database metadata is found.
     */
    StatusWith<repl::OpTimeWith<DatabaseType>> _fetchDatabaseMetadata(
        OperationContext* opCtx,
        const DatabaseName& dbName,
        const ReadPreferenceSetting& readPref,
        repl::ReadConcernLevel readConcernLevel);

    /**
     * Queries the config server to retrieve placement data based on the Request object.
     * TODO (SERVER-73029): Remove the method - and replace its invocations with
     * runPlacementHistoryAggregation()
     */
    HistoricalPlacement _fetchPlacementMetadata(OperationContext* opCtx,
                                                ConfigsvrGetHistoricalPlacement&& request);


    /**
     * Returns the Shard type that should be used to access the config server. Unless an instance
     * was provided at construction, which may be done e.g. to force using local operations, falls
     * back to using the config shard from the ShardRegistry.
     */
    std::shared_ptr<Shard> _getConfigShard(OperationContext* opCtx);

    // If set, this is used as the config shard by all methods. Be careful to only use an instance
    // that is always valid, like a ShardLocal.
    std::shared_ptr<Shard> _overrideConfigShard;
};

}  // namespace mongo
