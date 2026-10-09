// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"

#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/query/client_cursor/kill_cursors_gen.h"
#include "mongo/db/query/getmore_command_gen.h"
#include "mongo/db/shard_role/ddl/ddl_lock_manager.h"
#include "mongo/db/shard_role/ddl/list_collections_gen.h"
#include "mongo/db/shard_role/ddl/list_indexes_gen.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/executor/async_rpc.h"
#include "mongo/logv2/redaction.h"
#include "mongo/util/future_util.h"
#include "mongo/util/scopeguard.h"

#include <map>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {
using CollectionsByShard = std::map<ShardId, std::set<NamespaceString>>;

void throwIfIndexBuildInProgress(const AsyncRequestsSender::Response& cmdResponse,
                                 const NamespaceString& nss) {
    const auto status = AsyncRequestsSender::Response::getEffectiveStatus(cmdResponse);
    if (!status.isOK()) {
        if (status.code() == ErrorCodes::NamespaceNotFound ||
            status.code() == ErrorCodes::CommandNotSupportedOnView) {
            return;
        }
        uassertStatusOK(status);
    }

    const auto& replyData = cmdResponse.swResponse.getValue().data;
    const auto cursorObj = replyData["cursor"];
    const auto cursorId = cursorObj["id"].numberLong();
    if (cursorId != 0) {
        LOGV2_WARNING(11762400,
                      "listIndexes reply has unexhausted cursor; "
                      "index build check may be incomplete",
                      "nss"_attr = nss,
                      "shard"_attr = cmdResponse.shardId,
                      "cursorId"_attr = cursorId);
    }

    for (const auto& indexEntry : cursorObj["firstBatch"].Array()) {
        if (indexEntry["indexBuildInfo"].type() != BSONType::eoo) {
            uasserted(ErrorCodes::BackgroundOperationInProgressForNamespace,
                      str::stream() << "cannot perform operation: an index build is currently "
                                    << "running for collection " << nss.toStringForErrorMsg()
                                    << " on shard " << cmdResponse.shardId.toString());
        }
    }
}

void checkForInProgressIndexBuildsOnShards(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           const std::vector<ShardId>& shardIds,
                                           std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                           const CancellationToken& token) {
    if (shardIds.empty()) {
        return;
    }

    ListIndexes listIndexesCmd(nss);
    listIndexesCmd.setIncludeIndexBuildInfo(true);
    listIndexesCmd.setDbName(nss.dbName());
    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ListIndexes>>(
        **executor, token, listIndexesCmd);
    auto responses = sharding_ddl_util::sendAuthenticatedCommandToShards(
        opCtx, opts, shardIds, /*throwOnError=*/false);
    for (const auto& cmdResponse : responses) {
        throwIfIndexBuildInProgress(cmdResponse, nss);
    }
}

std::vector<ShardId> shardIdsHostingDatabase(OperationContext* opCtx, const DatabaseName& dbName) {
    if (dbName.isConfigDB()) {
        return {Grid::get(opCtx)->shardRegistry()->getConfigShard()->getId()};
    }
    return Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
}

void checkForInProgressIndexBuildsAcrossShards(
    OperationContext* opCtx,
    const std::set<NamespaceString>& nsses,
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) {
    if (nsses.empty()) {
        return;
    }

    for (const auto& nss : nsses) {
        checkForInProgressIndexBuildsOnShards(
            opCtx, nss, shardIdsHostingDatabase(opCtx, nss.dbName()), executor, token);
    }
}

void checkForInProgressIndexBuildsByShard(OperationContext* opCtx,
                                          const CollectionsByShard& nssByShard,
                                          std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                          const CancellationToken& token) {
    std::map<NamespaceString, std::vector<ShardId>> shardsByNss;
    for (const auto& [shardId, shardNsses] : nssByShard) {
        for (const auto& nss : shardNsses) {
            shardsByNss[nss].push_back(shardId);
        }
    }

    for (const auto& [nss, shardIds] : shardsByNss) {
        checkForInProgressIndexBuildsOnShards(opCtx, nss, shardIds, executor, token);
    }
}

