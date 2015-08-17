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

#include "mongo/s/catalog/forwarding_catalog_manager.h"

#include <vector>

#include "mongo/client/connection_string.h"
#include "mongo/s/catalog/type_settings.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/dist_lock_catalog_impl.h"
#include "mongo/s/catalog/legacy/catalog_manager_legacy.h"
#include "mongo/s/catalog/replset/catalog_manager_replica_set.h"
#include "mongo/s/catalog/replset/replset_dist_lock_manager.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace {
std::unique_ptr<CatalogManager> makeCatalogManager(ServiceContext* service,
                                                   const ConnectionString& configCS,
                                                   ShardRegistry* shardRegistry,
                                                   const std::string& distLockProcessId) {
    switch (configCS.type()) {
        case ConnectionString::SET: {
            auto distLockCatalog = stdx::make_unique<DistLockCatalogImpl>(
                shardRegistry, ReplSetDistLockManager::kDistLockWriteConcernTimeout);

            auto distLockManager = stdx::make_unique<ReplSetDistLockManager>(
                service,
                distLockProcessId,
                std::move(distLockCatalog),
                ReplSetDistLockManager::kDistLockPingInterval,
                ReplSetDistLockManager::kDistLockExpirationTime);

            return stdx::make_unique<CatalogManagerReplicaSet>(std::move(distLockManager));
        }
        case ConnectionString::SYNC:
        case ConnectionString::MASTER:
        case ConnectionString::CUSTOM: {
            auto catalogManagerLegacy = stdx::make_unique<CatalogManagerLegacy>();
            uassertStatusOK(catalogManagerLegacy->init(configCS));
            return std::move(catalogManagerLegacy);
        }
        default:
            MONGO_UNREACHABLE;
    }
}
}

ForwardingCatalogManager::ForwardingCatalogManager(ServiceContext* service,
                                                   const ConnectionString& configCS,
                                                   ShardRegistry* shardRegistry,
                                                   const std::string& distLockProcessId)
    : _actual(makeCatalogManager(service, configCS, shardRegistry, distLockProcessId)) {}

ForwardingCatalogManager::~ForwardingCatalogManager() = default;

ServerGlobalParams::ConfigServerMode ForwardingCatalogManager::getMode() {
    return retry([this] { return _actual->getMode(); });
}

Status ForwardingCatalogManager::startup() {
    return retry([this] { return _actual->startup(); });
}

void ForwardingCatalogManager::shutDown() {
    _actual->shutDown();
}

Status ForwardingCatalogManager::shardCollection(OperationContext* txn,
                                                 const std::string& ns,
                                                 const ShardKeyPattern& fieldsAndOrder,
                                                 bool unique,
                                                 const std::vector<BSONObj>& initPoints,
                                                 const std::set<ShardId>& initShardsIds) {
    return retry([&] {
        return _actual->shardCollection(txn, ns, fieldsAndOrder, unique, initPoints, initShardsIds);
    });
}

StatusWith<ShardDrainingStatus> ForwardingCatalogManager::removeShard(OperationContext* txn,
                                                                      const std::string& name) {
    return retry([&] { return _actual->removeShard(txn, name); });
}

StatusWith<OpTimePair<DatabaseType>> ForwardingCatalogManager::getDatabase(
    const std::string& dbName) {
    return retry([&] { return _actual->getDatabase(dbName); });
}

StatusWith<OpTimePair<CollectionType>> ForwardingCatalogManager::getCollection(
    const std::string& collNs) {
    return retry([&] { return _actual->getCollection(collNs); });
}

Status ForwardingCatalogManager::getCollections(const std::string* dbName,
                                                std::vector<CollectionType>* collections,
                                                repl::OpTime* opTime) {
    invariant(collections->empty());
    return retry([&] { return _actual->getCollections(dbName, collections, opTime); });
}

Status ForwardingCatalogManager::dropCollection(OperationContext* txn, const NamespaceString& ns) {
    return retry([&] { return _actual->dropCollection(txn, ns); });
}

Status ForwardingCatalogManager::getDatabasesForShard(const std::string& shardName,
                                                      std::vector<std::string>* dbs) {
    invariant(dbs->empty());
    return retry([&] { return _actual->getDatabasesForShard(shardName, dbs); });
}

