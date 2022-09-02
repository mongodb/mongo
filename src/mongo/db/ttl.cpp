/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include "mongo/db/ttl.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/catalog/coll_mod.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/fsync_locked.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/delete_stage.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/tenant_migration_access_blocker_registry.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/resource_consumption_metrics.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/bucket_catalog.h"
#include "mongo/db/ttl_collection_cache.h"
#include "mongo/db/ttl_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/background.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/log_with_sampling.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex


namespace mongo {

namespace {
const auto getTTLMonitor = ServiceContext::declareDecoration<std::unique_ptr<TTLMonitor>>();

bool isBatchingEnabled() {
    return feature_flags::gBatchMultiDeletes.isEnabled(serverGlobalParams.featureCompatibility) &&
        ttlMonitorBatchDeletes.load();
}

// When batching is enabled, returns BatchedDeleteStageParams that limit the amount of work done in
// a delete such that it is possible not all expired documents will be removed. Returns nullptr
// otherwise.
//
// When batching is disabled, all expired documents are removed by the delete operation.
std::unique_ptr<BatchedDeleteStageParams> getBatchedDeleteStageParams(bool batchingEnabled) {
    if (!batchingEnabled) {
        return nullptr;
    }

    auto batchedDeleteParams = std::make_unique<BatchedDeleteStageParams>();
    batchedDeleteParams->targetPassDocs = ttlIndexDeleteTargetDocs.load();
    batchedDeleteParams->targetPassTimeMS = Milliseconds(ttlIndexDeleteTargetTimeMS.load());
    return batchedDeleteParams;
}

// Generates an expiration date based on the user-configured expireAfterSeconds. Includes special
// 'safe' handling for time-series collections.
Date_t safeExpirationDate(OperationContext* opCtx,
                          const CollectionPtr& coll,
                          std::int64_t expireAfterSeconds) {
    if (auto timeseries = coll->getTimeseriesOptions()) {
        const auto bucketMaxSpan = Seconds(*timeseries->getBucketMaxSpanSeconds());

        // Don't delete data unless it is safely out of range of the bucket maximum time
        // range. On time-series collections, the _id (and thus RecordId) is the minimum
        // time value of a bucket. A bucket may have newer data, so we cannot safely delete
        // the entire bucket yet until the maximum bucket range has passed, even if the
        // minimum value can be expired.
        return Date_t::now() - Seconds(expireAfterSeconds) - bucketMaxSpan;
    }

    return Date_t::now() - Seconds(expireAfterSeconds);
}

//  Computes and returns the start 'RecordIdBound' with the correct type for a bounded, clustered
//  collection scan. All time-series buckets collections delete entries of type 'ObjectId'. All
//  other collections must only delete entries of type 'Date'.
RecordIdBound makeCollScanStartBound(const CollectionPtr& collection, const Date_t startDate) {
    if (collection->getTimeseriesOptions()) {
        auto startOID = OID();
        startOID.init(startDate, false /* max */);
        return RecordIdBound(record_id_helpers::keyForOID(startOID));
    }

    return RecordIdBound(record_id_helpers::keyForDate(startDate));
}

//  Computes and returns the end 'RecordIdBound' with the correct type for a bounded, clustered
//  collection scan. All time-series buckets collections delete entries of type 'ObjectId'. All
//  other collections must only delete entries of type 'Date'.
RecordIdBound makeCollScanEndBound(const CollectionPtr& collection, Date_t expirationDate) {
    if (collection->getTimeseriesOptions()) {
        auto endOID = OID();
        endOID.init(expirationDate, true /* max */);
        return RecordIdBound(record_id_helpers::keyForOID(endOID));
    }

    return RecordIdBound(record_id_helpers::keyForDate(expirationDate));
}

const IndexDescriptor* getValidTTLIndex(OperationContext* opCtx,
                                        TTLCollectionCache* ttlCollectionCache,
                                        const CollectionPtr& collection,
                                        const BSONObj& spec,
                                        std::string indexName) {
    if (!spec.hasField(IndexDescriptor::kExpireAfterSecondsFieldName)) {
        ttlCollectionCache->deregisterTTLIndexByName(collection->uuid(), indexName);
        return nullptr;
    }

    if (!collection->isIndexReady(indexName)) {
        return nullptr;
    }

    const BSONObj key = spec["key"].Obj();
    if (key.nFields() != 1) {
        LOGV2_ERROR(22540,
                    "key for ttl index can only have 1 field, skipping TTL job",
                    "index"_attr = spec);
        return nullptr;
    }

    const IndexDescriptor* desc = collection->getIndexCatalog()->findIndexByName(opCtx, indexName);

    if (!desc) {
        LOGV2_DEBUG(22535, 1, "index not found; skipping ttl job", "index"_attr = spec);
        return nullptr;
    }

    if (IndexType::INDEX_BTREE != IndexNames::nameToType(desc->getAccessMethodName())) {
        LOGV2_ERROR(22541,
                    "special index can't be used as a TTL index, skipping TTL job",
                    "index"_attr = spec);
        return nullptr;
    }

    BSONElement secondsExpireElt = spec[IndexDescriptor::kExpireAfterSecondsFieldName];
    if (!secondsExpireElt.isNumber() || secondsExpireElt.isNaN()) {
        LOGV2_ERROR(
            22542,
            "TTL indexes require the expire field to be numeric and not a NaN, skipping TTL job",
            "ns"_attr = collection->ns(),
            "uuid"_attr = collection->uuid(),
            "field"_attr = IndexDescriptor::kExpireAfterSecondsFieldName,
            "type"_attr = typeName(secondsExpireElt.type()),
            "index"_attr = spec);
        return nullptr;
    }
    return desc;
}

/**
 * Runs on primaries and secondaries. Forwards replica set events to the TTLMonitor.
 */
class TTLMonitorService : public ReplicaSetAwareService<TTLMonitorService> {
public:
    static TTLMonitorService* get(ServiceContext* serviceContext);
    TTLMonitorService() = default;

private:
    void onStartup(OperationContext* opCtx) override {}
    void onInitialDataAvailable(OperationContext* opCtx, bool isMajorityDataAvailable) override {}
    void onShutdown() override {}
    void onStepUpBegin(OperationContext* opCtx, long long term) override {}
    void onStepUpComplete(OperationContext* opCtx, long long term) override {
        auto ttlMonitor = TTLMonitor::get(opCtx->getServiceContext());
        if (!ttlMonitor) {
            // Some test fixtures might not install the TTLMonitor.
            return;
        }
        ttlMonitor->onStepUp(opCtx);
    }
    void onStepDown() override {}
    void onBecomeArbiter() override {}
};

const auto _ttlMonitorService = ServiceContext::declareDecoration<TTLMonitorService>();

const ReplicaSetAwareServiceRegistry::Registerer<TTLMonitorService> _ttlMonitorServiceRegisterer(
    "TTLMonitorService");

// static
TTLMonitorService* TTLMonitorService::get(ServiceContext* serviceContext) {
    return &_ttlMonitorService(serviceContext);
}

}  // namespace

MONGO_FAIL_POINT_DEFINE(hangTTLMonitorWithLock);
MONGO_FAIL_POINT_DEFINE(hangTTLMonitorBetweenPasses);

// A TTL pass completes when there are no more expired documents to remove. A single TTL pass may
// consist of multiple sub-passes. Each sub-pass deletes all the expired documents it can up to
// 'ttlSubPassTargetSecs'. It is possible for a sub-pass to complete before all expired documents
// have been removed.
CounterMetric ttlPasses("ttl.passes");
CounterMetric ttlSubPasses("ttl.subPasses");
CounterMetric ttlDeletedDocuments("ttl.deletedDocuments");

using MtabType = TenantMigrationAccessBlocker::BlockerType;

TTLMonitor* TTLMonitor::get(ServiceContext* serviceCtx) {
    return getTTLMonitor(serviceCtx).get();
}

void TTLMonitor::set(ServiceContext* serviceCtx, std::unique_ptr<TTLMonitor> monitor) {
    auto& ttlMonitor = getTTLMonitor(serviceCtx);
    if (ttlMonitor) {
        invariant(!ttlMonitor->running(),
                  "Tried to reset the TTLMonitor without shutting down the original instance.");
    }

    invariant(monitor);
    ttlMonitor = std::move(monitor);
}

void TTLMonitor::run() {
    ThreadClient tc(name(), getGlobalServiceContext());
    AuthorizationSession::get(cc())->grantInternalAuthorization(&cc());

    {
        stdx::lock_guard<Client> lk(*tc.get());
        tc.get()->setSystemOperationKillableByStepdown(lk);
    }

    while (true) {
        {
            // Wait until either ttlMonitorSleepSecs passes or a shutdown is requested.
            auto deadline = Date_t::now() + Seconds(ttlMonitorSleepSecs.load());
            stdx::unique_lock<Latch> lk(_stateMutex);

            MONGO_IDLE_THREAD_BLOCK;
            _shuttingDownCV.wait_until(
                lk, deadline.toSystemTimePoint(), [&] { return _shuttingDown; });

            if (_shuttingDown) {
                return;
            }
        }

        LOGV2_DEBUG(22528, 3, "thread awake");

        if (!ttlMonitorEnabled.load()) {
            LOGV2_DEBUG(22529, 1, "disabled");
            continue;
        }

        if (lockedForWriting()) {
            // Note: this is not perfect as you can go into fsync+lock between this and actually
            // doing the delete later.
            LOGV2_DEBUG(22530, 3, "locked for writing");
            continue;
        }

        try {
            _doTTLPass();
        } catch (const WriteConflictException&) {
            LOGV2_DEBUG(22531, 1, "got WriteConflictException");
        } catch (const DBException& ex) {
            LOGV2_WARNING(22537,
                          "TTLMonitor was interrupted, waiting before doing another pass",
                          "interruption"_attr = ex,
                          "wait"_attr = Milliseconds(Seconds(ttlMonitorSleepSecs.load())));
        }
    }
}

void TTLMonitor::shutdown() {
    LOGV2(3684100, "Shutting down TTL collection monitor thread");
    {
        stdx::lock_guard<Latch> lk(_stateMutex);
        _shuttingDown = true;
        _shuttingDownCV.notify_one();
    }
    wait();
    LOGV2(3684101, "Finished shutting down TTL collection monitor thread");
}

void TTLMonitor::_doTTLPass() {
    const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
    OperationContext* opCtx = opCtxPtr.get();

    hangTTLMonitorBetweenPasses.pauseWhileSet(opCtx);

    // Increment the metric after the TTL work has been finished.
    ON_BLOCK_EXIT([&] { ttlPasses.increment(); });

    bool moreToDelete = true;
    while (moreToDelete) {
        // Sub-passes may not delete all documents in the interest of fairness. If a sub-pass
        // indicates that it did not delete everything possible, we continue performing sub-passes.
        // This maintains the semantic that a full TTL pass deletes everything it possibly can
        // before sleeping periodically.
        moreToDelete = _doTTLSubPass(opCtx);
    }
}

bool TTLMonitor::_doTTLSubPass(OperationContext* opCtx) {
    // If part of replSet but not in a readable state (e.g. during initial sync), skip.
    if (repl::ReplicationCoordinator::get(opCtx)->getReplicationMode() ==
            repl::ReplicationCoordinator::modeReplSet &&
        !repl::ReplicationCoordinator::get(opCtx)->getMemberState().readable())
        return false;

    ON_BLOCK_EXIT([&] { ttlSubPasses.increment(); });

    TTLCollectionCache& ttlCollectionCache = TTLCollectionCache::get(getGlobalServiceContext());

    // Refresh view of current TTL indexes - prevents starvation if a new TTL index is introduced
    // during a long running pass.
    TTLCollectionCache::InfoMap work = ttlCollectionCache.getTTLInfos();

    // When batching is enabled, _doTTLIndexDelete will limit the amount of work it
    // performs in both time and the number of documents it deletes. If it reaches one
    // of these limits on an index, it will return moreToDelete as true, and we will
    // re-visit it, but only after passing through every other TTL index. We repeat this
    // process until we hit the ttlMonitorSubPassTargetSecs time limit.
    //
    // When batching is disabled, _doTTLIndexDelete will delete as many documents as
    // possible without limit.
    Timer timer;
    do {
        TTLCollectionCache::InfoMap moreWork;
        for (const auto& [uuid, infos] : work) {
            for (const auto& info : infos) {
                bool moreToDelete = _doTTLIndexDelete(opCtx, &ttlCollectionCache, uuid, info);
                if (moreToDelete) {
                    moreWork[uuid].push_back(info);
                }
            }
        }

        work = moreWork;
    } while (!work.empty() &&
             Seconds(timer.seconds()) < Seconds(ttlMonitorSubPassTargetSecs.load()));

    // More work signals there may more expired documents to visit.
    return !work.empty();
}

bool TTLMonitor::_doTTLIndexDelete(OperationContext* opCtx,
                                   TTLCollectionCache* ttlCollectionCache,
                                   const UUID& uuid,
                                   const TTLCollectionCache::Info& info) {
    // Skip collections that have not been made visible yet. The TTLCollectionCache
    // already has the index information available, so we want to avoid removing it
    // until the collection is visible.
    auto collectionCatalog = CollectionCatalog::get(opCtx);
    if (collectionCatalog->isCollectionAwaitingVisibility(uuid)) {
        return false;
    }

    // The collection was dropped.
    auto nss = collectionCatalog->lookupNSSByUUID(opCtx, uuid);
    if (!nss) {
        if (info.isClustered()) {
            ttlCollectionCache->deregisterTTLClusteredIndex(uuid);
        } else {
            ttlCollectionCache->deregisterTTLIndexByName(uuid, info.getIndexName());
        }
        return false;
    }

    if (nss->isTemporaryReshardingCollection() || nss->isDropPendingNamespace()) {
        // For resharding, the donor shard primary is responsible for performing the TTL
        // deletions.
        return false;
    }

    try {
        uassertStatusOK(userAllowedWriteNS(opCtx, *nss));

        // Attach IGNORED shard version to skip orphans (the range deleter will clear them up)
        auto scopedRole = ScopedSetShardRole(opCtx, *nss, ShardVersion::IGNORED(), boost::none);
        AutoGetCollection coll(opCtx, *nss, MODE_IX);
        // The collection with `uuid` might be renamed before the lock and the wrong namespace would
        // be locked and looked up so we double check here.
        if (!coll || coll->uuid() != uuid)
            return false;

        // Allow TTL deletion on non-capped collections, and on capped clustered collections.
        invariant(!coll->isCapped() || (coll->isCapped() && coll->isClustered()));

        if (MONGO_unlikely(hangTTLMonitorWithLock.shouldFail())) {
            LOGV2(22534,
                  "Hanging due to hangTTLMonitorWithLock fail point",
                  "ttlPasses"_attr = ttlPasses.get());
            hangTTLMonitorWithLock.pauseWhileSet(opCtx);
        }

        if (!repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, *nss)) {
            return false;
        }

        std::shared_ptr<TenantMigrationAccessBlocker> mtab;
        if (coll.getDb() &&
            nullptr !=
                (mtab = TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                            .getTenantMigrationAccessBlockerForDbName(
                                coll.getDb()->name().toString(), MtabType::kRecipient)) &&
            mtab->checkIfShouldBlockTTL()) {
            LOGV2_DEBUG(53768,
                        1,
                        "Postpone TTL of DB because of active tenant migration",
                        "tenantMigrationAccessBlocker"_attr = mtab->getDebugInfo().jsonString(),
                        "database"_attr = coll.getDb()->name().toString());
            return false;
        }

        ResourceConsumption::ScopedMetricsCollector scopedMetrics(opCtx, nss->db().toString());

        const auto& collection = coll.getCollection();
        if (info.isClustered()) {
            return _deleteExpiredWithCollscan(opCtx, ttlCollectionCache, collection);
        } else {
            return _deleteExpiredWithIndex(
                opCtx, ttlCollectionCache, collection, info.getIndexName());
        }
    } catch (const ExceptionForCat<ErrorCategory::StaleShardVersionError>& ex) {
        // The TTL index tried to delete some information from a sharded collection
        // through a direct operation against the shard but the filtering metadata was
        // not available.
        //
        // The current TTL task cannot be completed. However, if the critical section is
        // not held the code below will fire an asynchronous refresh, hoping that the
        // next time this task is re-executed the filtering information is already
        // present.
        if (auto staleInfo = ex.extraInfo<StaleConfigInfo>();
            staleInfo && !staleInfo->getCriticalSectionSignal()) {
            auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
            ExecutorFuture<void>(executor)
                .then([serviceContext = opCtx->getServiceContext(), nss, staleInfo] {
                    ThreadClient tc("TTLShardVersionRecovery", serviceContext);
                    {
                        stdx::lock_guard<Client> lk(*tc.get());
                        tc->setSystemOperationKillableByStepdown(lk);
                    }

                    auto uniqueOpCtx = tc->makeOperationContext();
                    auto opCtx = uniqueOpCtx.get();

                    onShardVersionMismatchNoExcept(opCtx, *nss, staleInfo->getVersionWanted())
                        .ignore();
                })
                .getAsync([](auto) {});
        }
        LOGV2_WARNING(6353000,
                      "Error running TTL job on collection: the shard should refresh "
                      "before being able to complete this task",
                      logAttrs(*nss),
                      "error"_attr = ex);
        return false;
    } catch (const DBException& ex) {
        if (opCtx->isKillPending()) {
            // The exception is relevant to the entire TTL monitoring process, not just the specific
            // TTL index. Let the exception escape so it can be addressed at the higher monitoring
            // layer.
            throw;
        }

        LOGV2_ERROR(
            5400703, "Error running TTL job on collection", logAttrs(*nss), "error"_attr = ex);
        return false;
    }
}

