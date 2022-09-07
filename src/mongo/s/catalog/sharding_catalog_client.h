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

#include <boost/optional.hpp>
#include <string>
#include <vector>

#include "mongo/client/read_preference.h"
#include "mongo/db/keys_collection_document_gen.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/catalog/type_index_catalog_gen.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/index_version.h"

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

    virtual ~ShardingCatalogClient() = default;

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
                                     StringData db,
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
     * Retrieves all collections under a specified database (or in the system). If the dbName
     * parameter is empty, returns all collections.
     */
    virtual std::vector<CollectionType> getCollections(
        OperationContext* opCtx,
        StringData db,
        repl::ReadConcernLevel readConcernLevel = repl::ReadConcernLevel::kMajorityReadConcern) = 0;

    /**
     * Returns the set of collections for the specified database, which have been marked as sharded.
     * Goes directly to the config server's metadata, without checking the local cache so it should
     * not be used in frequently called code paths.
     *
     * Throws exception on errors.
     */
    virtual std::vector<NamespaceString> getAllShardedCollectionsForDb(
        OperationContext* opCtx, StringData dbName, repl::ReadConcernLevel readConcern) = 0;

    /**
     * Retrieves all databases for a shard.
     *
     * Returns a !OK status if an error occurs.
     */
    virtual StatusWith<std::vector<std::string>> getDatabasesForShard(OperationContext* opCtx,
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
    virtual std::pair<CollectionType, std::vector<IndexCatalogType>> getCollectionAndGlobalIndexes(
        OperationContext* opCtx,
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
                                                 StringData dbname,
                                                 const BSONObj& cmdObj,
                                                 BSONObjBuilder* result) = 0;

    /**
     * Runs a user management related read-only command on a config server.
     */
    virtual bool runUserManagementReadCommand(OperationContext* opCtx,
                                              const std::string& dbname,
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
     * Returns keys for the given purpose and with an expiresAt value greater than newerThanThis.
     */
    virtual StatusWith<std::vector<KeysCollectionDocument>> getNewKeys(
        OperationContext* opCtx,
        StringData purpose,
        const LogicalTime& newerThanThis,
        repl::ReadConcernLevel readConcernLevel) = 0;

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
