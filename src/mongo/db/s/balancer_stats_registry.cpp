/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include "mongo/db/s/balancer_stats_registry.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/db/s/range_deletion_util.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/s/sharding_feature_flags_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

const auto balancerStatsRegistryDecorator =
    ServiceContext::declareDecoration<BalancerStatsRegistry>();

/**
 * Constructs the default options for the private thread pool used for asyncronous initialization
 */
ThreadPool::Options makeDefaultThreadPoolOptions() {
    ThreadPool::Options options;
    options.poolName = "BalancerStatsRegistry";
    options.minThreads = 0;
    options.maxThreads = 1;
    return options;
}
}  // namespace

ScopedRangeDeleterLock::ScopedRangeDeleterLock(OperationContext* opCtx)
    // TODO SERVER-62491 Use system tenantId for DBLock
    : _configLock(opCtx, DatabaseName(boost::none, NamespaceString::kConfigDb), MODE_IX),
      _rangeDeletionLock(opCtx, NamespaceString::kRangeDeletionNamespace, MODE_X) {}

// Take DB and Collection lock in mode IX as well as collection UUID lock to serialize with
// operations that take the above version of the ScopedRangeDeleterLock such as FCV downgrade and
// BalancerStatsRegistry initialization.
ScopedRangeDeleterLock::ScopedRangeDeleterLock(OperationContext* opCtx, const UUID& collectionUuid)
    // TODO SERVER-62491 Use system tenantId for DBLock
    : _configLock(opCtx, DatabaseName(boost::none, NamespaceString::kConfigDb), MODE_IX),
      _rangeDeletionLock(opCtx, NamespaceString::kRangeDeletionNamespace, MODE_IX),
      _collectionUuidLock(Lock::ResourceLock(
          opCtx->lockState(),
          ResourceId(RESOURCE_MUTEX, "RangeDeleterCollLock::" + collectionUuid.toString()),
          MODE_X)) {}

const ReplicaSetAwareServiceRegistry::Registerer<BalancerStatsRegistry>
    balancerStatsRegistryRegisterer("BalancerStatsRegistry");


BalancerStatsRegistry* BalancerStatsRegistry::get(ServiceContext* serviceContext) {
    return &balancerStatsRegistryDecorator(serviceContext);
}

BalancerStatsRegistry* BalancerStatsRegistry::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

void BalancerStatsRegistry::onStartup(OperationContext* opCtx) {
    _threadPool = std::make_shared<ThreadPool>(makeDefaultThreadPoolOptions());
    _threadPool->startup();
}

void BalancerStatsRegistry::onStepUpComplete(OperationContext* opCtx, long long term) {
    dassert(_state.load() == State::kSecondary);
    _state.store(State::kPrimaryIdle);

    if (!feature_flags::gOrphanTracking.isEnabled(serverGlobalParams.featureCompatibility)) {
        // If future flag is disabled do not initialize the registry
        return;
    }
    initializeAsync(opCtx);
}

void BalancerStatsRegistry::initializeAsync(OperationContext* opCtx) {
    ExecutorFuture<void>(_threadPool)
        .then([this] {
            ThreadClient tc("BalancerStatsRegistry::asynchronousInitialization",
                            getGlobalServiceContext());

            {
                stdx::lock_guard lk{_stateMutex};
                if (const auto currentState = _state.load(); currentState != State::kPrimaryIdle) {
                    LOGV2_DEBUG(6419631,
                                2,
                                "Abandoning BalancerStatsRegistry initialization",
                                "currentState"_attr = currentState);
                    return;
                }
                _state.store(State::kInitializing);
                _initOpCtxHolder = tc->makeOperationContext();
            }

            ON_BLOCK_EXIT([this] {
                stdx::lock_guard lk{_stateMutex};
                _initOpCtxHolder.reset();
            });

            auto opCtx{_initOpCtxHolder.get()};

            LOGV2_DEBUG(6419601, 2, "Initializing BalancerStatsRegistry");
            try {
                // Lock the range deleter to prevent
                // concurrent modifications of orphans count
                ScopedRangeDeleterLock rangeDeleterLock(opCtx);
                // Load current ophans count from disk
                _loadOrphansCount(opCtx);
                LOGV2_DEBUG(6419602, 2, "Completed BalancerStatsRegistry initialization");

                // Start accepting updates to the cached orphan count
                auto expectedState = State::kInitializing;
                _state.compareAndSwap(&expectedState, State::kInitialized);
                // Unlock the range deleter (~ScopedRangeDeleterLock)
            } catch (const DBException& ex) {
                LOGV2_ERROR(6419600,
                            "Failed to initialize BalancerStatsRegistry after stepUp",
                            "error"_attr = redact(ex));
            }
        })
        .getAsync([](auto) {});
}

