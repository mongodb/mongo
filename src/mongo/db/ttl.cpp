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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/db/ttl.h"

#include "mongo/base/counter.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/catalog/coll_mod.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/client.h"
#include "mongo/db/coll_mod_gen.h"
#include "mongo/db/commands/fsync_locked.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/delete.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/tenant_migration_access_blocker_registry.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/resource_consumption_metrics.h"
#include "mongo/db/timeseries/bucket_catalog.h"
#include "mongo/db/ttl_collection_cache.h"
#include "mongo/db/ttl_gen.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/logv2/log.h"
#include "mongo/util/background.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/log_with_sampling.h"

namespace mongo {

class TTLMonitor;

namespace {

const auto getTTLMonitor = ServiceContext::declareDecoration<std::unique_ptr<TTLMonitor>>();

}  // namespace

MONGO_FAIL_POINT_DEFINE(hangTTLMonitorWithLock);

Counter64 ttlPasses;
Counter64 ttlDeletedDocuments;

ServerStatusMetricField<Counter64> ttlPassesDisplay("ttl.passes", &ttlPasses);
ServerStatusMetricField<Counter64> ttlDeletedDocumentsDisplay("ttl.deletedDocuments",
                                                              &ttlDeletedDocuments);
using MtabType = TenantMigrationAccessBlocker::BlockerType;

class TTLMonitor : public BackgroundJob {
public:
    explicit TTLMonitor() : BackgroundJob(false /* selfDelete */) {}

    static TTLMonitor* get(ServiceContext* serviceCtx) {
        return getTTLMonitor(serviceCtx).get();
    }

    static void set(ServiceContext* serviceCtx, std::unique_ptr<TTLMonitor> monitor) {
        auto& ttlMonitor = getTTLMonitor(serviceCtx);
        if (ttlMonitor) {
            invariant(!ttlMonitor->running(),
                      "Tried to reset the TTLMonitor without shutting down the original instance.");
        }

        invariant(monitor);
        ttlMonitor = std::move(monitor);
    }

    std::string name() const {
        return "TTLMonitor";
    }

    void run() {
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
                doTTLPass();
            } catch (const WriteConflictException&) {
                LOGV2_DEBUG(22531, 1, "got WriteConflictException");
            } catch (const ExceptionForCat<ErrorCategory::Interruption>& interruption) {
                LOGV2_DEBUG(22532,
                            1,
                            "TTLMonitor was interrupted: {interruption}",
                            "interruption"_attr = interruption);
            }
        }
    }

    /**
     * Signals the thread to quit and then waits until it does.
     */
    void shutdown() {
        LOGV2(3684100, "Shutting down TTL collection monitor thread");
        {
            stdx::lock_guard<Latch> lk(_stateMutex);
            _shuttingDown = true;
            _shuttingDownCV.notify_one();
        }
        wait();
        LOGV2(3684101, "Finished shutting down TTL collection monitor thread");
    }

    /**
     * Invoked when the node enters the primary state.
     */
    void onStepUp(OperationContext* opCtx);

