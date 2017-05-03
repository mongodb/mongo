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

#include "mongo/client/connection_string.h"
#include "mongo/db/repl/optime.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/stdx/mutex.h"

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
    explicit ShardingCatalogClientImpl(std::unique_ptr<DistLockManager> distLockManager);
    virtual ~ShardingCatalogClientImpl();

    /**
     * Safe to call multiple times as long as the calls are externally synchronized to be
     * non-overlapping.
     */
    Status startup() override;

    void shutDown(OperationContext* opCtx) override;

    Status enableSharding(OperationContext* opCtx, const std::string& dbName) override;

    Status updateDatabase(OperationContext* opCtx,
                          const std::string& dbName,
                          const DatabaseType& db) override;

    Status updateCollection(OperationContext* opCtx,
                            const std::string& collNs,
                            const CollectionType& coll) override;

    Status createDatabase(OperationContext* opCtx, const std::string& dbName) override;

    Status logAction(OperationContext* opCtx,
                     const std::string& what,
                     const std::string& ns,
                     const BSONObj& detail) override;

    Status logChange(OperationContext* opCtx,
                     const std::string& what,
                     const std::string& ns,
                     const BSONObj& detail,
                     const WriteConcernOptions& writeConcern) override;

    Status shardCollection(OperationContext* opCtx,
                           const std::string& ns,
                           const ShardKeyPattern& fieldsAndOrder,
                           const BSONObj& defaultCollation,
                           bool unique,
                           const std::vector<BSONObj>& initPoints,
                           const std::set<ShardId>& initShardsIds) override;

    StatusWith<ShardDrainingStatus> removeShard(OperationContext* opCtx,
                                                const ShardId& name) override;

    StatusWith<repl::OpTimeWith<DatabaseType>> getDatabase(OperationContext* opCtx,
                                                           const std::string& dbName) override;

    StatusWith<repl::OpTimeWith<CollectionType>> getCollection(OperationContext* opCtx,
                                                               const std::string& collNs) override;

    Status getCollections(OperationContext* opCtx,
                          const std::string* dbName,
                          std::vector<CollectionType>* collections,
                          repl::OpTime* optime) override;

    Status dropCollection(OperationContext* opCtx, const NamespaceString& ns) override;

    Status getDatabasesForShard(OperationContext* opCtx,
                                const ShardId& shardName,
                                std::vector<std::string>* dbs) override;

    Status getChunks(OperationContext* opCtx,
                     const BSONObj& query,
                     const BSONObj& sort,
                     boost::optional<int> limit,
                     std::vector<ChunkType>* chunks,
                     repl::OpTime* opTime,
                     repl::ReadConcernLevel readConcern) override;

    Status getTagsForCollection(OperationContext* opCtx,
                                const std::string& collectionNs,
                                std::vector<TagsType>* tags) override;

    StatusWith<repl::OpTimeWith<std::vector<ShardType>>> getAllShards(
        OperationContext* opCtx, repl::ReadConcernLevel readConcern) override;

    bool runUserManagementWriteCommand(OperationContext* opCtx,
                                       const std::string& commandName,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       BSONObjBuilder* result) override;

    bool runUserManagementReadCommand(OperationContext* opCtx,
                                      const std::string& dbname,
                                      const BSONObj& cmdObj,
                                      BSONObjBuilder* result) override;

    Status applyChunkOpsDeprecated(OperationContext* opCtx,
                                   const BSONArray& updateOps,
                                   const BSONArray& preCondition,
                                   const std::string& nss,
                                   const ChunkVersion& lastChunkVersion,
                                   const WriteConcernOptions& writeConcern,
                                   repl::ReadConcernLevel readConcern) override;

    StatusWith<BSONObj> getGlobalSettings(OperationContext* opCtx, StringData key) override;

    StatusWith<VersionType> getConfigVersion(OperationContext* opCtx,
                                             repl::ReadConcernLevel readConcern) override;

    void writeConfigServerDirect(OperationContext* opCtx,
                                 const BatchedCommandRequest& request,
                                 BatchedCommandResponse* response) override;

    Status insertConfigDocument(OperationContext* opCtx,
                                const std::string& ns,
                                const BSONObj& doc,
                                const WriteConcernOptions& writeConcern) override;

    StatusWith<bool> updateConfigDocument(OperationContext* opCtx,
                                          const std::string& ns,
                                          const BSONObj& query,
                                          const BSONObj& update,
                                          bool upsert,
                                          const WriteConcernOptions& writeConcern) override;

    Status removeConfigDocuments(OperationContext* opCtx,
                                 const std::string& ns,
                                 const BSONObj& query,
                                 const WriteConcernOptions& writeConcern) override;

    DistLockManager* getDistLockManager() override;

    Status appendInfoForConfigServerDatabases(OperationContext* opCtx,
                                              const BSONObj& listDatabasesCmd,
                                              BSONArrayBuilder* builder) override;

    /**
     * Runs a read command against the config server with majority read concern.
     */
    bool runReadCommandForTest(OperationContext* opCtx,
                               const std::string& dbname,
                               const BSONObj& cmdObj,
                               BSONObjBuilder* result);

    StatusWith<std::vector<KeysCollectionDocument>> getNewKeys(
        OperationContext* opCtx,
        StringData purpose,
        const LogicalTime& newerThanThis,
        repl::ReadConcernLevel readConcernLevel) override;

