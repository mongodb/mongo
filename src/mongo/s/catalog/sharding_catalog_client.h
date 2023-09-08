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
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
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
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_index_catalog.h"
#include "mongo/s/catalog/type_index_catalog_gen.h"
#include "mongo/s/catalog/type_namespace_placement_gen.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/index_version.h"
#include "mongo/s/request_types/placement_history_commands_gen.h"
#include "mongo/util/duration.h"
#include "mongo/util/uuid.h"

namespace mongo {

struct BSONArray;
class BSONArrayBuilder;
class BSONObj;
class BSONObjBuilder;
class ChunkType;

class CollectionType;
class ConnectionString;

class DatabaseType;
class LogicalTime;
class OperationContext;
class ShardKeyPattern;
class TagsType;

class VersionType;

namespace executor {
struct ConnectionPoolStats;
}

/**
 * Abstracts reads of the sharding catalog metadata.
 *
 * All implementations of this interface should go directly to the persistent backing store
 * and should avoid doing any caching of their own. The caching is delegated to a parallel
 * read-only view of the catalog, which is maintained by a higher level code.
 *
 * TODO: For now this also includes some methods that write the sharding catalog metadata.  Those
 * should eventually all be moved to ShardingCatalogManager as catalog manipulation operations
 * move to be run on the config server primary.
 */
class ShardingCatalogClient {
    ShardingCatalogClient(const ShardingCatalogClient&) = delete;
    ShardingCatalogClient& operator=(const ShardingCatalogClient&) = delete;

public:
    // Constant to use for configuration data majority writes
    static const WriteConcernOptions kMajorityWriteConcern;

    // Constant to use for configuration data local writes
    static const WriteConcernOptions kLocalWriteConcern;

    // Identifier for the "initialization metadata" documents of config.placementHistory
    static const NamespaceString kConfigPlacementHistoryInitializationMarker;

    virtual ~ShardingCatalogClient() = default;

    virtual std::vector<BSONObj> runCatalogAggregation(
        OperationContext* opCtx,
        AggregateCommandRequest& aggRequest,
        const repl::ReadConcernArgs& readConcern,
        const Milliseconds& maxTimeout = Shard::kDefaultConfigCommandTimeout) = 0;

    /**
     * Retrieves the metadata for a given database, if it exists.
     *
     * @param dbName name of the database (case sensitive)
     *
     * Returns Status::OK along with the database information and the OpTime of the config server
     * which the database information was based upon. Otherwise, returns an error code indicating
     * the failure. These are some of the known failures:
     *  - NamespaceNotFound - database does not exist
     */
    virtual DatabaseType getDatabase(OperationContext* opCtx,
                                     const DatabaseName& db,
                                     repl::ReadConcernLevel readConcernLevel) = 0;

    /**
     * Retrieves all databases in a cluster.
     *
     * Returns a !OK status if an error occurs.
     */
    virtual std::vector<DatabaseType> getAllDBs(OperationContext* opCtx,
                                                repl::ReadConcernLevel readConcern) = 0;

    /**
     * Retrieves the metadata for a given collection, if it exists.
     *
     * @param nss fully qualified name of the collection (case sensitive)
     *
     * Returns Status::OK along with the collection information and the OpTime of the config server
     * which the collection information was based upon. Otherwise, returns an error code indicating
     * the failure. These are some of the known failures:
     *  - NamespaceNotFound - collection does not exist
     */
    virtual CollectionType getCollection(
        OperationContext* opCtx,
        const NamespaceString& nss,
        repl::ReadConcernLevel readConcernLevel = repl::ReadConcernLevel::kMajorityReadConcern) = 0;

    virtual CollectionType getCollection(
        OperationContext* opCtx,
        const UUID& uuid,
        repl::ReadConcernLevel readConcernLevel = repl::ReadConcernLevel::kMajorityReadConcern) = 0;

    /**
     * Retrieves all collections under a specified database (or in the system) which are sharded. If
     * the dbName parameter is empty, returns all sharded collections.
     *
     * @param sort Fields to use for sorting the results. If empty, no sorting is performed.
     */
    virtual std::vector<CollectionType> getShardedCollections(
        OperationContext* opCtx,
        const DatabaseName& db,
        repl::ReadConcernLevel readConcernLevel = repl::ReadConcernLevel::kMajorityReadConcern,
        const BSONObj& sort = BSONObj()) = 0;

