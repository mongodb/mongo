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
#include "mongo/db/repl/optime.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/s/catalog/catalog_manager_common.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

class NamespaceString;
struct ReadPreferenceSetting;
class VersionType;

/**
 * Implements the catalog manager for talking to replica set config servers.
 */
class CatalogManagerReplicaSet final : public CatalogManagerCommon {
public:
    explicit CatalogManagerReplicaSet(std::unique_ptr<DistLockManager> distLockManager);
    virtual ~CatalogManagerReplicaSet();

    ConfigServerMode getMode() override {
        return ConfigServerMode::CSRS;
    }

    /**
     * Safe to call multiple times as long as they
     */
    Status startup(OperationContext* txn, bool allowNetworking) override;

    void shutDown(OperationContext* txn, bool allowNetworking) override;

    Status shardCollection(OperationContext* txn,
                           const std::string& ns,
                           const ShardKeyPattern& fieldsAndOrder,
                           bool unique,
                           const std::vector<BSONObj>& initPoints,
                           const std::set<ShardId>& initShardsIds) override;

    StatusWith<ShardDrainingStatus> removeShard(OperationContext* txn,
                                                const std::string& name) override;

    StatusWith<OpTimePair<DatabaseType>> getDatabase(OperationContext* txn,
                                                     const std::string& dbName) override;

    StatusWith<OpTimePair<CollectionType>> getCollection(OperationContext* txn,
                                                         const std::string& collNs) override;

    Status getCollections(OperationContext* txn,
                          const std::string* dbName,
                          std::vector<CollectionType>* collections,
                          repl::OpTime* optime) override;

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

    Status getAllShards(OperationContext* txn, std::vector<ShardType>* shards) override;

    bool runUserManagementWriteCommand(OperationContext* txn,
                                       const std::string& commandName,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       BSONObjBuilder* result) override;

    bool runReadCommand(OperationContext* txn,
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

    void logAction(OperationContext* txn, const ActionLogType& actionLog) override;

    void logChange(OperationContext* txn,
                   const std::string& clientAddress,
                   const std::string& what,
                   const std::string& ns,
                   const BSONObj& detail) override;

    StatusWith<SettingsType> getGlobalSettings(OperationContext* txn,
                                               const std::string& key) override;

    void writeConfigServerDirect(OperationContext* txn,
                                 const BatchedCommandRequest& request,
                                 BatchedCommandResponse* response) override;

    DistLockManager* getDistLockManager() override;

    Status initConfigVersion(OperationContext* txn) override;

private:
    Status _checkDbDoesNotExist(OperationContext* txn,
                                const std::string& dbName,
                                DatabaseType* db) override;

    StatusWith<std::string> _generateNewShardName(OperationContext* txn) override;

    bool _runReadCommand(OperationContext* txn,
                         const std::string& dbname,
                         const BSONObj& cmdObj,
                         const ReadPreferenceSetting& settings,
                         BSONObjBuilder* result);

    /**
     * Helper method for running a count command against a given target server with appropriate
     * error handling.
     */
    StatusWith<long long> _runCountCommandOnConfig(const HostAndPort& target,
                                                   const NamespaceString& ns,
                                                   BSONObj query);

    StatusWith<BSONObj> _runCommandOnConfig(const HostAndPort& target,
                                            const std::string& dbName,
                                            BSONObj cmdObj);

    StatusWith<BSONObj> _runCommandOnConfigWithNotMasterRetries(const std::string& dbName,
                                                                BSONObj cmdObj);

    StatusWith<OpTimePair<std::vector<BSONObj>>> _exhaustiveFindOnConfig(
        const HostAndPort& host,
        const NamespaceString& nss,
        const BSONObj& query,
        const BSONObj& sort,
        boost::optional<long long> limit);

    /**
     * Appends a read committed read concern to the request object.
     */
    void _appendReadConcern(BSONObjBuilder* builder);

    /**
     * Returns the current cluster schema/protocol version.
     */
    StatusWith<VersionType> _getConfigVersion(OperationContext* txn);

    /**
     * Returns the highest last known config server opTime.
     */
    repl::OpTime _getConfigOpTime();

    /**
     * Updates the last known config server opTime if the given opTime is newer.
     */
    void _updateLastSeenConfigOpTime(const repl::OpTime& optime);

    //
    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (F) Self synchronizing.
    // (M) Must hold _mutex for access.
    // (R) Read only, can only be written during initialization.
    //

    stdx::mutex _mutex;

    // Distribted lock manager singleton.
    std::unique_ptr<DistLockManager> _distLockManager;  // (R)

    // Whether the logAction call should attempt to create the actionlog collection
    AtomicInt32 _actionLogCollectionCreated;  // (F)

    // Whether the logChange call should attempt to create the changelog collection
    AtomicInt32 _changeLogCollectionCreated;  // (F)

    // True if shutDown() has been called. False, otherwise.
    bool _inShutdown = false;  // (M)

    // Last known highest opTime from the config server.
    repl::OpTime _configOpTime;  // (M)
};

}  // namespace mongo