void appendListedCollections(const BSONElement& batch,
                             const DatabaseName& dbName,
                             std::set<NamespaceString>& collections) {
    for (const auto& entry : batch.Array()) {
        if (entry["type"].str() == "view") {
            continue;
        }
        const auto name = entry["name"].valueStringData();
        if (name.empty()) {
            continue;
        }
        collections.insert(NamespaceStringUtil::deserialize(dbName, name));
    }
}

void killListCollectionsCursor(OperationContext* opCtx,
                               std::shared_ptr<executor::ScopedTaskExecutor> executor,
                               const CancellationToken& token,
                               const ShardId& shardId,
                               const DatabaseName& dbName,
                               long long cursorId) {
    try {
        KillCursorsCommandRequest killCmd(NamespaceString::makeListCollectionsNSS(dbName));
        killCmd.setDbName(dbName);
        killCmd.setCursorIds({static_cast<CursorId>(cursorId)});
        auto opts = std::make_shared<async_rpc::AsyncRPCOptions<KillCursorsCommandRequest>>(
            **executor, token, std::move(killCmd));
        sharding_ddl_util::sendAuthenticatedCommandToShards(
            opCtx, opts, {shardId}, /*throwOnError=*/false);
    } catch (const DBException& ex) {
        LOGV2_WARNING(11762402,
                      "Failed to kill listCollections cursor",
                      "db"_attr = dbName,
                      "shard"_attr = shardId,
                      "cursorId"_attr = cursorId,
                      "error"_attr = redact(ex));
    }
}

CollectionsByShard listCollectionNamespacesAcrossShards(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) {
    auto shardIds = shardIdsHostingDatabase(opCtx, dbName);

    ListCollections listCollectionsCmd;
    listCollectionsCmd.setDbName(dbName);
    listCollectionsCmd.setNameOnly(true);
    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ListCollections>>(
        **executor, token, listCollectionsCmd);
    auto responses = sharding_ddl_util::sendAuthenticatedCommandToShards(
        opCtx, opts, shardIds, /*throwOnError=*/false);

    const auto cursorNss = NamespaceString::makeListCollectionsNSS(dbName);
    CollectionsByShard collectionsByShard;
    for (const auto& cmdResponse : responses) {
        const auto status = AsyncRequestsSender::Response::getEffectiveStatus(cmdResponse);
        if (!status.isOK()) {
            if (status.code() == ErrorCodes::NamespaceNotFound) {
                continue;
            }
            uassertStatusOK(status);
        }

        const auto& replyData = cmdResponse.swResponse.getValue().data;
        const auto cursorObj = replyData["cursor"];
        long long cursorId = cursorObj["id"].numberLong();
        ON_BLOCK_EXIT([&] {
            if (cursorId != 0) {
                killListCollectionsCursor(
                    opCtx, executor, token, cmdResponse.shardId, dbName, cursorId);
            }
        });

        std::set<NamespaceString> collections;
        appendListedCollections(cursorObj["firstBatch"], dbName, collections);
        while (cursorId != 0) {
            GetMoreCommandRequest getMore(cursorId, std::string{cursorNss.coll()});
            getMore.setDbName(dbName);
            auto getMoreOpts = std::make_shared<async_rpc::AsyncRPCOptions<GetMoreCommandRequest>>(
                **executor, token, std::move(getMore));
            auto getMoreResponses = sharding_ddl_util::sendAuthenticatedCommandToShards(
                opCtx, getMoreOpts, {cmdResponse.shardId}, /*throwOnError=*/true);

            const auto& getMoreReply = getMoreResponses.front().swResponse.getValue().data;
            const auto getMoreCursor = getMoreReply["cursor"];
            cursorId = getMoreCursor["id"].numberLong();
            appendListedCollections(getMoreCursor["nextBatch"], dbName, collections);
        }
        if (!collections.empty()) {
            collectionsByShard.emplace(cmdResponse.shardId, std::move(collections));
        }
    }
    return collectionsByShard;
}
}  // namespace

ShardingDDLCoordinatorMixin::ShardingDDLCoordinatorMixin(const BSONObj& coorDoc)
    : _databaseVersion(extractShardingCoordinatorMetadata(coorDoc).getDatabaseVersion()) {}

