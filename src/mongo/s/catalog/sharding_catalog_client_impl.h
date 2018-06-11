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
    /*
     * Updates (or if "upsert" is true, creates) catalog data for the sharded collection "collNs" by
     * writing a document to the "config.collections" collection with the catalog information
     * described by "coll."
     */
    static Status updateShardingCatalogEntryForCollection(OperationContext* opCtx,
                                                          const NamespaceString& nss,
                                                          const CollectionType& coll,
                                                          const bool upsert);

    explicit ShardingCatalogClientImpl(std::unique_ptr<DistLockManager> distLockManager);
    virtual ~ShardingCatalogClientImpl();

    /**
     * Safe to call multiple times as long as the calls are externally synchronized to be
     * non-overlapping.
     */
    void startup() override;

    void shutDown(OperationContext* opCtx) override;

    Status logAction(OperationContext* opCtx,
                     const std::string& what,
                     const std::string& ns,
                     const BSONObj& detail) override;

    Status logChange(OperationContext* opCtx,
                     const std::string& what,
                     const std::string& ns,
                     const BSONObj& detail,
                     const WriteConcernOptions& writeConcern) override;

    StatusWith<repl::OpTimeWith<DatabaseType>> getDatabase(
        OperationContext* opCtx,
        const std::string& dbName,
        repl::ReadConcernLevel readConcernLevel) override;

    StatusWith<repl::OpTimeWith<std::vector<DatabaseType>>> getAllDBs(
        OperationContext* opCtx, repl::ReadConcernLevel readConcern) override;

    StatusWith<repl::OpTimeWith<CollectionType>> getCollection(
        OperationContext* opCtx,
        const NamespaceString& nss,
        repl::ReadConcernLevel readConcernLevel) override;

    StatusWith<std::vector<CollectionType>> getCollections(
        OperationContext* opCtx,
        const std::string* dbName,
        repl::OpTime* optime,
        repl::ReadConcernLevel readConcernLevel) override;

    std::vector<NamespaceString> getAllShardedCollectionsForDb(
        OperationContext* opCtx, StringData dbName, repl::ReadConcernLevel readConcern) override;

    StatusWith<std::vector<std::string>> getDatabasesForShard(OperationContext* opCtx,
                                                              const ShardId& shardName) override;

    StatusWith<std::vector<ChunkType>> getChunks(OperationContext* opCtx,
                                                 const BSONObj& query,
                                                 const BSONObj& sort,
                                                 boost::optional<int> limit,
                                                 repl::OpTime* opTime,
                                                 repl::ReadConcernLevel readConcern) override;

    StatusWith<std::vector<TagsType>> getTagsForCollection(OperationContext* opCtx,
                                                           const NamespaceString& nss) override;

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
                                   const NamespaceString& nss,
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
                                const NamespaceString& nss,
                                const BSONObj& doc,
                                const WriteConcernOptions& writeConcern) override;

    StatusWith<bool> updateConfigDocument(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const BSONObj& query,
                                          const BSONObj& update,
                                          bool upsert,
                                          const WriteConcernOptions& writeConcern) override;

    Status removeConfigDocuments(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 const BSONObj& query,
                                 const WriteConcernOptions& writeConcern) override;

    DistLockManager* getDistLockManager() override;

    StatusWith<std::vector<KeysCollectionDocument>> getNewKeys(
        OperationContext* opCtx,
        StringData purpose,
        const LogicalTime& newerThanThis,
        repl::ReadConcernLevel readConcernLevel) override;

private:
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
     */
    static StatusWith<bool> _updateConfigDocument(OperationContext* opCtx,
                                                  const NamespaceString& nss,
                                                  const BSONObj& query,
                                                  const BSONObj& update,
                                                  bool upsert,
                                                  const WriteConcernOptions& writeConcern);

    /**
     * Creates the specified collection name in the config database.
     */
    Status _createCappedConfigCollection(OperationContext* opCtx,
                                         StringData collName,
                                         int cappedSize,
                                         const WriteConcernOptions& writeConcern);

    StatusWith<repl::OpTimeWith<std::vector<BSONObj>>> _exhaustiveFindOnConfig(
        OperationContext* opCtx,
        const ReadPreferenceSetting& readPref,
        const repl::ReadConcernLevel& readConcern,
        const NamespaceString& nss,
        const BSONObj& query,
        const BSONObj& sort,
        boost::optional<long long> limit) override;

    /**
     * Queries the config servers for the database metadata for the given database, using the
     * given read preference.  Returns NamespaceNotFound if no database metadata is found.
     */
    StatusWith<repl::OpTimeWith<DatabaseType>> _fetchDatabaseMetadata(
        OperationContext* opCtx,
        const std::string& dbName,
        const ReadPreferenceSetting& readPref,
        repl::ReadConcernLevel readConcernLevel);

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
                const std::string& operationNSS,
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
