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

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/sharding_catalog_client_mock.h"

#include "mongo/base/status.h"
#include "mongo/db/repl/optime.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_config_version.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/stdx/memory.h"

namespace mongo {

using std::string;
using std::vector;

ShardingCatalogClientMock::ShardingCatalogClientMock(
    std::unique_ptr<DistLockManager> distLockManager)
    : _distLockManager(std::move(distLockManager)) {}

ShardingCatalogClientMock::~ShardingCatalogClientMock() = default;

void ShardingCatalogClientMock::startup() {
    if (_distLockManager) {
        _distLockManager->startUp();
    }
}

void ShardingCatalogClientMock::shutDown(OperationContext* opCtx) {
    if (_distLockManager) {
        _distLockManager->shutDown(opCtx);
    }
}

Status ShardingCatalogClientMock::enableSharding(OperationContext* opCtx,
                                                 const std::string& dbName) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

StatusWith<ShardDrainingStatus> ShardingCatalogClientMock::removeShard(OperationContext* opCtx,
                                                                       const ShardId& name) {
    return ShardDrainingStatus::COMPLETED;
}

Status ShardingCatalogClientMock::updateDatabase(OperationContext* opCtx,
                                                 const string& dbName,
                                                 const DatabaseType& db) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

StatusWith<repl::OpTimeWith<DatabaseType>> ShardingCatalogClientMock::getDatabase(
    OperationContext* opCtx, const string& dbName) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

StatusWith<repl::OpTimeWith<CollectionType>> ShardingCatalogClientMock::getCollection(
    OperationContext* opCtx, const string& collNs) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status ShardingCatalogClientMock::getCollections(OperationContext* opCtx,
                                                 const string* dbName,
                                                 vector<CollectionType>* collections,
                                                 repl::OpTime* optime) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status ShardingCatalogClientMock::dropCollection(OperationContext* opCtx,
                                                 const NamespaceString& ns) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status ShardingCatalogClientMock::getDatabasesForShard(OperationContext* opCtx,
                                                       const ShardId& shardName,
                                                       vector<string>* dbs) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status ShardingCatalogClientMock::getChunks(OperationContext* opCtx,
                                            const BSONObj& filter,
                                            const BSONObj& sort,
                                            boost::optional<int> limit,
                                            std::vector<ChunkType>* chunks,
                                            repl::OpTime* opTime,
                                            repl::ReadConcernLevel readConcern) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status ShardingCatalogClientMock::getTagsForCollection(OperationContext* opCtx,
                                                       const string& collectionNs,
                                                       vector<TagsType>* tags) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

StatusWith<repl::OpTimeWith<std::vector<ShardType>>> ShardingCatalogClientMock::getAllShards(
    OperationContext* opCtx, repl::ReadConcernLevel readConcern) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

bool ShardingCatalogClientMock::runUserManagementWriteCommand(OperationContext* opCtx,
                                                              const string& commandName,
                                                              const string& dbname,
                                                              const BSONObj& cmdObj,
                                                              BSONObjBuilder* result) {
    return true;
}

bool ShardingCatalogClientMock::runUserManagementReadCommand(OperationContext* opCtx,
                                                             const string& dbname,
                                                             const BSONObj& cmdObj,
                                                             BSONObjBuilder* result) {
    return true;
}

Status ShardingCatalogClientMock::applyChunkOpsDeprecated(OperationContext* opCtx,
                                                          const BSONArray& updateOps,
                                                          const BSONArray& preCondition,
                                                          const std::string& nss,
                                                          const ChunkVersion& lastChunkVersion,
                                                          const WriteConcernOptions& writeConcern,
                                                          repl::ReadConcernLevel readConcern) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status ShardingCatalogClientMock::logAction(OperationContext* opCtx,
                                            const std::string& what,
                                            const std::string& ns,
                                            const BSONObj& detail) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status ShardingCatalogClientMock::logChange(OperationContext* opCtx,
                                            const string& what,
                                            const string& ns,
                                            const BSONObj& detail,
                                            const WriteConcernOptions& writeConcern) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

StatusWith<BSONObj> ShardingCatalogClientMock::getGlobalSettings(OperationContext* opCtx,
                                                                 StringData key) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

StatusWith<VersionType> ShardingCatalogClientMock::getConfigVersion(
    OperationContext* opCtx, repl::ReadConcernLevel readConcern) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

void ShardingCatalogClientMock::writeConfigServerDirect(OperationContext* opCtx,
                                                        const BatchedCommandRequest& request,
                                                        BatchedCommandResponse* response) {}

Status ShardingCatalogClientMock::insertConfigDocument(OperationContext* opCtx,
                                                       const std::string& ns,
                                                       const BSONObj& doc,
                                                       const WriteConcernOptions& writeConcern) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

StatusWith<bool> ShardingCatalogClientMock::updateConfigDocument(
    OperationContext* opCtx,
    const std::string& ns,
    const BSONObj& query,
    const BSONObj& update,
    bool upsert,
    const WriteConcernOptions& writeConcern) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status ShardingCatalogClientMock::removeConfigDocuments(OperationContext* opCtx,
                                                        const std::string& ns,
                                                        const BSONObj& query,
                                                        const WriteConcernOptions& writeConcern) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status ShardingCatalogClientMock::createDatabase(OperationContext* opCtx,
                                                 const std::string& dbName) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

DistLockManager* ShardingCatalogClientMock::getDistLockManager() {
    return _distLockManager.get();
}

Status ShardingCatalogClientMock::appendInfoForConfigServerDatabases(
    OperationContext* opCtx, const BSONObj& listDatabasesCmd, BSONArrayBuilder* builder) {
    return Status::OK();
}

StatusWith<std::vector<KeysCollectionDocument>> ShardingCatalogClientMock::getNewKeys(
    OperationContext* opCtx,
    StringData purpose,
    const LogicalTime& newerThanThis,
    repl::ReadConcernLevel readConcernLevel) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status ShardingCatalogClientMock::_checkDbDoesNotExist(OperationContext* opCtx,
                                                       const string& dbName,
                                                       DatabaseType* db) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

}  // namespace mongo