void BalancerStatsRegistry::terminate() {
    {
        stdx::lock_guard lk{_stateMutex};
        _state.store(State::kTerminating);
        if (_initOpCtxHolder) {
            stdx::lock_guard<Client> lk(*_initOpCtxHolder->getClient());
            _initOpCtxHolder->markKilled(ErrorCodes::Interrupted);
        }
    }

    // Wait for the  asynchronous initialization to complete
    _threadPool->waitForIdle();

    {
        // Clear the stats
        stdx::lock_guard lk{_mutex};
        _collStatsMap.clear();
    }

    dassert(_state.load() == State::kTerminating);
    _state.store(State::kPrimaryIdle);
    LOGV2_DEBUG(6419603, 2, "BalancerStatsRegistry terminated");
}

void BalancerStatsRegistry::onStepDown() {
    // TODO SERVER-65817 inline the terminate() function
    terminate();
    _state.store(State::kSecondary);
}

long long BalancerStatsRegistry::getCollNumOrphanDocs(const UUID& collectionUUID) const {
    if (!_isInitialized())
        uasserted(ErrorCodes::NotYetInitialized, "BalancerStatsRegistry is not initialized");

    stdx::lock_guard lk{_mutex};
    auto collStats = _collStatsMap.find(collectionUUID);
    if (collStats != _collStatsMap.end()) {
        return collStats->second.numOrphanDocs;
    }
    return 0;
}

long long BalancerStatsRegistry::getCollNumOrphanDocsFromDiskIfNeeded(
    OperationContext* opCtx, const UUID& collectionUUID) const {
    try {
        return getCollNumOrphanDocs(collectionUUID);
    } catch (const ExceptionFor<ErrorCodes::NotYetInitialized>&) {
        // Since the registry is not initialized, run an aggregation to get the number of orphans
        DBDirectClient client(opCtx);
        std::vector<BSONObj> pipeline;
        pipeline.push_back(
            BSON("$match" << BSON(RangeDeletionTask::kCollectionUuidFieldName << collectionUUID)));
        pipeline.push_back(
            BSON("$group" << BSON("_id"
                                  << "numOrphans"
                                  << "count"
                                  << BSON("$sum"
                                          << "$" + RangeDeletionTask::kNumOrphanDocsFieldName))));
        AggregateCommandRequest aggRequest(NamespaceString::kRangeDeletionNamespace, pipeline);
        auto swCursor = DBClientCursor::fromAggregationRequest(
            &client, aggRequest, false /* secondaryOk */, true /* useExhaust */);
        if (!swCursor.isOK()) {
            return 0;
        }
        auto cursor = std::move(swCursor.getValue());
        if (!cursor->more()) {
            return 0;
        }
        auto res = cursor->nextSafe();
        invariant(!cursor->more());
        auto numOrphans = res.getField("count");
        invariant(numOrphans);
        return numOrphans.exactNumberLong();
    }
}


void BalancerStatsRegistry::onRangeDeletionTaskInsertion(const UUID& collectionUUID,
                                                         long long numOrphanDocs) {
    if (!_isInitialized())
        return;

    stdx::lock_guard lk{_mutex};
    auto& stats = _collStatsMap[collectionUUID];
    stats.numRangeDeletionTasks += 1;
    stats.numOrphanDocs += numOrphanDocs;
}

void BalancerStatsRegistry::onRangeDeletionTaskDeletion(const UUID& collectionUUID,
                                                        long long numOrphanDocs) {
    if (!_isInitialized())
        return;

    stdx::lock_guard lk{_mutex};
    auto collStatsIt = _collStatsMap.find(collectionUUID);
    if (collStatsIt == _collStatsMap.end()) {
        LOGV2_ERROR(6419612,
                    "Couldn't find cached range deletion tasks count during decrese attempt",
                    "collectionUUID"_attr = collectionUUID,
                    "numOrphanDocs"_attr = numOrphanDocs);
        return;
    }
    auto& stats = collStatsIt->second;

    stats.numRangeDeletionTasks -= 1;
    stats.numOrphanDocs -= numOrphanDocs;

    if (stats.numRangeDeletionTasks <= 0) {
        if (MONGO_unlikely(stats.numRangeDeletionTasks < 0)) {
            LOGV2_ERROR(6419613,
                        "Cached count of range deletion tasks became negative. Resetting it to 0",
                        "collectionUUID"_attr = collectionUUID,
                        "numRangeDeletionTasks"_attr = stats.numRangeDeletionTasks,
                        "numOrphanDocs"_attr = stats.numRangeDeletionTasks);
        }
        _collStatsMap.erase(collStatsIt);
    }
}

