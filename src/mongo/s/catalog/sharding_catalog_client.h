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

#pragma once

#include <boost/optional.hpp>
#include <set>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/keys_collection_document.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/catalog/dist_lock_manager.h"
#include "mongo/s/client/shard.h"

namespace mongo {

class BatchedCommandRequest;
class BatchedCommandResponse;
struct BSONArray;
class BSONArrayBuilder;
class BSONObj;
class BSONObjBuilder;
class ChunkType;
struct ChunkVersion;
class CollectionType;
class ConnectionString;
class DatabaseType;
class LogicalTime;
class NamespaceString;
class OperationContext;
class ShardingCatalogManager;
class ShardKeyPattern;
class ShardRegistry;
class ShardType;
class Status;
template <typename T>
class StatusWith;
class TagsType;
class VersionType;

namespace executor {
struct ConnectionPoolStats;
}

/**
 * Used to indicate to the caller of the removeShard method whether draining of chunks for
 * a particular shard has started, is ongoing, or has been completed.
 */
enum ShardDrainingStatus {
    STARTED,
    ONGOING,
    COMPLETED,
};

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
    MONGO_DISALLOW_COPYING(ShardingCatalogClient);

    // Allows ShardingCatalogManager to access _checkDbDoesNotExist
    // TODO: move _checkDbDoesNotExist to ShardingCatalogManager when
    // ShardingCatalogClient::createDatabaseCommand, the other caller of this function,
    // is moved into ShardingCatalogManager.
    // SERVER-30022.
    friend class ShardingCatalogManager;

public:
    // Constant to use for configuration data majority writes
    static const WriteConcernOptions kMajorityWriteConcern;

    virtual ~ShardingCatalogClient() = default;

    /**
     * Performs implementation-specific startup tasks. Must be run after the catalog client
     * has been installed into the global 'grid' object. Implementations do not need to guarantee
     * thread safety so callers should employ proper synchronization when calling this method.
     */
    virtual void startup() = 0;

    /**
     * Performs necessary cleanup when shutting down cleanly.
     */
    virtual void shutDown(OperationContext* opCtx) = 0;

    /**
     * Tries to remove a shard. To completely remove a shard from a sharded cluster,
     * the data residing in that shard must be moved to the remaining shards in the
     * cluster by "draining" chunks from that shard.
     *
     * Because of the asynchronous nature of the draining mechanism, this method returns
     * the current draining status. See ShardDrainingStatus enum definition for more details.
     */
    virtual StatusWith<ShardDrainingStatus> removeShard(OperationContext* opCtx,
                                                        const ShardId& name) = 0;

    /**
     * Updates or creates the metadata for a given database.
     */
    virtual Status updateDatabase(OperationContext* opCtx,
                                  const std::string& dbName,
                                  const DatabaseType& db) = 0;

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
    virtual StatusWith<repl::OpTimeWith<DatabaseType>> getDatabase(OperationContext* opCtx,
                                                                   const std::string& dbName) = 0;

    /**
     * Retrieves the metadata for a given collection, if it exists.
     *
     * @param collectionNs fully qualified name of the collection (case sensitive)
     *
     * Returns Status::OK along with the collection information and the OpTime of the config server
     * which the collection information was based upon. Otherwise, returns an error code indicating
     * the failure. These are some of the known failures:
     *  - NamespaceNotFound - collection does not exist
     */
    virtual StatusWith<repl::OpTimeWith<CollectionType>> getCollection(
        OperationContext* opCtx, const std::string& collNs) = 0;

    /**
     * Retrieves all collections undera specified database (or in the system).
     *
     * @param dbName an optional database name. Must be nullptr or non-empty. If nullptr is
     *      specified, all collections on the system are returned.
     * @param collections variable to receive the set of collections.
     * @param optime an out parameter that will contain the opTime of the config server.
     *      Can be null. Note that collections can be fetched in multiple batches and each batch
     *      can have a unique opTime. This opTime will be the one from the last batch.
     *
     * Returns a !OK status if an error occurs.
     */
    virtual Status getCollections(OperationContext* opCtx,
                                  const std::string* dbName,
                                  std::vector<CollectionType>* collections,
                                  repl::OpTime* optime) = 0;