private:
    /**
     * Gets all TTL specifications for every collection and deletes expired documents.
     */
    void doTTLPass() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext* opCtx = opCtxPtr.get();

        // If part of replSet but not in a readable state (e.g. during initial sync), skip.
        if (repl::ReplicationCoordinator::get(opCtx)->getReplicationMode() ==
                repl::ReplicationCoordinator::modeReplSet &&
            !repl::ReplicationCoordinator::get(opCtx)->getMemberState().readable())
            return;

        TTLCollectionCache& ttlCollectionCache = TTLCollectionCache::get(getGlobalServiceContext());
        auto ttlInfos = ttlCollectionCache.getTTLInfos();

        // Increment the metric after the TTL work has been finished.
        ON_BLOCK_EXIT([&] { ttlPasses.increment(); });

        // Perform a pass for every collection and index described as being TTL.
        for (const auto& [uuid, infos] : ttlInfos) {
            for (const auto& info : infos) {
                // Skip collections that have not been made visible yet. The TTLCollectionCache
                // already has the index information available, so we want to avoid removing it
                // until the collection is visible.
                auto collectionCatalog = CollectionCatalog::get(opCtx);
                if (collectionCatalog->isCollectionAwaitingVisibility(uuid)) {
                    continue;
                }

                // The collection was dropped.
                auto nss = collectionCatalog->lookupNSSByUUID(opCtx, uuid);
                if (!nss) {
                    if (info.isClustered()) {
                        ttlCollectionCache.deregisterTTLClusteredIndex(uuid);
                    } else {
                        ttlCollectionCache.deregisterTTLIndexByName(uuid, info.getIndexName());
                    }
                    continue;
                }

                try {
                    deleteExpired(opCtx, &ttlCollectionCache, uuid, *nss, info);
                } catch (const ExceptionForCat<ErrorCategory::Interruption>&) {
                    LOGV2_WARNING(22537,
                                  "TTLMonitor was interrupted, waiting before doing another pass",
                                  "wait"_attr = Milliseconds(Seconds(ttlMonitorSleepSecs.load())));
                    return;
                } catch (const DBException& ex) {
                    LOGV2_ERROR(5400703,
                                "Error running TTL job on collection",
                                logAttrs(*nss),
                                "error"_attr = ex);
                    continue;
                }
            }
        }
    }

    /**
     * Deletes expired data on the given collection with the provided information.
     */
    void deleteExpired(OperationContext* opCtx,
                       TTLCollectionCache* ttlCollectionCache,
                       const UUID& uuid,
                       const NamespaceString& nss,
                       const TTLCollectionCache::Info& info) {
        if (nss.isTemporaryReshardingCollection()) {
            // For resharding, the donor shard primary is responsible for performing the TTL
            // deletions.
            return;
        }

        if (nss.isDropPendingNamespace()) {
            return;
        }

        uassertStatusOK(userAllowedWriteNS(opCtx, nss));

        AutoGetCollection coll(opCtx, nss, MODE_IX);
        // The collection with `uuid` might be renamed before the lock and the wrong namespace would
        // be locked and looked up so we double check here.
        if (!coll || coll->uuid() != uuid)
            return;

        // TTL indexes are not compatible with capped collections.
        invariant(!coll->isCapped());

        if (MONGO_unlikely(hangTTLMonitorWithLock.shouldFail())) {
            LOGV2(22534,
                  "Hanging due to hangTTLMonitorWithLock fail point",
                  "ttlPasses"_attr = ttlPasses.get());
            hangTTLMonitorWithLock.pauseWhileSet(opCtx);
        }

        if (!repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss)) {
            return;
        }

        std::shared_ptr<TenantMigrationAccessBlocker> mtab;
        if (coll.getDb() &&
            nullptr !=
                (mtab = TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                            .getTenantMigrationAccessBlockerForDbName(coll.getDb()->name(),
                                                                      MtabType::kRecipient)) &&
            mtab->checkIfShouldBlockTTL()) {
            LOGV2_DEBUG(53768,
                        1,
                        "Postpone TTL of DB because of active tenant migration",
                        "tenantMigrationAccessBlocker"_attr = mtab->getDebugInfo().jsonString(),
                        "database"_attr = coll.getDb()->name());
            return;
        }

        ResourceConsumption::ScopedMetricsCollector scopedMetrics(opCtx, nss.db().toString());

        const auto& collection = coll.getCollection();
        if (info.isClustered()) {
            deleteExpiredWithCollscan(opCtx, ttlCollectionCache, collection);
        } else {
            deleteExpiredWithIndex(opCtx, ttlCollectionCache, collection, info.getIndexName());
        }
    }

    /**
     * Generate the safe expiration date for a given collection and user-configured
     * expireAfterSeconds value.
     */
    Date_t safeExpirationDate(OperationContext* opCtx,
                              const CollectionPtr& coll,
                              std::int64_t expireAfterSeconds) const {
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

    /**
     * Removes documents from the collection using the specified TTL index after a sufficient
     * amount of time has passed according to its expiry specification.
     */
    void deleteExpiredWithIndex(OperationContext* opCtx,
                                TTLCollectionCache* ttlCollectionCache,
                                const CollectionPtr& collection,
                                std::string indexName) {
        if (!collection->isIndexPresent(indexName)) {
            ttlCollectionCache->deregisterTTLIndexByName(collection->uuid(), indexName);
            return;
        }

        BSONObj spec = collection->getIndexSpec(indexName);
        if (!spec.hasField(IndexDescriptor::kExpireAfterSecondsFieldName)) {
            ttlCollectionCache->deregisterTTLIndexByName(collection->uuid(), indexName);
            return;
        }

        if (!collection->isIndexReady(indexName)) {
            return;
        }

        const BSONObj key = spec["key"].Obj();
        const StringData name = spec["name"].valueStringData();
        if (key.nFields() != 1) {
            LOGV2_ERROR(22540,
                        "key for ttl index can only have 1 field, skipping TTL job",
                        "index"_attr = spec);
            return;
        }

        LOGV2_DEBUG(22533,
                    1,
                    "running TTL job for index",
                    logAttrs(collection->ns()),
                    "key"_attr = key,
                    "name"_attr = name);

        const IndexDescriptor* desc = collection->getIndexCatalog()->findIndexByName(opCtx, name);
        if (!desc) {
            LOGV2_DEBUG(22535, 1, "index not found; skipping ttl job", "index"_attr = spec);
            return;
        }

        if (IndexType::INDEX_BTREE != IndexNames::nameToType(desc->getAccessMethodName())) {
            LOGV2_ERROR(22541,
                        "special index can't be used as a TTL index, skipping TTL job",
                        "index"_attr = spec);
            return;
        }

        BSONElement secondsExpireElt = spec[IndexDescriptor::kExpireAfterSecondsFieldName];
        if (!secondsExpireElt.isNumber() || secondsExpireElt.isNaN()) {
            LOGV2_ERROR(22542,
                        "TTL indexes require the expire field to be numeric and not a NaN, "
                        "skipping TTL job",
                        "ns"_attr = collection->ns(),
                        "uuid"_attr = collection->uuid(),
                        "field"_attr = IndexDescriptor::kExpireAfterSecondsFieldName,
                        "type"_attr = typeName(secondsExpireElt.type()),
                        "index"_attr = spec);
            return;
        }

        const Date_t kDawnOfTime =
            Date_t::fromMillisSinceEpoch(std::numeric_limits<long long>::min());
        const auto expirationDate =
            safeExpirationDate(opCtx, collection, secondsExpireElt.safeNumberLong());
        const BSONObj startKey = BSON("" << kDawnOfTime);
        const BSONObj endKey = BSON("" << expirationDate);
        // The canonical check as to whether a key pattern element is "ascending" or
        // "descending" is (elt.number() >= 0).  This is defined by the Ordering class.
        const InternalPlanner::Direction direction = (key.firstElement().number() >= 0)
            ? InternalPlanner::Direction::FORWARD
            : InternalPlanner::Direction::BACKWARD;

        // We need to pass into the DeleteStageParams (below) a CanonicalQuery with a BSONObj that
        // queries for the expired documents correctly so that we do not delete documents that are
        // not actually expired when our snapshot changes during deletion.
        const char* keyFieldName = key.firstElement().fieldName();
        BSONObj query =
            BSON(keyFieldName << BSON("$gte" << kDawnOfTime << "$lte" << expirationDate));
        auto findCommand = std::make_unique<FindCommandRequest>(collection->ns());
        findCommand->setFilter(query);
        auto canonicalQuery = CanonicalQuery::canonicalize(opCtx, std::move(findCommand));
        invariant(canonicalQuery.getStatus());

        auto params = std::make_unique<DeleteStageParams>();
        params->isMulti = true;
        params->canonicalQuery = canonicalQuery.getValue().get();

        Timer timer;
        auto exec =
            InternalPlanner::deleteWithIndexScan(opCtx,
                                                 &collection,
                                                 std::move(params),
                                                 desc,
                                                 startKey,
                                                 endKey,
                                                 BoundInclusion::kIncludeBothStartAndEndKeys,
                                                 PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                                 direction);

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
                      "index"_attr = name,
                      "numDeleted"_attr = numDeleted,
                      "duration"_attr = duration);
            }
        } catch (const ExceptionFor<ErrorCodes::QueryPlanKilled>&) {
            // It is expected that a collection drop can kill a query plan while the TTL monitor
            // is deleting an old document, so ignore this error.
        }
    }

    /*
     * Removes expired documents from a collection clustered by _id using a bounded collection scan.
     */
    void deleteExpiredWithCollscan(OperationContext* opCtx,
                                   TTLCollectionCache* ttlCollectionCache,
                                   const CollectionPtr& collection) {
        const auto& collOptions = collection->getCollectionOptions();
        uassert(5400701,
                "collection is not clustered by _id but is described as being TTL",
                collOptions.clusteredIndex);
        invariant(collection->isClustered());

        auto expireAfterSeconds = collOptions.expireAfterSeconds;
        if (!expireAfterSeconds) {
            ttlCollectionCache->deregisterTTLClusteredIndex(collection->uuid());
            return;
        }

        LOGV2_DEBUG(5400704,
                    1,
                    "running TTL job for collection clustered by _id",
                    logAttrs(collection->ns()));

        const auto expirationDate = safeExpirationDate(opCtx, collection, *expireAfterSeconds);

        // Generate upper bound ObjectId that compares greater than every ObjectId with a the same
        // timestamp or lower.
        auto endOID = OID();
        endOID.init(expirationDate, true /* max */);

        const auto endId = record_id_helpers::keyForOID(endOID);

        auto params = std::make_unique<DeleteStageParams>();
        params->isMulti = true;

        // Deletes records using a bounded collection scan from the beginning of time to the
        // expiration time (inclusive).
        Timer timer;
        auto exec =
            InternalPlanner::deleteWithCollectionScan(opCtx,
                                                      &collection,
                                                      std::move(params),
                                                      PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                                      InternalPlanner::Direction::FORWARD,
                                                      boost::none /* minRecord */,
                                                      endId);

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
        } catch (const ExceptionFor<ErrorCodes::QueryPlanKilled>&) {
            // It is expected that a collection drop can kill a query plan while the TTL monitor
            // is deleting an old document, so ignore this error.
        }
    }

    // Protects the state below.
    mutable Mutex _stateMutex = MONGO_MAKE_LATCH("TTLMonitorStateMutex");

    // Signaled to wake up the thread, if the thread is waiting. The thread will check whether
    // _shuttingDown is set and stop accordingly.
    mutable stdx::condition_variable _shuttingDownCV;

    bool _shuttingDown = false;
};

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
                collModCmd.setIndex(collModIndex);

                // processCollModCommand() will acquire MODE_X access to the collection.
                BSONObjBuilder builder;
                uassertStatusOK(collMod(opCtx, *nss, collModCmd.toBSON({}), &builder));
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

namespace {

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

}  // namespace mongo
