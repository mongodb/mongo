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

#include "mongo/s/catalog/dist_lock_manager.h"
#include "mongo/s/catalog/sharding_catalog_client.h"

namespace mongo {

/**
 * A dummy implementation of ShardingCatalogClient for testing purposes.
 */
class ShardingCatalogClientMock : public ShardingCatalogClient {
public:
    ShardingCatalogClientMock(std::unique_ptr<DistLockManager> distLockManager);
    ~ShardingCatalogClientMock();

    void startup() override;

    void shutDown(OperationContext* opCtx) override;

    Status enableSharding(OperationContext* opCtx, const std::string& dbName);

    StatusWith<ShardDrainingStatus> removeShard(OperationContext* opCtx,
                                                const ShardId& name) override;

    Status updateDatabase(OperationContext* opCtx,
                          const std::string& dbName,
                          const DatabaseType& db) override;

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
                     const BSONObj& filter,
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

    Status logAction(OperationContext* opCtx,
                     const std::string& what,
                     const std::string& ns,
                     const BSONObj& detail) override;

    Status logChange(OperationContext* opCtx,
                     const std::string& what,
                     const std::string& ns,
                     const BSONObj& detail,
                     const WriteConcernOptions& writeConcern) override;

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

    Status createDatabase(OperationContext* opCtx, const std::string& dbName);

    DistLockManager* getDistLockManager() override;

    Status appendInfoForConfigServerDatabases(OperationContext* opCtx,
                                              const BSONObj& listDatabasesCmd,
                                              BSONArrayBuilder* builder) override;

    StatusWith<std::vector<KeysCollectionDocument>> getNewKeys(
        OperationContext* opCtx,
        StringData purpose,
        const LogicalTime& newerThanThis,
        repl::ReadConcernLevel readConcernLevel) override;

private:
    std::unique_ptr<DistLockManager> _distLockManager;

    Status _checkDbDoesNotExist(OperationContext* opCtx,
                                const std::string& dbName,
                                DatabaseType* db) override;
};

}  // namespace mongo