    /**
     * Drops the specified collection from the collection metadata store.
     *
     * Returns Status::OK if successful or any error code indicating the failure. These are
     * some of the known failures:
     *  - NamespaceNotFound - collection does not exist
     */
    virtual Status dropCollection(OperationContext* opCtx, const NamespaceString& ns) = 0;

    /**
     * Retrieves all databases for a shard.
     *
     * Returns a !OK status if an error occurs.
     */
    virtual Status getDatabasesForShard(OperationContext* opCtx,
                                        const ShardId& shardId,
                                        std::vector<std::string>* dbs) = 0;

    /**
     * Gets the requested number of chunks (of type ChunkType) that satisfy a query.
     *
     * @param filter The query to filter out the results.
     * @param sort Fields to use for sorting the results. Pass empty BSON object for no sort.
     * @param limit The number of chunk entries to return. Pass boost::none for no limit.
     * @param chunks Vector entry to receive the results
     * @param optime an out parameter that will contain the opTime of the config server.
     *      Can be null. Note that chunks can be fetched in multiple batches and each batch
     *      can have a unique opTime. This opTime will be the one from the last batch.
     * @param readConcern The readConcern to use while querying for chunks.
     *
     * Returns a !OK status if an error occurs.
     */
    virtual Status getChunks(OperationContext* opCtx,
                             const BSONObj& filter,
                             const BSONObj& sort,
                             boost::optional<int> limit,
                             std::vector<ChunkType>* chunks,
                             repl::OpTime* opTime,
                             repl::ReadConcernLevel readConcern) = 0;

    /**
     * Retrieves all tags for the specified collection.
     */
    virtual Status getTagsForCollection(OperationContext* opCtx,
                                        const std::string& collectionNs,
                                        std::vector<TagsType>* tags) = 0;

    /**
     * Retrieves all shards in this sharded cluster.
     * Returns a !OK status if an error occurs.
     */
    virtual StatusWith<repl::OpTimeWith<std::vector<ShardType>>> getAllShards(
        OperationContext* opCtx, repl::ReadConcernLevel readConcern) = 0;

