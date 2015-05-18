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

#include <boost/thread/condition.hpp>
#include <boost/thread/thread.hpp>
#include <string>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/s/catalog/catalog_manager.h"

namespace mongo {

    class DistLockManager;


    /**
     * Implements the catalog manager using the legacy 3-config server protocol.
     */
    class CatalogManagerLegacy : public CatalogManager {
    public:
        CatalogManagerLegacy();
        virtual ~CatalogManagerLegacy();

        /**
         * Initializes the catalog manager with the hosts, which will be used as a configuration
         * server. Can only be called once for the lifetime.
         */
        Status init(const ConnectionString& configCS);

        /**
         * Starts the thread that periodically checks data consistency amongst the config servers.
         * Note: this is not thread safe and can only be called once for the lifetime.
         */
        Status startConfigServerChecker();

        virtual void shutDown() override;

        virtual Status enableSharding(const std::string& dbName);

        virtual Status shardCollection(const std::string& ns,
                                       const ShardKeyPattern& fieldsAndOrder,
                                       bool unique,
                                       std::vector<BSONObj>* initPoints,
                                       std::vector<Shard>* initShards);

        virtual StatusWith<std::string> addShard(const std::string& name,
                                                 const ConnectionString& shardConnectionString,
                                                 const long long maxSize);

        virtual StatusWith<ShardDrainingStatus> removeShard(OperationContext* txn,
                                                            const std::string& name);

        virtual Status createDatabase(const std::string& dbName);

        virtual Status updateDatabase(const std::string& dbName, const DatabaseType& db);

        virtual StatusWith<DatabaseType> getDatabase(const std::string& dbName);

        virtual Status updateCollection(const std::string& collNs, const CollectionType& coll);

        virtual StatusWith<CollectionType> getCollection(const std::string& collNs);

        virtual Status getCollections(const std::string* dbName,
                                      std::vector<CollectionType>* collections);

        virtual Status dropCollection(const std::string& collectionNs);

        Status getDatabasesForShard(const std::string& shardName,
                                    std::vector<std::string>* dbs) final;

        virtual Status getChunks(const Query& query,
                                 int nToReturn,
                                 std::vector<ChunkType>* chunks);

        Status getTagsForCollection(const std::string& collectionNs,
                                    std::vector<TagsType>* tags) final;

        StatusWith<std::string> getTagForChunk(const std::string& collectionNs,
                                               const ChunkType& chunk) final;

        virtual Status getAllShards(std::vector<ShardType>* shards);

        virtual bool isShardHost(const ConnectionString& shardConnectionString);

        virtual bool doShardsExist();

        /**
         * Grabs a distributed lock and runs the command on all config servers.
         */
        virtual bool runUserManagementWriteCommand(const std::string& commandName,
                                                   const std::string& dbname,
                                                   const BSONObj& cmdObj,
                                                   BSONObjBuilder* result);

        virtual bool runUserManagementReadCommand(const std::string& dbname,
                                                  const BSONObj& cmdObj,
                                                  BSONObjBuilder* result);

        virtual Status applyChunkOpsDeprecated(const BSONArray& updateOps,
                                               const BSONArray& preCondition);

        virtual void logAction(const ActionLogType& actionLog);

        virtual void logChange(OperationContext* txn,
                               const std::string& what,
                               const std::string& ns,
                               const BSONObj& detail);

        virtual StatusWith<SettingsType> getGlobalSettings(const std::string& key);

        virtual void writeConfigServerDirect(const BatchedCommandRequest& request,
                                             BatchedCommandResponse* response);

        virtual DistLockManager* getDistLockManager() override;

    private:
        /**
         * Direct network check to see if a particular database does not already exist with the
         * same name or different case.
         */
        Status _checkDbDoesNotExist(const std::string& dbName) const;

        /**
         * Generates a new shard name "shard<xxxx>"
         * where <xxxx> is an autoincrementing value and <xxxx> < 10000
         */
        StatusWith<std::string> _getNewShardName() const;

        /**
         * Returns the number of shards recognized by the config servers
         * in this sharded cluster.
         * Optional: use query parameter to filter shard count.
         */
        size_t _getShardCount(const BSONObj& query = {}) const;

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

        // protects _inShutdown, _consistentFromLastCheck; used by _consistencyCheckerCV
        boost::mutex _mutex;

        // True if CatalogManagerLegacy::shutDown has been called. False, otherwise.
        bool _inShutdown = false;

        // used by consistency checker thread to check if config
        // servers are consistent
        bool _consistentFromLastCheck = false;

        // Thread that runs dbHash on config servers for checking data consistency.
        boost::thread _consistencyCheckerThread;

        // condition variable used by the consistency checker thread to wait
        // for <= 60s, on every iteration, until shutDown is called
        boost::condition _consistencyCheckerCV;
    };

} // namespace mongo