    /**
     * Retrieves all collections under a specified database (or in the system). If the dbName
     * parameter is empty, returns all collections.
     *
     * @param sort Fields to use for sorting the results. If empty, no sorting is performed.
     */
    virtual std::vector<CollectionType> getCollections(
        OperationContext* opCtx,
        const DatabaseName& db,
        repl::ReadConcernLevel readConcernLevel = repl::ReadConcernLevel::kMajorityReadConcern,
        const BSONObj& sort = BSONObj()) = 0;

    /**
     * Returns the set of collections for the specified database, which have been marked as sharded.
     * Goes directly to the config server's metadata, without checking the local cache so it should
     * not be used in frequently called code paths.
     *
     * Throws exception on errors.
     */
    virtual std::vector<NamespaceString> getShardedCollectionNamespacesForDb(
        OperationContext* opCtx,
        const DatabaseName& dbName,
        repl::ReadConcernLevel readConcern,
        const BSONObj& sort = BSONObj()) = 0;

    /**
     * Returns the set of collections tracked for the specified database, regardless of being
     * sharded or not. Goes directly to the config server's metadata, without checking the local
     * cache so it should not be used in frequently called code paths.
     *
     * Throws exception on errors.
     */
    virtual std::vector<NamespaceString> getCollectionNamespacesForDb(
        OperationContext* opCtx,
        const DatabaseName& dbName,
        repl::ReadConcernLevel readConcern,
        const BSONObj& sort = BSONObj()) = 0;

    /**
     * Returns the set of collections for the specified database, which have been marked as
     * unsplittable. Goes directly to the config server's metadata, without checking the local cache
     * so it should not be used in frequently called code paths.
     *
     * Throws exception on errors.
     */
    virtual std::vector<NamespaceString> getUnsplittableCollectionNamespacesForDb(
        OperationContext* opCtx,
        const DatabaseName& dbName,
        repl::ReadConcernLevel readConcern,
        const BSONObj& sort = BSONObj()) = 0;

    /**
     * Retrieves all databases for a shard.
     *
     * Returns a !OK status if an error occurs.
     */
    virtual StatusWith<std::vector<DatabaseName>> getDatabasesForShard(OperationContext* opCtx,
                                                                       const ShardId& shardId) = 0;

    /**
     * Gets the requested number of chunks (of type ChunkType) that satisfy a query.
     *
     * @param filter The query to filter out the results.
     * @param sort Fields to use for sorting the results. Pass empty BSON object for no sort.
     * @param limit The number of chunk entries to return. Pass boost::none for no limit.
     * @param optime an out parameter that will contain the opTime of the config server.
     *      Can be null. Note that chunks can be fetched in multiple batches and each batch
     *      can have a unique opTime. This opTime will be the one from the last batch.
     * @param epoch epoch associated to the collection, needed to build the chunks.
     * @param timestamp timestamp associated to the collection, needed to build the chunks.
     * @param readConcern The readConcern to use while querying for chunks.

     *
     * Returns a vector of ChunkTypes, or a !OK status if an error occurs.
     */
    virtual StatusWith<std::vector<ChunkType>> getChunks(
        OperationContext* opCtx,
        const BSONObj& filter,
        const BSONObj& sort,
        boost::optional<int> limit,
        repl::OpTime* opTime,
        const OID& epoch,
        const Timestamp& timestamp,
        repl::ReadConcernLevel readConcern,
        const boost::optional<BSONObj>& hint = boost::none) = 0;

    /**
     * Retrieves the collection metadata and its chunks metadata. If the collection epoch matches
     * the one specified in sinceVersion, then it only returns chunks with 'lastmod' gte than
     * sinceVersion; otherwise it returns all of its chunks.
     */
    virtual std::pair<CollectionType, std::vector<ChunkType>> getCollectionAndChunks(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const ChunkVersion& sinceVersion,
        const repl::ReadConcernArgs& readConcern) = 0;

