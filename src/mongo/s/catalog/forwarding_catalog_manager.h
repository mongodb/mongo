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
#include "mongo/db/server_options.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/s/catalog/dist_lock_manager.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/rwlock.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

class NamespaceString;
class ServiceContext;
class ShardRegistry;
class VersionType;
struct ReadPreferenceSetting;

/**
 * The ForwardingCatalogManager is an indirection layer that allows for dynamic switching of
 * catalog manager implementations at runtime, to facilitate upgrade.
 * Inheriting privately from CatalogManager is intentional.  All callers of CatalogManager methods
 * on a ForwardingCatalogManager will get access to the FCM pointer by calling
 * FCM::getCatalogManagerToUse, which can return a CatalogManager* because it is a member of FCM
 * and thus knows that FCM inherits from CatalogManager.  This makes it obvious if we try to call
 * CatalogManager methods directly on a ForwardingCatalogManager pointer.
 */
class ForwardingCatalogManager final : private CatalogManager {
public:
    class ScopedDistLock;

    ForwardingCatalogManager(ServiceContext* service,
                             const ConnectionString& configCS,
                             ShardRegistry* shardRegistry,
                             const HostAndPort& thisHost);

    /**
     * Constructor for use in tests.
     */
    ForwardingCatalogManager(ServiceContext* service,
                             std::unique_ptr<CatalogManager> actual,
                             ShardRegistry* shardRegistry,
                             const HostAndPort& thisHost);

    virtual ~ForwardingCatalogManager();

    // Only public because of unit tests
    DistLockManager* getDistLockManager() override;

    /**
     * If desiredMode doesn't equal _actual->getMode(), schedules work to swap the actual catalog
     * manager to one of the type specified by desiredMode.
     * Currently only supports going to CSRS mode from SCCC mode.
     */
    Status scheduleReplaceCatalogManagerIfNeeded(ConfigServerMode desiredMode,
                                                 StringData replSetName,
                                                 const HostAndPort& knownServer);

    /**
     * Blocking method, which will waits for a previously scheduled catalog manager change to
     * complete. It is illegal to call unless scheduleReplaceCatalogManagerIfNeeded has been called.
     */
    void waitForCatalogManagerChange(OperationContext* txn);

    /**
     * Returns a ScopedDistLock which is the RAII type for holding a distributed lock.
     * ScopedDistLock prevents the underlying CatalogManager from being swapped as long as it is
     * in scope.
     */
    StatusWith<ScopedDistLock> distLock(
        OperationContext* txn,
        StringData name,
        StringData whyMessage,
        stdx::chrono::milliseconds waitFor = DistLockManager::kDefaultSingleLockAttemptTimeout,
        stdx::chrono::milliseconds lockTryInterval = DistLockManager::kDefaultLockRetryInterval);

    /**
     * Returns a pointer to the CatalogManager that should be used for general CatalogManager
     * operation.  Most of the time it will return 'this' - the ForwardingCatalogManager.  If there
     * is a distributed lock held as part of this operation, however, it will return the underlying
     * CatalogManager to prevent deadlock from occurring by trying to swap the catalog manager while
     * a distlock is held.
     */
    CatalogManager* getCatalogManagerToUse(OperationContext* txn);

    Status appendInfoForConfigServerDatabases(OperationContext* txn,
                                              BSONArrayBuilder* builder) override;

private:
    ConfigServerMode getMode() override;

    Status startup(OperationContext* txn, bool allowNetworking) override;

    void shutDown(OperationContext* txn, bool allowNetworking = true) override;

    Status enableSharding(OperationContext* txn, const std::string& dbName) override;

    Status shardCollection(OperationContext* txn,
                           const std::string& ns,
                           const ShardKeyPattern& fieldsAndOrder,
                           bool unique,
                           const std::vector<BSONObj>& initPoints,
                           const std::set<ShardId>& initShardsIds) override;

    StatusWith<std::string> addShard(OperationContext* txn,
                                     const std::string* shardProposedName,
                                     const ConnectionString& shardConnectionString,
                                     const long long maxSize) override;

    StatusWith<ShardDrainingStatus> removeShard(OperationContext* txn,
                                                const std::string& name) override;

    Status updateDatabase(OperationContext* txn,
                          const std::string& dbName,
                          const DatabaseType& db) override;

    StatusWith<OpTimePair<DatabaseType>> getDatabase(OperationContext* txn,
                                                     const std::string& dbName) override;

    Status updateCollection(OperationContext* txn,
                            const std::string& collNs,
                            const CollectionType& coll) override;

    StatusWith<OpTimePair<CollectionType>> getCollection(OperationContext* txn,
                                                         const std::string& collNs) override;

    Status getCollections(OperationContext* txn,
                          const std::string* dbName,
                          std::vector<CollectionType>* collections,
                          repl::OpTime* opTime) override;

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

    Status logAction(OperationContext* txn,
                     const std::string& what,
                     const std::string& ns,
                     const BSONObj& detail) override;

    Status logChange(OperationContext* txn,
                     const std::string& what,
                     const std::string& ns,
                     const BSONObj& detail) override;

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

    Status createDatabase(OperationContext* txn, const std::string& dbName) override;

    Status initConfigVersion(OperationContext* txn) override;

    template <typename Callable>
    auto retry(OperationContext* txn, Callable&& c) -> decltype(std::forward<Callable>(c)());

    void _replaceCatalogManager(const executor::TaskExecutor::CallbackArgs& args);

    ServiceContext* _service;
    ShardRegistry* _shardRegistry;
    HostAndPort _thisHost;

    RWLock _operationLock;
    stdx::mutex _observerMutex;  // If going to hold both _operationLock and _observerMutex, get
                                 // _operationLock first.

    // The actual catalog manager implementation.
    //
    // Must hold _operationLock or _observerMutex in any mode to read. Must hold both in exclusive
    // mode to write.
    std::unique_ptr<CatalogManager> _actual;

    ConnectionString _nextConfigConnectionString;                   // Guarded by _observerMutex.
    executor::TaskExecutor::EventHandle _nextConfigChangeComplete;  // Guarded by _observerMutex.
};

class ForwardingCatalogManager::ScopedDistLock {
    MONGO_DISALLOW_COPYING(ScopedDistLock);

public:
    ScopedDistLock(OperationContext* txn,
                   ForwardingCatalogManager* fcm,
                   DistLockManager::ScopedDistLock theLock);
    ScopedDistLock(ScopedDistLock&& other);
    ~ScopedDistLock();

    ScopedDistLock& operator=(ScopedDistLock&& other);

    /**
     * Checks to see if we are currently waiting to swap the catalog manager.  If so, holding on to
     * this ScopedDistLock will block the swap from happening, so it is important that if this
     * returns a non-OK status the caller must release the lock (most likely by failing the current
     * operation).
     */
    Status checkForPendingCatalogSwap();

    /**
     * Queries the config server to make sure the lock is still present, as well as checking
     * if we need to swap the catalog manager
     */
    Status checkStatus();

private:
    OperationContext* _txn;
    ForwardingCatalogManager* _fcm;
    DistLockManager::ScopedDistLock _lock;
};

}  // namespace mongo
