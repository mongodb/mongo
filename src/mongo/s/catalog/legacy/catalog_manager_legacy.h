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
#include "mongo/s/catalog/catalog_manager_common.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"

namespace mongo {

/**
 * Implements the catalog manager using the legacy 3-config server protocol.
 */
class CatalogManagerLegacy final : public CatalogManagerCommon {
public:
    CatalogManagerLegacy();
    ~CatalogManagerLegacy();

    ConfigServerMode getMode() override {
        return ConfigServerMode::SCCC;
    }

    /**
     * Initializes the catalog manager with the hosts, which will be used as a configuration
     * server. Can only be called once for the lifetime.
     */
    Status init(const ConnectionString& configCS, const std::string& distLockProcessId);

    /**
     * Can terminate the server if called more than once.
     */
    Status startup(OperationContext* txn, bool allowNetworking) override;

    void shutDown(OperationContext* txn, bool allowNetworking) override;

    Status shardCollection(OperationContext* txn,
                           const std::string& ns,
                           const ShardKeyPattern& fieldsAndOrder,
                           bool unique,
                           const std::vector<BSONObj>& initPoints,
                           const std::set<ShardId>& initShardIds) override;

    StatusWith<ShardDrainingStatus> removeShard(OperationContext* txn,
                                                const std::string& name) override;

    StatusWith<OpTimePair<DatabaseType>> getDatabase(OperationContext* txn,
                                                     const std::string& dbName) override;

    StatusWith<OpTimePair<CollectionType>> getCollection(OperationContext* txn,
                                                         const std::string& collNs) override;

    Status getCollections(OperationContext* txn,
                          const std::string* dbName,
                          std::vector<CollectionType>* collections,
                          repl::OpTime* optime);

    Status dropCollection(OperationContext* txn, const NamespaceString& ns) override;

    Status getDatabasesForShard(OperationContext* txn,
                                const std::string& shardName,
                                std::vector<std::string>* dbs) override;

    Status getChunks(OperationContext* txn,
                     const BSONObj& query,
                     const BSONObj& sort,
                     boost::optional<int> limit,
                     std::vector<ChunkType>* chunks,
                     repl::OpTime* opTime) override;

    Status getTagsForCollection(OperationContext* txn,
                                const std::string& collectionNs,
                                std::vector<TagsType>* tags) override;

    StatusWith<std::string> getTagForChunk(OperationContext* txn,
                                           const std::string& collectionNs,
                                           const ChunkType& chunk) override;

    StatusWith<OpTimePair<std::vector<ShardType>>> getAllShards(OperationContext* txn) override;

    /**
     * Grabs a distributed lock and runs the command on all config servers.
     */
    bool runUserManagementWriteCommand(OperationContext* txn,
                                       const std::string& commandName,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       BSONObjBuilder* result) override;

    bool runUserManagementReadCommand(OperationContext* txn,
                                      const std::string& dbname,
                                      const BSONObj& cmdObj,
                                      BSONObjBuilder* result) override;

    Status applyChunkOpsDeprecated(OperationContext* txn,
                                   const BSONArray& updateOps,
                                   const BSONArray& preCondition) override;

    StatusWith<SettingsType> getGlobalSettings(OperationContext* txn,
                                               const std::string& key) override;

    void writeConfigServerDirect(OperationContext* txn,
                                 const BatchedCommandRequest& request,
                                 BatchedCommandResponse* response) override;

    Status insertConfigDocument(OperationContext* txn,
                                const std::string& ns,
                                const BSONObj& doc) override;

    StatusWith<bool> updateConfigDocument(OperationContext* txn,
                                          const std::string& ns,
                                          const BSONObj& query,
                                          const BSONObj& update,
                                          bool upsert) override;

    Status removeConfigDocuments(OperationContext* txn,
                                 const std::string& ns,
                                 const BSONObj& query) override;

    DistLockManager* getDistLockManager() override;

    Status initConfigVersion(OperationContext* txn) override;

    Status appendInfoForConfigServerDatabases(OperationContext* txn,
                                              BSONArrayBuilder* builder) override;

private:
    Status _checkDbDoesNotExist(OperationContext* txn,
                                const std::string& dbName,
                                DatabaseType* db) override;

    StatusWith<std::string> _generateNewShardName(OperationContext* txn) override;

    Status _createCappedConfigCollection(OperationContext* txn,
                                         StringData collName,
                                         int cappedSize) override;

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
     * Returns OK if all config servers that were contacted have the same state.
     * If inconsistency detected on first attempt, checks at most 3 more times.
     */
    Status _checkConfigServersConsistent(const unsigned tries = 4) const;

    /**
     * Checks data consistency amongst config servers every 60 seconds.
     */
    void _consistencyChecker();

    /**
     * Returns true if the config servers have the same contents since the last
     * check was performed.
     */
    bool _isConsistentFromLastCheck();

    /**
     * Sends a read only command to the config server.
     */
    bool _runReadCommand(OperationContext* txn,
                         const std::string& dbname,
                         const BSONObj& cmdObj,
                         BSONObjBuilder* result);

    // Parsed config server hosts, as specified on the command line.
    ConnectionString _configServerConnectionString;
    std::vector<ConnectionString> _configServers;

    // Distributed lock manager singleton.
    std::unique_ptr<DistLockManager> _distLockManager;

    // protects _inShutdown, _consistentFromLastCheck; used by _consistencyCheckerCV
    stdx::mutex _mutex;

    // True if CatalogManagerLegacy::shutDown has been called. False, otherwise.
    bool _inShutdown = false;

    // Set to true once startup() has been called and returned an OK status.  Allows startup() to be
    // called multiple times with any time after the first successful call being a no-op.
    bool _started = false;

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
