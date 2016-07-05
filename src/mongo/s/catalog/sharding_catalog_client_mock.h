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

#include "mongo/s/catalog/dist_lock_manager_mock.h"
#include "mongo/s/catalog/sharding_catalog_client.h"

namespace mongo {

/**
 * A dummy implementation of ShardingCatalogClient for testing purposes.
 */
class ShardingCatalogClientMock : public ShardingCatalogClient {
public:
    ShardingCatalogClientMock();
    ~ShardingCatalogClientMock();

    Status startup() override;

    void shutDown(OperationContext* txn) override;

    Status enableSharding(OperationContext* txn, const std::string& dbName);

    Status shardCollection(OperationContext* txn,
                           const std::string& ns,
                           const ShardKeyPattern& fieldsAndOrder,
                           bool unique,
                           const std::vector<BSONObj>& initPoints,
                           const std::set<ShardId>& initShardIds) override;

    StatusWith<ShardDrainingStatus> removeShard(OperationContext* txn,
                                                const ShardId& name) override;

    Status updateDatabase(OperationContext* txn,
                          const std::string& dbName,
                          const DatabaseType& db) override;

    StatusWith<repl::OpTimeWith<DatabaseType>> getDatabase(OperationContext* txn,
                                                           const std::string& dbName) override;

    Status updateCollection(OperationContext* txn,
                            const std::string& collNs,
                            const CollectionType& coll) override;

    StatusWith<repl::OpTimeWith<CollectionType>> getCollection(OperationContext* txn,
                                                               const std::string& collNs) override;

    Status getCollections(OperationContext* txn,
                          const std::string* dbName,
                          std::vector<CollectionType>* collections,
                          repl::OpTime* optime) override;

    Status dropCollection(OperationContext* txn, const NamespaceString& ns) override;

    Status getDatabasesForShard(OperationContext* txn,
                                const ShardId& shardName,
                                std::vector<std::string>* dbs) override;

    Status getChunks(OperationContext* txn,
                     const BSONObj& filter,
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

    StatusWith<repl::OpTimeWith<std::vector<ShardType>>> getAllShards(
        OperationContext* txn) override;

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
                                   const BSONArray& preCondition,
                                   const std::string& nss,
                                   const ChunkVersion& lastChunkVersion) override;

    Status logAction(OperationContext* txn,
                     const std::string& what,
                     const std::string& ns,
                     const BSONObj& detail) override;

    Status logChange(OperationContext* txn,
                     const std::string& what,
                     const std::string& ns,
                     const BSONObj& detail) override;

    StatusWith<BSONObj> getGlobalSettings(OperationContext* txn, StringData key) override;

    StatusWith<VersionType> getConfigVersion(OperationContext* txn,
                                             repl::ReadConcernLevel readConcern) override;

    void writeConfigServerDirect(OperationContext* txn,
                                 const BatchedCommandRequest& request,
                                 BatchedCommandResponse* response) override;

    Status insertConfigDocument(OperationContext* txn,
                                const std::string& ns,
                                const BSONObj& doc,
                                const WriteConcernOptions& writeConcern) override;

    StatusWith<bool> updateConfigDocument(OperationContext* txn,
                                          const std::string& ns,
                                          const BSONObj& query,
                                          const BSONObj& update,
                                          bool upsert,
                                          const WriteConcernOptions& writeConcern) override;

    Status removeConfigDocuments(OperationContext* txn,
                                 const std::string& ns,
                                 const BSONObj& query,
                                 const WriteConcernOptions& writeConcern) override;

    Status createDatabase(OperationContext* txn, const std::string& dbName);

    DistLockManager* getDistLockManager() override;

    StatusWith<DistLockManager::ScopedDistLock> distLock(OperationContext* txn,
                                                         StringData name,
                                                         StringData whyMessage,
                                                         Milliseconds waitFor) override;

    Status appendInfoForConfigServerDatabases(OperationContext* txn,
                                              BSONArrayBuilder* builder) override;

private:
    std::unique_ptr<DistLockManagerMock> _mockDistLockMgr;
};

}  // namespace mongo