void BalancerStatsRegistry::updateOrphansCount(const UUID& collectionUUID, long long delta) {
    if (!_isInitialized() || delta == 0)
        return;

    stdx::lock_guard lk{_mutex};
    if (delta > 0) {
        // Increase or create the stats if missing
        _collStatsMap[collectionUUID].numOrphanDocs += delta;
    } else {
        // Decrease orphan docs count
        auto collStatsIt = _collStatsMap.find(collectionUUID);
        if (collStatsIt == _collStatsMap.end()) {
            // This should happen only in case of direct manipulation of range deletion tasks
            // documents or direct writes into orphaned ranges
            LOGV2_ERROR(6419610,
                        "Couldn't find cached orphan documents count during decrese attempt",
                        "collectionUUID"_attr = collectionUUID,
                        "delta"_attr = delta);
            return;
        }
        auto& stats = collStatsIt->second;

        stats.numOrphanDocs += delta;

        if (stats.numOrphanDocs < 0) {
            // This should happen only in case of direct manipulation of range deletion tasks
            // documents or direct writes into orphaned ranges
            LOGV2_ERROR(6419611,
                        "Cached orphan documents count became negative, resetting it to 0",
                        "collectionUUID"_attr = collectionUUID,
                        "numOrphanDocs"_attr = stats.numOrphanDocs,
                        "delta"_attr = delta,
                        "numRangeDeletionTasks"_attr = stats.numRangeDeletionTasks);
            stats.numOrphanDocs = 0;
        }
    }
}


void BalancerStatsRegistry::_loadOrphansCount(OperationContext* opCtx) {
    static constexpr auto kNumOrphanDocsLabel = "numOrphanDocs"_sd;
    static constexpr auto kNumRangeDeletionTasksLabel = "numRangeDeletionTasks"_sd;

    /*
     * {
     * 	$group: {
     * 		_id: $collectionUUID,
     * 		numOrphanDocs: {$sum: $numOrphanDocs},
     * 		numRangeDeletionTasks: {$count: {}},
     * 		}
     * 	}
     */
    static const BSONObj groupStage{
        BSON("$group" << BSON("_id"
                              << "$" + RangeDeletionTask::kCollectionUuidFieldName
                              << kNumOrphanDocsLabel
                              << BSON("$sum"
                                      << "$" + RangeDeletionTask::kNumOrphanDocsFieldName)
                              << kNumRangeDeletionTasksLabel << BSON("$count" << BSONObj())))};
    AggregateCommandRequest aggRequest{NamespaceString::kRangeDeletionNamespace, {groupStage}};

    DBDirectClient client{opCtx};
    auto cursor = uassertStatusOK(DBClientCursor::fromAggregationRequest(
        &client, std::move(aggRequest), false /* secondaryOk */, true /* useExhaust */));

    {
        stdx::lock_guard lk{_mutex};
        _collStatsMap.clear();
        while (cursor->more()) {
            auto collObj = cursor->next();
            auto collUUID = uassertStatusOK(UUID::parse(collObj["_id"]));
            auto orphanCount = collObj[kNumOrphanDocsLabel].exactNumberLong();
            auto numRangeDeletionTasks = collObj[kNumRangeDeletionTasksLabel].exactNumberLong();
            invariant(numRangeDeletionTasks > 0);
            if (orphanCount < 0) {
                LOGV2_ERROR(6419621,
                            "Found negative orphan count in range deletion task documents",
                            "collectionUUID"_attr = collUUID,
                            "numOrphanDocs"_attr = orphanCount,
                            "numRangeDeletionTasks"_attr = numRangeDeletionTasks);
                orphanCount = 0;
            }
            _collStatsMap.emplace(collUUID, CollectionStats{orphanCount, numRangeDeletionTasks});
        }
        LOGV2_DEBUG(6419604,
                    2,
                    "Populated BalancerStatsRegistry cache",
                    "numCollections"_attr = _collStatsMap.size());
    }
}

}  // namespace mongo
