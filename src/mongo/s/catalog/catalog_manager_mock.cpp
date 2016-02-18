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

#include "mongo/s/catalog/catalog_manager_mock.h"

#include "mongo/base/status.h"
#include "mongo/db/repl/optime.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_settings.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/stdx/memory.h"

namespace mongo {

using std::string;
using std::vector;

CatalogManagerMock::CatalogManagerMock() {
    _mockDistLockMgr = stdx::make_unique<DistLockManagerMock>();
}

CatalogManagerMock::~CatalogManagerMock() = default;

Status CatalogManagerMock::startup(OperationContext* txn, bool allowNetworking) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

void CatalogManagerMock::shutDown(OperationContext* txn, bool allowNetworking) {}

Status CatalogManagerMock::enableSharding(OperationContext* txn, const std::string& dbName) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status CatalogManagerMock::shardCollection(OperationContext* txn,
                                           const string& ns,
                                           const ShardKeyPattern& fieldsAndOrder,
                                           bool unique,
                                           const vector<BSONObj>& initPoints,
                                           const std::set<ShardId>& initShardIds) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

StatusWith<string> CatalogManagerMock::addShard(OperationContext* txn,
                                                const std::string* shardProposedName,
                                                const ConnectionString& shardConnectionString,
                                                const long long maxSize) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

StatusWith<ShardDrainingStatus> CatalogManagerMock::removeShard(OperationContext* txn,
                                                                const string& name) {
    return ShardDrainingStatus::COMPLETED;
}

Status CatalogManagerMock::updateDatabase(OperationContext* txn,
                                          const string& dbName,
                                          const DatabaseType& db) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

StatusWith<OpTimePair<DatabaseType>> CatalogManagerMock::getDatabase(OperationContext* txn,
                                                                     const string& dbName) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status CatalogManagerMock::updateCollection(OperationContext* txn,
                                            const string& collNs,
                                            const CollectionType& coll) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

StatusWith<OpTimePair<CollectionType>> CatalogManagerMock::getCollection(OperationContext* txn,
                                                                         const string& collNs) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status CatalogManagerMock::getCollections(OperationContext* txn,
                                          const string* dbName,
                                          vector<CollectionType>* collections,
                                          repl::OpTime* optime) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status CatalogManagerMock::dropCollection(OperationContext* txn, const NamespaceString& ns) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status CatalogManagerMock::getDatabasesForShard(OperationContext* txn,
                                                const string& shardName,
                                                vector<string>* dbs) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status CatalogManagerMock::getChunks(OperationContext* txn,
                                     const BSONObj& filter,
                                     const BSONObj& sort,
                                     boost::optional<int> limit,
                                     std::vector<ChunkType>* chunks,
                                     repl::OpTime* opTime) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status CatalogManagerMock::getTagsForCollection(OperationContext* txn,
                                                const string& collectionNs,
                                                vector<TagsType>* tags) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

StatusWith<string> CatalogManagerMock::getTagForChunk(OperationContext* txn,
                                                      const string& collectionNs,
                                                      const ChunkType& chunk) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

StatusWith<OpTimePair<std::vector<ShardType>>> CatalogManagerMock::getAllShards(
    OperationContext* txn) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

bool CatalogManagerMock::runUserManagementWriteCommand(OperationContext* txn,
                                                       const string& commandName,
                                                       const string& dbname,
                                                       const BSONObj& cmdObj,
                                                       BSONObjBuilder* result) {
    return true;
}

bool CatalogManagerMock::runUserManagementReadCommand(OperationContext* txn,
                                                      const string& dbname,
                                                      const BSONObj& cmdObj,
                                                      BSONObjBuilder* result) {
    return true;
}

Status CatalogManagerMock::applyChunkOpsDeprecated(OperationContext* txn,
                                                   const BSONArray& updateOps,
                                                   const BSONArray& preCondition,
                                                   const std::string& nss,
                                                   const ChunkVersion& lastChunkVersion) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status CatalogManagerMock::logAction(OperationContext* txn,
                                     const std::string& what,
                                     const std::string& ns,
                                     const BSONObj& detail) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status CatalogManagerMock::logChange(OperationContext* txn,
                                     const string& what,
                                     const string& ns,
                                     const BSONObj& detail) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

StatusWith<SettingsType> CatalogManagerMock::getGlobalSettings(OperationContext* txn,
                                                               const string& key) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

void CatalogManagerMock::writeConfigServerDirect(OperationContext* txn,
                                                 const BatchedCommandRequest& request,
                                                 BatchedCommandResponse* response) {}

Status CatalogManagerMock::insertConfigDocument(OperationContext* txn,
                                                const std::string& ns,
                                                const BSONObj& doc) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

StatusWith<bool> CatalogManagerMock::updateConfigDocument(OperationContext* txn,
                                                          const std::string& ns,
                                                          const BSONObj& query,
                                                          const BSONObj& update,
                                                          bool upsert) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status CatalogManagerMock::removeConfigDocuments(OperationContext* txn,
                                                 const std::string& ns,
                                                 const BSONObj& query) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status CatalogManagerMock::createDatabase(OperationContext* txn, const std::string& dbName) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

DistLockManager* CatalogManagerMock::getDistLockManager() {
    return _mockDistLockMgr.get();
}

Status CatalogManagerMock::initConfigVersion(OperationContext* txn) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status CatalogManagerMock::appendInfoForConfigServerDatabases(OperationContext* txn,
                                                              BSONArrayBuilder* builder) {
    return Status::OK();
}

}  // namespace mongo