bool TTLMonitor::_deleteExpiredWithIndex(OperationContext* opCtx,
                                         TTLCollectionCache* ttlCollectionCache,
                                         const CollectionPtr& collection,
                                         std::string indexName) {
    if (!collection->isIndexPresent(indexName)) {
        ttlCollectionCache->deregisterTTLIndexByName(collection->uuid(), indexName);
        return false;
    }

    BSONObj spec = collection->getIndexSpec(indexName);
    const IndexDescriptor* desc =
        getValidTTLIndex(opCtx, ttlCollectionCache, collection, spec, indexName);

    if (!desc) {
        return false;
    }

    LOGV2_DEBUG(22533,
                1,
                "running TTL job for index",
                logAttrs(collection->ns()),
                "key"_attr = desc->keyPattern(),
                "name"_attr = indexName);

    auto expireAfterSeconds = spec[IndexDescriptor::kExpireAfterSecondsFieldName].safeNumberLong();
    const Date_t kDawnOfTime = Date_t::fromMillisSinceEpoch(std::numeric_limits<long long>::min());
    const auto expirationDate = safeExpirationDate(opCtx, collection, expireAfterSeconds);
    const BSONObj startKey = BSON("" << kDawnOfTime);
    const BSONObj endKey = BSON("" << expirationDate);

    auto key = desc->keyPattern();
    // The canonical check as to whether a key pattern element is "ascending" or
    // "descending" is (elt.number() >= 0).  This is defined by the Ordering class.
    const InternalPlanner::Direction direction = (key.firstElement().number() >= 0)
        ? InternalPlanner::Direction::FORWARD
        : InternalPlanner::Direction::BACKWARD;

    // We need to pass into the DeleteStageParams (below) a CanonicalQuery with a BSONObj that
    // queries for the expired documents correctly so that we do not delete documents that are
    // not actually expired when our snapshot changes during deletion.
    const char* keyFieldName = key.firstElement().fieldName();
    BSONObj query = BSON(keyFieldName << BSON("$gte" << kDawnOfTime << "$lte" << expirationDate));
    auto findCommand = std::make_unique<FindCommandRequest>(collection->ns());
    findCommand->setFilter(query);
    auto canonicalQuery = CanonicalQuery::canonicalize(opCtx, std::move(findCommand));
    invariant(canonicalQuery.getStatus());

    auto params = std::make_unique<DeleteStageParams>();
    params->isMulti = true;
    params->canonicalQuery = canonicalQuery.getValue().get();

    // Maintain a consistent view of whether batching is enabled - batching depends on
    // parameters that can be set at runtime, and it is illegal to try to get
    // BatchedDeleteStageStats from a non-batched delete.
    bool batchingEnabled = isBatchingEnabled();

    Timer timer;
    auto exec = InternalPlanner::deleteWithIndexScan(opCtx,
                                                     &collection,
                                                     std::move(params),
                                                     desc,
                                                     startKey,
                                                     endKey,
                                                     BoundInclusion::kIncludeBothStartAndEndKeys,
                                                     PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                                     direction,
                                                     getBatchedDeleteStageParams(batchingEnabled));

    try {
        const auto numDeleted = exec->executeDelete();
        ttlDeletedDocuments.increment(numDeleted);

        const auto duration = Milliseconds(timer.millis());
        if (shouldLogSlowOpWithSampling(opCtx,
                                        logv2::LogComponent::kIndex,
                                        duration,
                                        Milliseconds(serverGlobalParams.slowMS))
                .first) {
            LOGV2(5479200,
                  "Deleted expired documents using index",
                  logAttrs(collection->ns()),
                  "index"_attr = indexName,
                  "numDeleted"_attr = numDeleted,
                  "duration"_attr = duration);
        }

        if (batchingEnabled) {
            auto batchedDeleteStats = exec->getBatchedDeleteStats();
            // A pass target met implies there may be more to delete.
            return batchedDeleteStats.passTargetMet;
        }
    } catch (const ExceptionFor<ErrorCodes::QueryPlanKilled>&) {
        // It is expected that a collection drop can kill a query plan while the TTL monitor
        // is deleting an old document, so ignore this error.
    }
    return false;
}