    /**
     * Retrieves the collection metadata and its global index metadata. This function will return
     * all of the global idexes for a collection.
     */
    virtual std::pair<CollectionType, std::vector<IndexCatalogType>>
    getCollectionAndShardingIndexCatalogEntries(OperationContext* opCtx,
                                                const NamespaceString& nss,
                                                const repl::ReadConcernArgs& readConcern) = 0;

    /**
     * Retrieves all zones defined for the specified collection. The returned vector is sorted based
     * on the min key of the zones.
     *
     * Returns a !OK status if an error occurs.
     */
    virtual StatusWith<std::vector<TagsType>> getTagsForCollection(OperationContext* opCtx,
                                                                   const NamespaceString& nss) = 0;

    /**
     * Retrieves all namespaces that have zones associated with a database.
     */
    virtual std::vector<NamespaceString> getAllNssThatHaveZonesForDatabase(
        OperationContext* opCtx, const DatabaseName& dbName) = 0;

    /**
     * Retrieves all shards in this sharded cluster.
     * Returns a !OK status if an error occurs.
     */
    virtual StatusWith<repl::OpTimeWith<std::vector<ShardType>>> getAllShards(
        OperationContext* opCtx, repl::ReadConcernLevel readConcern) = 0;

    /**
     * Runs a user management command on the config servers. Do not use for general write command
     * execution.
     *
     * @param commandName: name of command
     * @param dbname: database for which the user management command is invoked
     * @param cmdObj: command obj
     * @param result: contains data returned from config servers
     * Returns true on success.
     */
    virtual Status runUserManagementWriteCommand(OperationContext* opCtx,
                                                 StringData commandName,
                                                 const DatabaseName& dbname,
                                                 const BSONObj& cmdObj,
                                                 BSONObjBuilder* result) = 0;

    /**
     * Runs a user management related read-only command on a config server.
     */
    virtual bool runUserManagementReadCommand(OperationContext* opCtx,
                                              const DatabaseName& dbname,
                                              const BSONObj& cmdObj,
                                              BSONObjBuilder* result) = 0;

    /**
     * Reads global sharding settings from the confing.settings collection. The key parameter is
     * used as the _id of the respective setting document.
     *
     * NOTE: This method should generally not be used directly and instead the respective
     * configuration class should be used (e.g. BalancerConfiguration).
     *
     * Returns ErrorCodes::NoMatchingDocument if no such key exists or the BSON content of the
     * setting otherwise.
     */
    virtual StatusWith<BSONObj> getGlobalSettings(OperationContext* opCtx, StringData key) = 0;

    /**
     * Returns the contents of the config.version document - containing the current cluster schema
     * version as well as the clusterID.
     */
    virtual StatusWith<VersionType> getConfigVersion(OperationContext* opCtx,
                                                     repl::ReadConcernLevel readConcern) = 0;

    /**
     * Returns internal keys for the given purpose and have an expiresAt value greater than
     * newerThanThis.
     */
    virtual StatusWith<std::vector<KeysCollectionDocument>> getNewInternalKeys(
        OperationContext* opCtx,
        StringData purpose,
        const LogicalTime& newerThanThis,
        repl::ReadConcernLevel readConcernLevel) = 0;

    /**
     * Returns all external (i.e. validation-only) keys for the given purpose.
     */
    virtual StatusWith<std::vector<ExternalKeysCollectionDocument>> getAllExternalKeys(
        OperationContext* opCtx, StringData purpose, repl::ReadConcernLevel readConcernLevel) = 0;

    /**
     * Directly inserts a document in the specified namespace on the config server. The document
     * must have an _id index. Must only be used for insertions in the 'config' database.
     *
     * NOTE: Should not be used in new code outside the ShardingCatalogManager.
     */
    virtual Status insertConfigDocument(OperationContext* opCtx,
                                        const NamespaceString& nss,
                                        const BSONObj& doc,
                                        const WriteConcernOptions& writeConcern) = 0;