    /**
     * Runs a user management command on the config servers, potentially synchronizing through
     * a distributed lock. Do not use for general write command execution.
     *
     * @param commandName: name of command
     * @param dbname: database for which the user management command is invoked
     * @param cmdObj: command obj
     * @param result: contains data returned from config servers
     * Returns true on success.
     */
    virtual bool runUserManagementWriteCommand(OperationContext* opCtx,
                                               const std::string& commandName,
                                               const std::string& dbname,
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
     * Applies oplog entries to the config servers.
     * Used by mergeChunk, splitChunk, and moveChunk commands.
     *
     * @param updateOps: documents to write to the chunks collection.
     * @param preCondition: preconditions for applying documents.
     * @param nss: namespace string for the chunks collection.
     * @param lastChunkVersion: version of the last document being written to the chunks
     * collection.
     * @param writeConcern: writeConcern to use for applying documents.
     * @param readConcern: readConcern to use for verifying that documents have been applied.
     *
     * 'nss' and 'lastChunkVersion' uniquely identify the last document being written, which is
     * expected to appear in the chunks collection on success. This is important for the
     * case where network problems cause a retry of a successful write, which then returns
     * failure because the precondition no longer matches. If a query of the chunks collection
     * returns a document matching both 'nss' and 'lastChunkVersion,' the write succeeded.
     */
    virtual Status applyChunkOpsDeprecated(OperationContext* opCtx,
                                           const BSONArray& updateOps,
                                           const BSONArray& preCondition,
                                           const std::string& nss,
                                           const ChunkVersion& lastChunkVersion,
                                           const WriteConcernOptions& writeConcern,
                                           repl::ReadConcernLevel readConcern) = 0;

    /**
     * Writes a diagnostic event to the action log.
     */
    virtual Status logAction(OperationContext* opCtx,
                             const std::string& what,
                             const std::string& ns,
                             const BSONObj& detail) = 0;

    /**
     * Writes a diagnostic event to the change log.
     */
    virtual Status logChange(OperationContext* opCtx,
                             const std::string& what,
                             const std::string& ns,
                             const BSONObj& detail,
                             const WriteConcernOptions& writeConcern) = 0;

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
     * Directly sends the specified command to the config server and returns the response.
     *
     * NOTE: Usage of this function is disallowed in new code, which should instead go through
     *       the regular catalog management calls. It is currently only used privately by this
     *       class and externally for writes to the admin/config namespaces.
     *
     * @param request Request to be sent to the config server.
     * @param response Out parameter to receive the response. Can be nullptr.
     */
    virtual void writeConfigServerDirect(OperationContext* opCtx,
                                         const BatchedCommandRequest& request,
                                         BatchedCommandResponse* response) = 0;

    /**
     * Creates a new database entry for the specified database name in the configuration
     * metadata and sets the specified shard as primary.
     *
     * @param dbName name of the database (case sensitive)
     *
     * Returns Status::OK on success or any error code indicating the failure. These are some
     * of the known failures:
     *  - NamespaceExists - database already exists
     *  - DatabaseDifferCase - database already exists, but with a different case
     *  - ShardNotFound - could not find a shard to place the DB on
     */
    virtual Status createDatabase(OperationContext* opCtx, const std::string& dbName) = 0;

    /**
     * Directly inserts a document in the specified namespace on the config server. The document
     * must have an _id index. Must only be used for insertions in the 'config' database.
     *
     * NOTE: Should not be used in new code outside the ShardingCatalogManager.
     */
    virtual Status insertConfigDocument(OperationContext* opCtx,
                                        const std::string& ns,
                                        const BSONObj& doc,
                                        const WriteConcernOptions& writeConcern) = 0;

    /**
     * Updates a single document in the specified namespace on the config server. The document must
     * have an _id index. Must only be used for updates to the 'config' database.
     *
     * This method retries the operation on NotMaster or network errors, so it should only be used
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
                                                  const std::string& ns,
                                                  const BSONObj& query,
                                                  const BSONObj& update,
                                                  bool upsert,
                                                  const WriteConcernOptions& writeConcern) = 0;

    /**
     * Removes documents matching a particular query predicate from the specified namespace on the
     * config server. Must only be used for deletions from the 'config' database.
     *
     * NOTE: Should not be used in new code outside the ShardingCatalogManager.
     */
    virtual Status removeConfigDocuments(OperationContext* opCtx,
                                         const std::string& ns,
                                         const BSONObj& query,
                                         const WriteConcernOptions& writeConcern) = 0;

    /**
     * Appends the information about the config and admin databases in the config server with the
     * format for listDatabases, based on the listDatabases command parameters in
     * 'listDatabasesCmd'.
     */
    virtual Status appendInfoForConfigServerDatabases(OperationContext* opCtx,
                                                      const BSONObj& listDatabasesCmd,
                                                      BSONArrayBuilder* builder) = 0;

    /**
     * Obtains a reference to the distributed lock manager instance to use for synchronizing
     * system-wide changes.
     *
     * The returned reference is valid only as long as the catalog client is valid and should not
     * be cached.
     */
    virtual DistLockManager* getDistLockManager() = 0;

protected:
    ShardingCatalogClient() = default;

private:
    /**
     * Checks that the given database name doesn't already exist in the config.databases
     * collection, including under different casing. Optional db can be passed and will
     * be set with the database details if the given dbName exists.
     *
     * Returns OK status if the db does not exist.
     * Some known errors include:
     *  NamespaceExists if it exists with the same casing
     *  DatabaseDifferCase if it exists under different casing.
     */
    virtual Status _checkDbDoesNotExist(OperationContext* opCtx,
                                        const std::string& dbName,
                                        DatabaseType* db) = 0;
};

}  // namespace mongo
