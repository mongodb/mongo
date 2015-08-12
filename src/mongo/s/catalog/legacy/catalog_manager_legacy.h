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
#include "mongo/platform/atomic_word.h"
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"

namespace mongo {

/**
 * Implements the catalog manager using the legacy 3-config server protocol.
 */
class CatalogManagerLegacy final : public CatalogManager {
public:
    CatalogManagerLegacy();
    ~CatalogManagerLegacy();

    /**
     * Initializes the catalog manager with the hosts, which will be used as a configuration
     * server. Can only be called once for the lifetime.
     */
    Status init(const ConnectionString& configCS);

    Status startup() override;

    ConnectionString connectionString() override;

    void shutDown() override;

    Status shardCollection(OperationContext* txn,
                           const std::string& ns,
                           const ShardKeyPattern& fieldsAndOrder,
                           bool unique,
                           const std::vector<BSONObj>& initPoints,
                           const std::set<ShardId>& initShardIds) override;

    StatusWith<ShardDrainingStatus> removeShard(OperationContext* txn,
                                                const std::string& name) override;

    StatusWith<DatabaseType> getDatabase(const std::string& dbName) override;

    StatusWith<CollectionType> getCollection(const std::string& collNs) override;

    Status getCollections(const std::string* dbName, std::vector<CollectionType>* collections);

    Status dropCollection(OperationContext* txn, const NamespaceString& ns) override;

    Status getDatabasesForShard(const std::string& shardName,
                                std::vector<std::string>* dbs) override;

    Status getChunks(const BSONObj& query,
                     const BSONObj& sort,
                     boost::optional<int> limit,
                     std::vector<ChunkType>* chunks) override;

    Status getTagsForCollection(const std::string& collectionNs,
                                std::vector<TagsType>* tags) override;

    StatusWith<std::string> getTagForChunk(const std::string& collectionNs,
                                           const ChunkType& chunk) override;

    Status getAllShards(std::vector<ShardType>* shards) override;

    /**
     * Grabs a distributed lock and runs the command on all config servers.
     */
    bool runUserManagementWriteCommand(const std::string& commandName,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       BSONObjBuilder* result) override;

    bool runReadCommand(const std::string& dbname,
                        const BSONObj& cmdObj,
                        BSONObjBuilder* result) override;

    bool runUserManagementReadCommand(const std::string& dbname,
                                      const BSONObj& cmdObj,
                                      BSONObjBuilder* result) override;

    Status applyChunkOpsDeprecated(const BSONArray& updateOps,
                                   const BSONArray& preCondition) override;

    void logAction(const ActionLogType& actionLog);

    void logChange(const std::string& clientAddress,
                   const std::string& what,
                   const std::string& ns,
                   const BSONObj& detail) override;

    StatusWith<SettingsType> getGlobalSettings(const std::string& key) override;

    void writeConfigServerDirect(const BatchedCommandRequest& request,
                                 BatchedCommandResponse* response) override;

    DistLockManager* getDistLockManager() override;

    Status checkAndUpgrade(bool checkOnly) override;

private:
    Status _checkDbDoesNotExist(const std::string& dbName, DatabaseType* db) override;

    StatusWith<std::string> _generateNewShardName() override;

    /**
     * Starts the thread that periodically checks data consistency amongst the config servers.
     * Note: this is not thread safe and can only be called once for the lifetime.
     */
    Status _startConfigServerChecker();

    /**
     * Returns the number of shards recognized by the config servers
     * in this sharded cluster.
     * Optional: use query parameter to filter shard count.
     */
    size_t _getShardCount(const BSONObj& query) const;

    /**
     * Returns true if all config servers have the same state.
     * If inconsistency detected on first attempt, checks at most 3 more times.
     */
    bool _checkConfigServersConsistent(const unsigned tries = 4) const;

    /**
     * Checks data consistency amongst config servers every 60 seconds.
     */
    void _consistencyChecker();

    /**
     * Returns true if the config servers have the same contents since the last
     * check was performed.
     */
    bool _isConsistentFromLastCheck();

    // Parsed config server hosts, as specified on the command line.
    ConnectionString _configServerConnectionString;
    std::vector<ConnectionString> _configServers;

    // Distribted lock manager singleton.
    std::unique_ptr<DistLockManager> _distLockManager;

    // Whether the logChange call should attempt to create the changelog collection
    AtomicInt32 _changeLogCollectionCreated;

    // Whether the logAction call should attempt to create the actionlog collection
    AtomicInt32 _actionLogCollectionCreated;

    // protects _inShutdown, _consistentFromLastCheck; used by _consistencyCheckerCV
    stdx::mutex _mutex;

    // True if CatalogManagerLegacy::shutDown has been called. False, otherwise.
    bool _inShutdown = false;

    // used by consistency checker thread to check if config
    // servers are consistent
    bool _consistentFromLastCheck = false;

    // Thread that runs dbHash on config servers for checking data consistency.
    stdx::thread _consistencyCheckerThread;

    // condition variable used by the consistency checker thread to wait
    // for <= 60s, on every iteration, until shutDown is called
    stdx::condition_variable _consistencyCheckerCV;
};

}  // namespace mongo