Status ForwardingCatalogManager::getChunks(const BSONObj& query,
                                           const BSONObj& sort,
                                           boost::optional<int> limit,
                                           std::vector<ChunkType>* chunks,
                                           repl::OpTime* opTime) {
    invariant(chunks->empty());
    return retry([&] { return _actual->getChunks(query, sort, limit, chunks, opTime); });
}

Status ForwardingCatalogManager::getTagsForCollection(const std::string& collectionNs,
                                                      std::vector<TagsType>* tags) {
    invariant(tags->empty());
    return retry([&] { return _actual->getTagsForCollection(collectionNs, tags); });
}

StatusWith<std::string> ForwardingCatalogManager::getTagForChunk(const std::string& collectionNs,
                                                                 const ChunkType& chunk) {
    return retry([&] { return _actual->getTagForChunk(collectionNs, chunk); });
}

Status ForwardingCatalogManager::getAllShards(std::vector<ShardType>* shards) {
    invariant(shards->empty());
    return retry([&] { return _actual->getAllShards(shards); });
}

bool ForwardingCatalogManager::runUserManagementWriteCommand(const std::string& commandName,
                                                             const std::string& dbname,
                                                             const BSONObj& cmdObj,
                                                             BSONObjBuilder* result) {
    return retry([&] {
        BSONObjBuilder builder;
        const bool success =
            _actual->runUserManagementWriteCommand(commandName, dbname, cmdObj, &builder);
        result->appendElements(builder.done());
        return success;
    });
}

bool ForwardingCatalogManager::runReadCommand(const std::string& dbname,
                                              const BSONObj& cmdObj,
                                              BSONObjBuilder* result) {
    return retry([&] {
        BSONObjBuilder builder;
        const bool success = _actual->runReadCommand(dbname, cmdObj, &builder);
        result->appendElements(builder.done());
        return success;
    });
}

bool ForwardingCatalogManager::runUserManagementReadCommand(const std::string& dbname,
                                                            const BSONObj& cmdObj,
                                                            BSONObjBuilder* result) {
    return retry([&] {
        BSONObjBuilder builder;
        const bool success = _actual->runUserManagementReadCommand(dbname, cmdObj, &builder);
        result->appendElements(builder.done());
        return success;
    });
}

Status ForwardingCatalogManager::applyChunkOpsDeprecated(const BSONArray& updateOps,
                                                         const BSONArray& preCondition) {
    return retry([&] { return _actual->applyChunkOpsDeprecated(updateOps, preCondition); });
}

void ForwardingCatalogManager::logAction(const ActionLogType& actionLog) {
    retry([&] { _actual->logAction(actionLog); });
}

void ForwardingCatalogManager::logChange(const std::string& clientAddress,
                                         const std::string& what,
                                         const std::string& ns,
                                         const BSONObj& detail) {
    retry([&] { _actual->logChange(clientAddress, what, ns, detail); });
}

StatusWith<SettingsType> ForwardingCatalogManager::getGlobalSettings(const std::string& key) {
    return retry([&] { return _actual->getGlobalSettings(key); });
}

void ForwardingCatalogManager::writeConfigServerDirect(const BatchedCommandRequest& request,
                                                       BatchedCommandResponse* response) {
    retry([&] {
        BatchedCommandResponse theResponse;
        _actual->writeConfigServerDirect(request, &theResponse);
        theResponse.cloneTo(response);
    });
}

DistLockManager* ForwardingCatalogManager::getDistLockManager() {
    return retry([&] { return _actual->getDistLockManager(); });
}

Status ForwardingCatalogManager::checkAndUpgrade(bool checkOnly) {
    return retry([&] { return _actual->checkAndUpgrade(checkOnly); });
}

Status ForwardingCatalogManager::_checkDbDoesNotExist(const std::string& dbName, DatabaseType* db) {
    return retry([&] { return _actual->_checkDbDoesNotExist(dbName, db); });
}
StatusWith<std::string> ForwardingCatalogManager::_generateNewShardName() {
    return retry([&] { return _actual->_generateNewShardName(); });
}

template <typename Callable>
auto ForwardingCatalogManager::retry(Callable&& c) -> decltype(std::forward<Callable>(c)()) {
    return std::forward<Callable>(c)();
}


}  // namespace mongo