    /**
     * Updates a single document in the specified namespace on the config server. Must only be used
     * for updates to the 'config' database.
     *
     * This method retries the operation on NotPrimary or network errors, so it should only be used
     * with modifications which are idempotent.
     *
     * Returns non-OK status if the command failed to run for some reason. If the command was
     * successful, returns true if a document was actually modified (that is, it did not exist and
     * was upserted or it existed and any of the fields changed) and false otherwise (basically
     * returns whether the update command's response update.n value is > 0).
     *
     * NOTE: Should not be used in new code outside the ShardingCatalogManager.
     */
    virtual StatusWith<bool> updateConfigDocument(OperationContext* opCtx,
                                                  const NamespaceString& nss,
                                                  const BSONObj& query,
                                                  const BSONObj& update,
                                                  bool upsert,
                                                  const WriteConcernOptions& writeConcern) = 0;

    /**
     * Overload version of updateConfigDocument with the extra parameter 'maxTimeMs' for setting a
     * custom timeout duration. Setting 'maxTimeMs' to Milliseconds::max() will entirely remove
     * maxTimeMs from the command object sent over the wire.
     */
    virtual StatusWith<bool> updateConfigDocument(OperationContext* opCtx,
                                                  const NamespaceString& nss,
                                                  const BSONObj& query,
                                                  const BSONObj& update,
                                                  bool upsert,
                                                  const WriteConcernOptions& writeConcern,
                                                  Milliseconds maxTimeMs) = 0;

    /**
     * Removes documents matching a particular query predicate from the specified namespace on the
     * config server. Must only be used for deletions from the 'config' database.
     *
     * NOTE: Should not be used in new code outside the ShardingCatalogManager.
     */
    virtual Status removeConfigDocuments(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         const BSONObj& query,
                                         const WriteConcernOptions& writeConcern,
                                         boost::optional<BSONObj> hint = boost::none) = 0;

    /**
     * Returns shard-placement information for collection named 'collName' at the requested point in
     * time 'clusterTime'.
     * - When an exact response may be computed, this will be composed by the shards hosting data of
     *   collName + the primary shard of the parent db.
     * - Otherwise, an approximated response is generated based on a past snapshot of config.shards.
     * References to shards that aren't currently part of the cluster may be included in the
     * response.
     */
    virtual HistoricalPlacement getShardsThatOwnDataForCollAtClusterTime(
        OperationContext* opCtx, const NamespaceString& collName, const Timestamp& clusterTime) = 0;

    /**
     * Returns shard-placement information for database named 'dbName' at the requested point in
     * time 'clusterTime'.
     * When no exact response may be computed, an approximated one is generated based on a past
     * snapshot of config.shards.
     * References to shards that aren't currently part of the cluster may be included in the
     * response.
     */
    virtual HistoricalPlacement getShardsThatOwnDataForDbAtClusterTime(
        OperationContext* opCtx, const NamespaceString& dbName, const Timestamp& clusterTime) = 0;

    /**
     * Returns shard-placement information for the whole cluster at the requested point in time
     * 'clusterTime'.
     * When no exact response may be computed, an approximated one is generated based on a past
     * snapshot of config.shards.
     * References to shards that aren't currently part of the cluster may be included in the
     * response.
     */
    virtual HistoricalPlacement getShardsThatOwnDataAtClusterTime(OperationContext* opCtx,
                                                                  const Timestamp& clusterTime) = 0;

    /**
     * Queries config.placementHistory to retrieve placement metadata on the requested namespace at
     * a specific point in time. When no namespace is specified, placement metadata on the whole
     * cluster will be returned. This function is meant to be exclusively invoked by config server
     * nodes.
     *
     * TODO (SERVER-73029): convert to private method of ShardingCatalogClientImpl
     */
    virtual HistoricalPlacement getHistoricalPlacement(
        OperationContext* opCtx,
        const Timestamp& atClusterTime,
        const boost::optional<NamespaceString>& nss) = 0;

protected:
    ShardingCatalogClient() = default;

private:
    virtual StatusWith<repl::OpTimeWith<std::vector<BSONObj>>> _exhaustiveFindOnConfig(
        OperationContext* opCtx,
        const ReadPreferenceSetting& readPref,
        const repl::ReadConcernLevel& readConcern,
        const NamespaceString& nss,
        const BSONObj& query,
        const BSONObj& sort,
        boost::optional<long long> limit,
        const boost::optional<BSONObj>& hint = boost::none) = 0;
};

}  // namespace mongo