bool TTLMonitor::_deleteExpiredWithCollscan(OperationContext* opCtx,
                                            TTLCollectionCache* ttlCollectionCache,
                                            const CollectionPtr& collection) {
    const auto& collOptions = collection->getCollectionOptions();
    uassert(5400701,
            "collection is not clustered but is described as being TTL",
            collOptions.clusteredIndex);
    invariant(collection->isClustered());

    auto expireAfterSeconds = collOptions.expireAfterSeconds;
    if (!expireAfterSeconds) {
        ttlCollectionCache->deregisterTTLClusteredIndex(collection->uuid());
        return false;
    }

    LOGV2_DEBUG(5400704, 1, "running TTL job for clustered collection", logAttrs(collection->ns()));

    const auto startId = makeCollScanStartBound(collection, Date_t::min());

    const auto expirationDate = safeExpirationDate(opCtx, collection, *expireAfterSeconds);
    const auto endId = makeCollScanEndBound(collection, expirationDate);

    auto params = std::make_unique<DeleteStageParams>();
    params->isMulti = true;

    // Maintain a consistent view of whether batching is enabled - batching depends on
    // parameters that can be set at runtime, and it is illegal to try to get
    // BatchedDeleteStageStats from a non-batched delete.
    bool batchingEnabled = isBatchingEnabled();

    // Deletes records using a bounded collection scan from the beginning of time to the
    // expiration time (inclusive).
    Timer timer;
    auto exec = InternalPlanner::deleteWithCollectionScan(
        opCtx,
        &collection,
        std::move(params),
        PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
        InternalPlanner::Direction::FORWARD,
        startId,
        endId,
        CollectionScanParams::ScanBoundInclusion::kIncludeBothStartAndEndRecords,
        getBatchedDeleteStageParams(batchingEnabled));

    try {
        const auto numDeleted = exec->executeDelete();
        ttlDeletedDocuments.increment(numDeleted);

        const auto duration = Milliseconds(timer.millis());
        if (shouldLogSlowOpWithSampling(opCtx,
                                        logv2::LogComponent::kIndex,
                                        duration,
                                        Milliseconds(serverGlobalParams.slowMS))
                .first) {
            LOGV2(5400702,
                  "Deleted expired documents using collection scan",
                  logAttrs(collection->ns()),
                  "numDeleted"_attr = numDeleted,
                  "duration"_attr = duration);
        }
        if (batchingEnabled) {
            auto batchedDeleteStats = exec->getBatchedDeleteStats();
            // A pass target met implies there may be more work to be done on the index.
            return batchedDeleteStats.passTargetMet;
        }
    } catch (const ExceptionFor<ErrorCodes::QueryPlanKilled>&) {
        // It is expected that a collection drop can kill a query plan while the TTL monitor
        // is deleting an old document, so ignore this error.
    }

    return false;
}