const boost::optional<mongo::DatabaseVersion>& ShardingDDLCoordinatorMixin::getDatabaseVersion()
    const {
    return _databaseVersion;
}

void ShardingDDLCoordinatorMixin::_initializeLockerAndCheckAllowedToStart(ShardingCoordinator& self,
                                                                          OperationContext* opCtx) {
    tassert(10644521, "Expected _locker to be unset", !_locker);
    _locker = std::make_unique<Locker>(opCtx->getServiceContext());
    _locker->unsetThreadId();
    _locker->setDebugInfo(str::stream() << self._coordId.toBSON());

    // Check if this coordinator is allowed to start according to the user-writes blocking
    // critical section. If it is not the first execution, it means it had started already
    // and we are recovering this coordinator. In this case, let it be completed even though
    // new DDL operations may be prohibited now.
    // Coordinators that do not affect user data are allowed to start even when user writes
    // are blocked.
    if (self._firstExecution && !self.canAlwaysStartWhenUserWritesAreDisabled()) {
        self._getExternalState()->checkShardedDDLAllowedToStart(opCtx, self.originalNss());
    }
}

void ShardingDDLCoordinatorMixin::_checkDBVersion(ShardingCoordinator& self,
                                                  OperationContext* opCtx,
                                                  bool afterAcquiringLocks) {
    const auto& originalNss = self.originalNss();

    if (!originalNss.isConfigDB() && !originalNss.isAdminDB() && !self._recoveredFromDisk) {
        if (afterAcquiringLocks) {
            tassert(10644522,
                    "Expected databaseVersion to be set on the coordinator document metadata",
                    _databaseVersion);
        } else {
            uassert(ErrorCodes::IllegalOperation,
                    "Request sent without attaching database version",
                    _databaseVersion);
        }

        ScopedSetShardRole scopedSetShardRole(
            opCtx, originalNss, /*shardVersion=*/boost::none, _databaseVersion);

        self._getExternalState()->assertIsPrimaryShardForDb(opCtx, originalNss.dbName());
    };
}

std::set<NamespaceString> ShardingDDLCoordinatorMixin::_getAdditionalLocksToAcquire(
    OperationContext* opCtx) {
    return {};
}

std::set<NamespaceString> ShardingDDLCoordinatorMixin::_locksToAcquire(ShardingCoordinator& self,
                                                                       OperationContext* opCtx) {
    const NamespaceString& originalNss = self.originalNss();

    // Fetching all the locks that need to be acquired. Sort them by nss to avoid deadlocks.
    // If the requested nss represents a timeseries buckets namespace, translate it to its view
    // nss.
    std::set<NamespaceString> locksToAcquire;
    locksToAcquire.insert(originalNss.isTimeseriesBucketsCollection()
                              ? originalNss.getTimeseriesViewNamespace()
                              : originalNss);

    for (const auto& additionalLock : _getAdditionalLocksToAcquire(opCtx)) {
        locksToAcquire.insert(additionalLock.isTimeseriesBucketsCollection()
                                  ? additionalLock.getTimeseriesViewNamespace()
                                  : additionalLock);
    }

    return locksToAcquire;
}

