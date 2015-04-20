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

#include <string>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/s/catalog/catalog_manager.h"

namespace mongo {

    /**
     * Implements the catalog manager using the legacy 3-config server protocol.
     */
    class CatalogManagerLegacy : public CatalogManager {
    public:
        CatalogManagerLegacy() = default;
        virtual ~CatalogManagerLegacy() = default;

        /**
         * Initializes the catalog manager with the hosts, which will be used as a configuration
         * server. Can only be called once for the lifetime.
         */
        Status init(const ConnectionString& configCS);

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

        virtual Status createDatabase(const std::string& dbName, const Shard* shard);

        virtual Status updateDatabase(const std::string& dbName, const DatabaseType& db);

        virtual StatusWith<DatabaseType> getDatabase(const std::string& dbName);

        virtual Status updateCollection(const std::string& collNs, const CollectionType& coll);

        virtual StatusWith<CollectionType> getCollection(const std::string& collNs);

        virtual Status getCollections(const std::string* dbName,
                                      std::vector<CollectionType>* collections);

        virtual Status dropCollection(const std::string& collectionNs);

        virtual void getDatabasesForShard(const std::string& shardName,
                                          std::vector<std::string>* dbs);

        virtual Status getChunksForShard(const std::string& shardName,
                                         std::vector<ChunkType>* chunks);

        virtual Status getAllShards(std::vector<ShardType>* shards);

        virtual bool isShardHost(const ConnectionString& shardConnectionString);

        virtual bool doShardsExist();

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

        // Parsed config server hosts, as specified on the command line.
        ConnectionString _configServerConnectionString;
        std::vector<ConnectionString> _configServers;
    };

} // namespace mongo