void startTTLMonitor(ServiceContext* serviceContext) {
    std::unique_ptr<TTLMonitor> ttlMonitor = std::make_unique<TTLMonitor>();
    ttlMonitor->go();
    TTLMonitor::set(serviceContext, std::move(ttlMonitor));
}

void shutdownTTLMonitor(ServiceContext* serviceContext) {
    TTLMonitor* ttlMonitor = TTLMonitor::get(serviceContext);
    // We allow the TTLMonitor not to be set in case shutdown occurs before the thread has been
    // initialized.
    if (ttlMonitor) {
        ttlMonitor->shutdown();
    }
}

void TTLMonitor::onStepUp(OperationContext* opCtx) {
    auto&& ttlCollectionCache = TTLCollectionCache::get(opCtx->getServiceContext());
    auto ttlInfos = ttlCollectionCache.getTTLInfos();
    for (const auto& [uuid, infos] : ttlInfos) {
        auto collectionCatalog = CollectionCatalog::get(opCtx);
        if (collectionCatalog->isCollectionAwaitingVisibility(uuid)) {
            continue;
        }

        // The collection was dropped.
        auto nss = collectionCatalog->lookupNSSByUUID(opCtx, uuid);
        if (!nss) {
            continue;
        }

        if (nss->isTemporaryReshardingCollection() || nss->isDropPendingNamespace()) {
            continue;
        }

        try {
            uassertStatusOK(userAllowedWriteNS(opCtx, *nss));

            for (const auto& info : infos) {
                // Skip clustered indexes with TTL. This includes time-series collections.
                if (info.isClustered()) {
                    continue;
                }
                if (!info.isExpireAfterSecondsNaN()) {
                    continue;
                }

                auto indexName = info.getIndexName();
                LOGV2(6847700,
                      "Running collMod to fix TTL index with NaN 'expireAfterSeconds'.",
                      "ns"_attr = *nss,
                      "uuid"_attr = uuid,
                      "name"_attr = indexName,
                      "expireAfterSecondsNew"_attr =
                          index_key_validate::kExpireAfterSecondsForInactiveTTLIndex);

                // Compose collMod command to amend 'expireAfterSeconds' to same value that
                // would be used by listIndexes() to convert the NaN value in the catalog.
                CollModIndex collModIndex;
                collModIndex.setName(StringData{indexName});
                collModIndex.setExpireAfterSeconds(mongo::durationCount<Seconds>(
                    index_key_validate::kExpireAfterSecondsForInactiveTTLIndex));
                CollMod collModCmd{*nss};
                collModCmd.getCollModRequest().setIndex(collModIndex);

                // processCollModCommand() will acquire MODE_X access to the collection.
                BSONObjBuilder builder;
                uassertStatusOK(
                    processCollModCommand(opCtx, {nss->db(), uuid}, collModCmd, &builder));
                auto result = builder.obj();
                LOGV2(6847701,
                      "Successfully fixed TTL index with NaN 'expireAfterSeconds' using collMod",
                      "ns"_attr = *nss,
                      "uuid"_attr = uuid,
                      "name"_attr = indexName,
                      "result"_attr = result);
            }
        } catch (const ExceptionForCat<ErrorCategory::Interruption>&) {
            // The exception is relevant to the entire TTL monitoring process, not just the specific
            // TTL index. Let the exception escape so it can be addressed at the higher monitoring
            // layer.
            throw;
        } catch (const DBException& ex) {
            LOGV2_ERROR(6835901,
                        "Error checking TTL job on collection during step up",
                        logAttrs(*nss),
                        "error"_attr = ex);
            continue;
        }
    }
}

long long TTLMonitor::getTTLPasses_forTest() {
    return ttlPasses.get();
}

long long TTLMonitor::getTTLSubPasses_forTest() {
    return ttlSubPasses.get();
}

}  // namespace mongo