ExecutorFuture<void> ShardingDDLCoordinatorMixin::_acquireAllDDLLocksAsync(
    ShardingCoordinator& self,
    OperationContext* opCtx,
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) {
    std::set<NamespaceString> locksToAcquire = _locksToAcquire(self, opCtx);

    // Acquiring all DDL locks in sorted order to avoid deadlocks
    // Note that the sorted order is provided by default through the std::set container
    auto futureChain = ExecutorFuture<void>(**executor);
    boost::optional<DatabaseName> lastDb;
    for (const auto& lockNss : locksToAcquire) {
        const bool isDbOnly = lockNss.coll().empty();

        // Acquiring the database DDL lock
        const auto normalizedDbName = [&] {
            if (self._coordId.getOperationType() != CoordinatorTypeEnum::kCreateDatabase) {
                // Already existing databases are not allowed to have their names differ just on
                // case. Uses the requested database name directly.
                return lockNss.dbName();
            }
            const auto dbNameStr =
                DatabaseNameUtil::serialize(lockNss.dbName(), SerializationContext::stateDefault());
            return DatabaseNameUtil::deserialize(
                boost::none, str::toLower(dbNameStr), SerializationContext::stateDefault());
        }();
        if (lastDb != normalizedDbName) {
            const auto lockMode = (isDbOnly ? MODE_X : MODE_IX);
            futureChain =
                std::move(futureChain).then([=, this, &self, anchor = self.shared_from_this()] {
                    return _acquireDDLLockAsync<DatabaseName>(
                        self, executor, token, normalizedDbName, lockMode);
                });
            lastDb = normalizedDbName;
        }

        // Acquiring the collection DDL lock
        if (!isDbOnly) {
            futureChain =
                std::move(futureChain)
                    .then([=, this, &self, nss = lockNss, anchor = self.shared_from_this()] {
                        return _acquireDDLLockAsync<NamespaceString>(
                            self, executor, token, nss, MODE_X);
                    });
        }
    }
    return futureChain;
}

void ShardingDDLCoordinatorMixin::_releaseDDLLocks(OperationContext* opCtx) {
    while (!_scopedLocks.empty()) {
        _scopedLocks.pop();
    }
}

void ShardingDDLCoordinatorMixin::_checkIndexBuildsInProgressGeneric(
    ShardingCoordinator& self,
    OperationContext* opCtx,
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) {
    // Collection namespaces are checked on every shard. Database namespaces are expanded with
    // listCollections, and each resulting collection is checked only on the shards that reported
    // it, so a database DDL does not fan listIndexes out to shards that do not have the collection.
    std::set<NamespaceString> nssesOnAllShards;
    std::map<ShardId, std::set<NamespaceString>> nssByShard;
    for (const auto& nss : _conflictingIndexBuildNamespaces(self, opCtx)) {
        if (!nss.isDbOnly()) {
            nssesOnAllShards.insert(nss);
            continue;
        }

        for (auto&& [shardId, collNsses] :
             listCollectionNamespacesAcrossShards(opCtx, nss.dbName(), executor, token)) {
            nssByShard[shardId].insert(collNsses.begin(), collNsses.end());
        }
    }

    for (auto&& [_, collNsses] : nssByShard) {
        for (auto&& nss : collNsses) {
            nssesOnAllShards.erase(nss);
        }
    }

    checkForInProgressIndexBuildsAcrossShards(opCtx, nssesOnAllShards, executor, token);
    checkForInProgressIndexBuildsByShard(opCtx, nssByShard, executor, token);
}

template <typename T>
ExecutorFuture<void> ShardingDDLCoordinatorMixin::_acquireDDLLockAsync(
    ShardingCoordinator& self,
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token,
    const T& resource,
    LockMode lockMode) {
    return AsyncTry([this, &self, resource, lockMode] {
               auto opCtxHolder = self.makeOperationContext(/*deprioritizable=*/true);
               auto* opCtx = opCtxHolder.get();

               const auto coorName = idl::serialize(self._coordId.getOperationType());

               _scopedLocks.emplace(DDLLockManager::ScopedBaseDDLLock{opCtx,
                                                                      _locker.get(),
                                                                      resource,
                                                                      coorName,
                                                                      lockMode,
                                                                      false /* waitForRecovery */});
           })
        .until([this, &self, resource, lockMode](Status status) {
            if (!status.isOK()) {
                LOGV2_WARNING(6819300,
                              "DDL lock acquisition attempt failed",
                              logv2::DynamicAttributes{
                                  logv2::DynamicAttributes(self.getCoordinatorLogAttrs()),
                                  "resource"_attr = toStringForLogging(resource),
                                  "lockMode"_attr = modeName(lockMode),
                                  "error"_attr = redact(status)});
            }
            // Coordinators can't generally be rolled back so in case we recovered a coordinator
            // from disk we need to ensure eventual completion of the operation, so we must
            // retry until we manage to acquire the lock.
            return (!self._recoveredFromDisk) || status.isOK();
        })
        .withBackoffBetweenIterations(ShardingCoordinator::kExponentialBackoff)
        .on(**executor, token);
}

}  // namespace mongo
