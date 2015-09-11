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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/forwarding_catalog_manager.h"

#include <cstdint>
#include <vector>

#include "mongo/client/connection_string.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/random.h"
#include "mongo/s/catalog/dist_lock_catalog_impl.h"
#include "mongo/s/catalog/legacy/catalog_manager_legacy.h"
#include "mongo/s/catalog/replset/catalog_manager_replica_set.h"
#include "mongo/s/catalog/replset/replset_dist_lock_manager.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_settings.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using executor::TaskExecutor;

namespace {
std::unique_ptr<CatalogManager> makeCatalogManager(ServiceContext* service,
                                                   const ConnectionString& configCS,
                                                   ShardRegistry* shardRegistry,
                                                   const HostAndPort& thisHost) {
    switch (configCS.type()) {
        case ConnectionString::SET: {
            auto distLockCatalog = stdx::make_unique<DistLockCatalogImpl>(
                shardRegistry, ReplSetDistLockManager::kDistLockWriteConcernTimeout);

            std::unique_ptr<SecureRandom> rng(SecureRandom::create());
            std::string distLockProcessId = str::stream()
                << thisHost.toString() << ':'
                << durationCount<Seconds>(service->getClockSource()->now().toDurationSinceEpoch())
                << ':' << static_cast<int32_t>(rng->nextInt64());
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
                                                   const HostAndPort& thisHost)
    : ForwardingCatalogManager(service,
                               makeCatalogManager(service, configCS, shardRegistry, thisHost),
                               shardRegistry,
                               thisHost) {}

ForwardingCatalogManager::ForwardingCatalogManager(ServiceContext* service,
                                                   std::unique_ptr<CatalogManager> actual,
                                                   ShardRegistry* shardRegistry,
                                                   const HostAndPort& thisHost)
    : _service(service),
      _shardRegistry(shardRegistry),
      _thisHost(thisHost),
      _operationLock("CatalogOperationLock"),
      _actual(std::move(actual)) {}

ForwardingCatalogManager::~ForwardingCatalogManager() = default;


StatusWith<ForwardingCatalogManager::ScopedDistLock> ForwardingCatalogManager::distLock(
    OperationContext* txn,
    StringData name,
    StringData whyMessage,
    stdx::chrono::milliseconds waitFor,
    stdx::chrono::milliseconds lockTryInterval) {
    for (int i = 0; i < 2; ++i) {
        try {
            _operationLock.lock_shared();
            auto guard = MakeGuard([this] { _operationLock.unlock_shared(); });
            auto dlmLock =
                _actual->getDistLockManager()->lock(name, whyMessage, waitFor, lockTryInterval);
            if (dlmLock.isOK()) {
                guard.Dismiss();  // Don't unlock _operationLock; hold it until the returned
                                  // ScopedDistLock goes out of scope!
                try {
                    return ScopedDistLock(txn, this, std::move(dlmLock.getValue()));
                } catch (...) {
                    // Once the guard that unlocks _operationLock is dismissed, any exception before
                    // this method returns is fatal.
                    std::terminate();
                }
            } else if (dlmLock.getStatus() != ErrorCodes::IncompatibleCatalogManager) {
                return dlmLock.getStatus();
            }
        } catch (const DBException& ex) {
            if (ex.getCode() != ErrorCodes::IncompatibleCatalogManager) {
                throw;
            }
        }

        waitForCatalogManagerChange(txn);
    }
    MONGO_UNREACHABLE;
}

namespace {
const auto scopedDistLockHeld = OperationContext::declareDecoration<bool>();
}

CatalogManager* ForwardingCatalogManager::getCatalogManagerToUse(OperationContext* txn) {
    if (scopedDistLockHeld(txn)) {
        return _actual.get();
    } else {
        return this;
    }
}

ForwardingCatalogManager::ScopedDistLock::ScopedDistLock(OperationContext* txn,
                                                         ForwardingCatalogManager* fcm,
                                                         DistLockManager::ScopedDistLock theLock)
    : _txn(txn), _fcm(fcm), _lock(std::move(theLock)) {
    scopedDistLockHeld(txn) = true;
}

ForwardingCatalogManager::ScopedDistLock::ScopedDistLock(ScopedDistLock&& other)
    : _txn(other._txn), _fcm(other._fcm), _lock(std::move(other._lock)) {
    other._txn = nullptr;
    other._fcm = nullptr;
}

ForwardingCatalogManager::ScopedDistLock::~ScopedDistLock() {
    if (_fcm) {  // This ScopedDistLock was not moved from
        auto guard = MakeGuard([this] { _fcm->_operationLock.unlock_shared(); });
        DistLockManager::ScopedDistLock dlmLock = std::move(_lock);
        scopedDistLockHeld(_txn) = false;
    }
}

ForwardingCatalogManager::ScopedDistLock& ForwardingCatalogManager::ScopedDistLock::operator=(
    ScopedDistLock&& other) {
    invariant(!_fcm);
    _fcm = other._fcm;
    other._fcm = nullptr;
    _lock = std::move(other._lock);
    return *this;
}

Status ForwardingCatalogManager::ScopedDistLock::checkForPendingCatalogSwap() {
    stdx::lock_guard<stdx::mutex> lk(_fcm->_observerMutex);
    if (!_fcm->_nextConfigChangeComplete.isValid()) {
        return Status::OK();
    }
    return Status(ErrorCodes::IncompatibleCatalogManager,
                  "Need to swap sharding catalog manager.  Config server "
                  "reports that it is in replica set mode, but we are still using the "
                  "legacy SCCC protocol for config server communication");
}

Status ForwardingCatalogManager::ScopedDistLock::checkStatus() {
    Status status = checkForPendingCatalogSwap();
    if (!status.isOK()) {
        return status;
    }

    return _lock.checkStatus();
}

Status ForwardingCatalogManager::scheduleReplaceCatalogManagerIfNeeded(
    ConfigServerMode desiredMode, StringData replSetName, const HostAndPort& knownServer) {
    stdx::lock_guard<stdx::mutex> lk(_observerMutex);
    const auto currentMode = _actual->getMode();
    if (currentMode == desiredMode) {
        return Status::OK();
    }
    if (desiredMode == ConfigServerMode::SCCC) {
        // TODO(spencer): Support downgrade.
        return {ErrorCodes::IllegalOperation,
                "Config server reports that it legacy SCCC mode, but we are already using "
                "the replica set config server protocol for config server "
                "communication.  Downgrade needed but not yet supported"};
    }
    invariant(desiredMode == ConfigServerMode::CSRS);
    if (_nextConfigChangeComplete.isValid()) {
        if (_nextConfigConnectionString.getSetName() != replSetName) {
            severe() << "Conflicting new config server replica set names: "
                     << _nextConfigConnectionString.getSetName() << " vs " << replSetName;
            fassertFailed(28788);
        }
    } else {
        _nextConfigConnectionString = ConnectionString::forReplicaSet(replSetName, {knownServer});
        _nextConfigChangeComplete =
            fassertStatusOK(28789, _shardRegistry->getExecutor()->makeEvent());
        fassertStatusOK(
            28787,
            _shardRegistry->getExecutor()->scheduleWork(
                [this](const TaskExecutor::CallbackArgs& args) { _replaceCatalogManager(args); }));
    }
    return {ErrorCodes::IncompatibleCatalogManager,
            "Need to swap sharding catalog manager.  Config server "
            "reports that it is in replica set mode, but we are still using the "
            "legacy SCCC protocol for config server communication"};
}

void ForwardingCatalogManager::waitForCatalogManagerChange(OperationContext* txn) {
    fassert(28802, !scopedDistLockHeld(txn));

    stdx::unique_lock<stdx::mutex> oblk(_observerMutex);
    invariant(_nextConfigChangeComplete.isValid());
    auto configChangeComplete = _nextConfigChangeComplete;
    oblk.unlock();
    _shardRegistry->getExecutor()->waitForEvent(configChangeComplete);
}

namespace {

template <typename T>
struct CheckForIncompatibleCatalogManager {
    T operator()(T&& v) {
        return std::forward<T>(v);
    }
};

template <typename T>
struct CheckForIncompatibleCatalogManager<StatusWith<T>> {
    StatusWith<T> operator()(StatusWith<T>&& v) {
        if (!v.isOK() && v.getStatus().code() == ErrorCodes::IncompatibleCatalogManager) {
            uassertStatusOK(v);
        }
        return std::forward<StatusWith<T>>(v);
    }
};

template <>
struct CheckForIncompatibleCatalogManager<Status> {
    Status operator()(Status&& v) {
        if (v.code() == ErrorCodes::IncompatibleCatalogManager) {
            uassertStatusOK(v);
        }
        return std::move(v);
    }
};

template <typename T>
T checkForIncompatibleCatalogManager(T&& v) {
    return CheckForIncompatibleCatalogManager<T>()(std::forward<T>(v));
}

}  // namespace

template <typename Callable>
auto ForwardingCatalogManager::retry(OperationContext* txn, Callable&& c)
    -> decltype(std::forward<Callable>(c)()) {
    for (int i = 0; i < 2; ++i) {
        try {
            rwlock_shared oplk(_operationLock);
            return checkForIncompatibleCatalogManager(std::forward<Callable>(c)());
        } catch (const DBException& ex) {
            if (ex.getCode() != ErrorCodes::IncompatibleCatalogManager) {
                throw;
            }
        }

        waitForCatalogManagerChange(txn);
    }
    MONGO_UNREACHABLE;
}

void ForwardingCatalogManager::_replaceCatalogManager(const TaskExecutor::CallbackArgs& args) {
    if (!args.status.isOK()) {
        return;
    }
    Client::initThreadIfNotAlready();
    auto txn = cc().makeOperationContext();

    stdx::lock_guard<RWLock> oplk(_operationLock);
    stdx::lock_guard<stdx::mutex> oblk(_observerMutex);
    _actual->shutDown(txn.get(), /* allowNetworking */ false);
    _actual = makeCatalogManager(_service, _nextConfigConnectionString, _shardRegistry, _thisHost);
    _shardRegistry->updateConfigServerConnectionString(_nextConfigConnectionString);
    // Note: this assumes that downgrade is not supported, as this will not start the config
    // server consistency checker for the legacy catalog manager.
    fassert(28790, _actual->startup(txn.get(), false /* allowNetworking */));
    args.executor->signalEvent(_nextConfigChangeComplete);
}

CatalogManager::ConfigServerMode ForwardingCatalogManager::getMode() {
    stdx::lock_guard<stdx::mutex> lk(_observerMutex);
    return _actual->getMode();
}

Status ForwardingCatalogManager::startup(OperationContext* txn, bool allowNetworking) {
    return retry(txn,
                 [this, txn, allowNetworking] { return _actual->startup(txn, allowNetworking); });
}

void ForwardingCatalogManager::shutDown(OperationContext* txn, bool allowNetworking) {
    retry(txn,
          [this, txn, allowNetworking] {
              _actual->shutDown(txn, allowNetworking);
              return 1;
          });
}

void ForwardingCatalogManager::advanceConfigOpTime(OperationContext* txn, repl::OpTime opTime) {
    retry(txn,
          [&] {
              _actual->advanceConfigOpTime(txn, opTime);
              return 1;
          });
}

Status ForwardingCatalogManager::enableSharding(OperationContext* txn, const std::string& dbName) {
    return retry(txn, [&] { return _actual->enableSharding(txn, dbName); });
}

Status ForwardingCatalogManager::shardCollection(OperationContext* txn,
                                                 const std::string& ns,
                                                 const ShardKeyPattern& fieldsAndOrder,
                                                 bool unique,
                                                 const std::vector<BSONObj>& initPoints,
                                                 const std::set<ShardId>& initShardsIds) {
    return retry(txn,
                 [&] {
                     return _actual->shardCollection(
                         txn, ns, fieldsAndOrder, unique, initPoints, initShardsIds);
                 });
}

StatusWith<std::string> ForwardingCatalogManager::addShard(
    OperationContext* txn,
    const std::string* shardProposedName,
    const ConnectionString& shardConnectionString,
    const long long maxSize) {
    return retry(
        txn,
        [&] { return _actual->addShard(txn, shardProposedName, shardConnectionString, maxSize); });
}

StatusWith<ShardDrainingStatus> ForwardingCatalogManager::removeShard(OperationContext* txn,
                                                                      const std::string& name) {
    return retry(txn, [&] { return _actual->removeShard(txn, name); });
}

Status ForwardingCatalogManager::updateDatabase(OperationContext* txn,
                                                const std::string& dbName,
                                                const DatabaseType& db) {
    return retry(txn, [&] { return _actual->updateDatabase(txn, dbName, db); });
}

StatusWith<OpTimePair<DatabaseType>> ForwardingCatalogManager::getDatabase(
    OperationContext* txn, const std::string& dbName) {
    return retry(txn, [&] { return _actual->getDatabase(txn, dbName); });
}

Status ForwardingCatalogManager::updateCollection(OperationContext* txn,
                                                  const std::string& collNs,
                                                  const CollectionType& coll) {
    return retry(txn, [&] { return _actual->updateCollection(txn, collNs, coll); });
}

StatusWith<OpTimePair<CollectionType>> ForwardingCatalogManager::getCollection(
    OperationContext* txn, const std::string& collNs) {
    return retry(txn, [&] { return _actual->getCollection(txn, collNs); });
}

Status ForwardingCatalogManager::getCollections(OperationContext* txn,
                                                const std::string* dbName,
                                                std::vector<CollectionType>* collections,
                                                repl::OpTime* opTime) {
    invariant(collections->empty());
    return retry(txn,
                 [&] {
                     collections->clear();
                     return _actual->getCollections(txn, dbName, collections, opTime);
                 });
}

Status ForwardingCatalogManager::dropCollection(OperationContext* txn, const NamespaceString& ns) {
    return retry(txn, [&] { return _actual->dropCollection(txn, ns); });
}

Status ForwardingCatalogManager::getDatabasesForShard(OperationContext* txn,
                                                      const std::string& shardName,
                                                      std::vector<std::string>* dbs) {
    invariant(dbs->empty());
    return retry(txn,
                 [&] {
                     dbs->clear();
                     return _actual->getDatabasesForShard(txn, shardName, dbs);
                 });
}

Status ForwardingCatalogManager::getChunks(OperationContext* txn,
                                           const BSONObj& query,
                                           const BSONObj& sort,
                                           boost::optional<int> limit,
                                           std::vector<ChunkType>* chunks,
                                           repl::OpTime* opTime) {
    invariant(chunks->empty());
    return retry(txn,
                 [&] {
                     chunks->clear();
                     return _actual->getChunks(txn, query, sort, limit, chunks, opTime);
                 });
}

Status ForwardingCatalogManager::getTagsForCollection(OperationContext* txn,
                                                      const std::string& collectionNs,
                                                      std::vector<TagsType>* tags) {
    invariant(tags->empty());
    return retry(txn,
                 [&] {
                     tags->clear();
                     return _actual->getTagsForCollection(txn, collectionNs, tags);
                 });
}

StatusWith<std::string> ForwardingCatalogManager::getTagForChunk(OperationContext* txn,
                                                                 const std::string& collectionNs,
                                                                 const ChunkType& chunk) {
    return retry(txn, [&] { return _actual->getTagForChunk(txn, collectionNs, chunk); });
}

Status ForwardingCatalogManager::getAllShards(OperationContext* txn,
                                              std::vector<ShardType>* shards) {
    invariant(shards->empty());
    return retry(txn,
                 [&] {
                     shards->clear();
                     return _actual->getAllShards(txn, shards);
                 });
}

bool ForwardingCatalogManager::runUserManagementWriteCommand(OperationContext* txn,
                                                             const std::string& commandName,
                                                             const std::string& dbname,
                                                             const BSONObj& cmdObj,
                                                             BSONObjBuilder* result) {
    return retry(txn,
                 [&] {
                     BSONObjBuilder builder;
                     const bool success = _actual->runUserManagementWriteCommand(
                         txn, commandName, dbname, cmdObj, &builder);
                     result->appendElements(builder.done());
                     return success;
                 });
}

bool ForwardingCatalogManager::runReadCommand(OperationContext* txn,
                                              const std::string& dbname,
                                              const BSONObj& cmdObj,
                                              BSONObjBuilder* result) {
    return retry(txn,
                 [&] {
                     BSONObjBuilder builder;
                     const bool success = _actual->runReadCommand(txn, dbname, cmdObj, &builder);
                     result->appendElements(builder.done());
                     return success;
                 });
}

bool ForwardingCatalogManager::runUserManagementReadCommand(OperationContext* txn,
                                                            const std::string& dbname,
                                                            const BSONObj& cmdObj,
                                                            BSONObjBuilder* result) {
    return retry(txn,
                 [&] {
                     BSONObjBuilder builder;
                     const bool success =
                         _actual->runUserManagementReadCommand(txn, dbname, cmdObj, &builder);
                     result->appendElements(builder.done());
                     return success;
                 });
}

Status ForwardingCatalogManager::applyChunkOpsDeprecated(OperationContext* txn,
                                                         const BSONArray& updateOps,
                                                         const BSONArray& preCondition) {
    return retry(txn,
                 [&] { return _actual->applyChunkOpsDeprecated(txn, updateOps, preCondition); });
}

void ForwardingCatalogManager::logAction(OperationContext* txn, const ActionLogType& actionLog) {
    retry(txn,
          [&] {
              _actual->logAction(txn, actionLog);
              return 1;
          });
}

void ForwardingCatalogManager::logChange(OperationContext* txn,
                                         const std::string& clientAddress,
                                         const std::string& what,
                                         const std::string& ns,
                                         const BSONObj& detail) {
    retry(txn,
          [&] {
              _actual->logChange(txn, clientAddress, what, ns, detail);
              return 1;
          });
}

StatusWith<SettingsType> ForwardingCatalogManager::getGlobalSettings(OperationContext* txn,
                                                                     const std::string& key) {
    return retry(txn, [&] { return _actual->getGlobalSettings(txn, key); });
}

void ForwardingCatalogManager::writeConfigServerDirect(OperationContext* txn,
                                                       const BatchedCommandRequest& request,
                                                       BatchedCommandResponse* response) {
    retry(txn,
          [&] {
              BatchedCommandResponse theResponse;
              _actual->writeConfigServerDirect(txn, request, &theResponse);
              theResponse.cloneTo(response);
              return 1;
          });
}

Status ForwardingCatalogManager::createDatabase(OperationContext* txn, const std::string& dbName) {
    return retry(txn, [&] { return _actual->createDatabase(txn, dbName); });
}

DistLockManager* ForwardingCatalogManager::getDistLockManager() {
    warning() << "getDistLockManager called on ForwardingCatalogManager which should never happen "
                 "outside of unit tests!";
    stdx::lock_guard<stdx::mutex> lk(_observerMutex);
    return _actual->getDistLockManager();
}

Status ForwardingCatalogManager::initConfigVersion(OperationContext* txn) {
    return retry(txn, [&] { return _actual->initConfigVersion(txn); });
}

}  // namespace mongo