private:
    /**
     * Selects an optimal shard on which to place a newly created database from the set of
     * available shards. Will return ShardNotFound if shard could not be found.
     */
    static StatusWith<ShardId> _selectShardForNewDatabase(OperationContext* opCtx,
                                                          ShardRegistry* shardRegistry);

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
    Status _checkDbDoesNotExist(OperationContext* opCtx,
                                const std::string& dbName,
                                DatabaseType* db);

    /**
     * Creates the specified collection name in the config database.
     */
    Status _createCappedConfigCollection(OperationContext* opCtx,
                                         StringData collName,
                                         int cappedSize,
                                         const WriteConcernOptions& writeConcern);

    /**
     * Helper method for running a count command against the config server with appropriate
     * error handling.
     */
    StatusWith<long long> _runCountCommandOnConfig(OperationContext* opCtx,
                                                   const NamespaceString& ns,
                                                   BSONObj query);

    StatusWith<repl::OpTimeWith<std::vector<BSONObj>>> _exhaustiveFindOnConfig(
        OperationContext* opCtx,
        const ReadPreferenceSetting& readPref,
        repl::ReadConcernLevel readConcern,
        const NamespaceString& nss,
        const BSONObj& query,
        const BSONObj& sort,
        boost::optional<long long> limit);

    /**
     * Appends a read committed read concern to the request object.
     */
    void _appendReadConcern(BSONObjBuilder* builder);

    /**
     * Queries the config servers for the database metadata for the given database, using the
     * given read preference.  Returns NamespaceNotFound if no database metadata is found.
     */
    StatusWith<repl::OpTimeWith<DatabaseType>> _fetchDatabaseMetadata(
        OperationContext* opCtx, const std::string& dbName, const ReadPreferenceSetting& readPref);

    /**
     * Best effort method, which logs diagnostic events on the config server. If the config server
     * write fails for any reason a warning will be written to the local service log and the method
     * will return a failed status.
     *
     * @param opCtx Operation context in which the call is running
     * @param logCollName Which config collection to write to (excluding the database name)
     * @param what E.g. "split", "migrate" (not interpreted)
     * @param operationNS To which collection the metadata change is being applied (not interpreted)
     * @param detail Additional info about the metadata change (not interpreted)
     * @param writeConcern Write concern options to use for logging
     */
    Status _log(OperationContext* opCtx,
                const StringData& logCollName,
                const std::string& what,
                const std::string& operationNS,
                const BSONObj& detail,
                const WriteConcernOptions& writeConcern);

    //
    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (M) Must hold _mutex for access.
    // (R) Read only, can only be written during initialization.
    // (S) Self-synchronizing; access in any way from any context.
    //

    stdx::mutex _mutex;

    // Distributed lock manager singleton.
    std::unique_ptr<DistLockManager> _distLockManager;  // (R)

    // True if shutDown() has been called. False, otherwise.
    bool _inShutdown = false;  // (M)

    // True if startup() has been called.
    bool _started = false;  // (M)

    // Whether the logAction call should attempt to create the actionlog collection
    AtomicInt32 _actionLogCollectionCreated{0};  // (S)

    // Whether the logChange call should attempt to create the changelog collection
    AtomicInt32 _changeLogCollectionCreated{0};  // (S)
};

}  // namespace mongo
