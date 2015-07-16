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

#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/s/catalog/dist_lock_manager_mock.h"

namespace mongo {

/**
 * A dummy implementation of CatalogManager for testing purposes.
 */
class CatalogManagerMock : public CatalogManager {
public:
    CatalogManagerMock();
    ~CatalogManagerMock();

    ConnectionString connectionString() const override;

    Status startup() override;

    void shutDown() override;

    Status shardCollection(OperationContext* txn,
                           const std::string& ns,
                           const ShardKeyPattern& fieldsAndOrder,
                           bool unique,
                           const std::vector<BSONObj>& initPoints,
                           const std::set<ShardId>& initShardIds) override;

    StatusWith<std::string> addShard(OperationContext* txn,
                                     const std::string* shardProposedName,
                                     const ConnectionString& shardConnectionString,
                                     const long long maxSize) override;

    StatusWith<ShardDrainingStatus> removeShard(OperationContext* txn,
                                                const std::string& name) override;

    Status updateDatabase(const std::string& dbName, const DatabaseType& db) override;

    StatusWith<DatabaseType> getDatabase(const std::string& dbName) override;

    Status updateCollection(const std::string& collNs, const CollectionType& coll) override;

    StatusWith<CollectionType> getCollection(const std::string& collNs) override;

    Status getCollections(const std::string* dbName,
                          std::vector<CollectionType>* collections) override;

    Status getDatabasesForShard(const std::string& shardName,
                                std::vector<std::string>* dbs) override;

    Status getChunks(const BSONObj& filter,
                     const BSONObj& sort,
                     boost::optional<int> limit,
                     std::vector<ChunkType>* chunks) override;

    Status getTagsForCollection(const std::string& collectionNs,
                                std::vector<TagsType>* tags) override;

    StatusWith<std::string> getTagForChunk(const std::string& collectionNs,
                                           const ChunkType& chunk) override;

    Status getAllShards(std::vector<ShardType>* shards) override;

    bool runUserManagementWriteCommand(const std::string& commandName,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       BSONObjBuilder* result) override;

    bool runReadCommand(const std::string& dbname,
                        const BSONObj& cmdObj,
                        BSONObjBuilder* result) override;

    Status applyChunkOpsDeprecated(const BSONArray& updateOps,
                                   const BSONArray& preCondition) override;

    void logAction(const ActionLogType& actionLog) override;

    void logChange(const std::string& clientAddress,
                   const std::string& what,
                   const std::string& ns,
                   const BSONObj& detail) override;

    StatusWith<SettingsType> getGlobalSettings(const std::string& key) override;

    void writeConfigServerDirect(const BatchedCommandRequest& request,
                                 BatchedCommandResponse* response) override;

    DistLockManager* getDistLockManager() const override;

    Status checkAndUpgrade(bool checkOnly) override;

private:
    Status _checkDbDoesNotExist(const std::string& dbName, DatabaseType* db) const override;

    StatusWith<std::string> _generateNewShardName() const override;

    std::unique_ptr<DistLockManagerMock> _mockDistLockMgr;
};

}  // namespace mongo
