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
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/fsync_locked.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/delete.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/resource_consumption_metrics.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/ttl_collection_cache.h"
#include "mongo/db/ttl_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/util/background.h"
#include "mongo/util/concurrency/idle_thread_block.h"

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

private:
    /**
     * Gets all TTL indexes from every collection and performs doTTLForIndex().
     */
    void doTTLPass() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;

        // If part of replSet but not in a readable state (e.g. during initial sync), skip.
        if (repl::ReplicationCoordinator::get(&opCtx)->getReplicationMode() ==
                repl::ReplicationCoordinator::modeReplSet &&
            !repl::ReplicationCoordinator::get(&opCtx)->getMemberState().readable())
            return;

        TTLCollectionCache& ttlCollectionCache = TTLCollectionCache::get(getGlobalServiceContext());
        std::vector<std::pair<UUID, std::string>> ttlInfos = ttlCollectionCache.getTTLInfos();

        // Pair of collection namespace and index spec.
        std::vector<std::pair<NamespaceString, BSONObj>> ttlIndexes;

        // Increment the metric after the TTL work has been finished.
        ON_BLOCK_EXIT([&] { ttlPasses.increment(); });

        // Get all TTL indexes from every collection.
        auto collectionCatalog = CollectionCatalog::get(opCtxPtr.get());
        for (const std::pair<UUID, std::string>& ttlInfo : ttlInfos) {
            auto uuid = ttlInfo.first;
            auto indexName = ttlInfo.second;

            // Skip collections that have not been made visible yet. The TTLCollectionCache already
            // has the index information available, so we want to avoid removing it until the
            // collection is visible.
            if (collectionCatalog->isCollectionAwaitingVisibility(uuid)) {
                continue;
            }

            auto nss = collectionCatalog->lookupNSSByUUID(&opCtx, uuid);
            if (!nss) {
                ttlCollectionCache.deregisterTTLInfo(ttlInfo);
                continue;
            }

            if (nss->isTemporaryReshardingCollection()) {
                // For resharding, the donor shard primary is responsible for performing the TTL
                // deletions.
                continue;
            }

            AutoGetCollection coll(&opCtx, *nss, MODE_IS);
            // The collection with `uuid` might be renamed before the lock and the wrong
            // namespace would be locked and looked up so we double check here.
            if (!coll || coll->uuid() != uuid)
                continue;

            if (!DurableCatalog::get(opCtxPtr.get())
                     ->isIndexPresent(&opCtx, coll->getCatalogId(), indexName)) {
                ttlCollectionCache.deregisterTTLInfo(ttlInfo);
                continue;
            }

            BSONObj spec = DurableCatalog::get(opCtxPtr.get())
                               ->getIndexSpec(&opCtx, coll->getCatalogId(), indexName);
            if (!spec.hasField(IndexDescriptor::kExpireAfterSecondsFieldName)) {
                ttlCollectionCache.deregisterTTLInfo(ttlInfo);
                continue;
            }

            if (!DurableCatalog::get(opCtxPtr.get())
                     ->isIndexReady(&opCtx, coll->getCatalogId(), indexName))
                continue;

            ttlIndexes.push_back(std::make_pair(*nss, spec.getOwned()));
        }

        for (const auto& it : ttlIndexes) {
            try {
                doTTLForIndex(&opCtx, it.first, it.second);
            } catch (const ExceptionForCat<ErrorCategory::Interruption>&) {
                LOGV2_WARNING(22537,
                              "TTLMonitor was interrupted, waiting {ttlMonitorSleepSecs_load} "
                              "seconds before doing another pass",
                              "TTLMonitor was interrupted, waiting before doing another pass",
                              "wait"_attr = Milliseconds(Seconds(ttlMonitorSleepSecs.load())));
                return;
            } catch (const DBException& dbex) {
                LOGV2_ERROR(22538,
                            "Error processing ttl index: {it_second} -- {dbex}",
                            "Error processing TTL index",
                            "index"_attr = it.second,
                            "error"_attr = dbex);
                // Continue on to the next index.
                continue;
            }
        }
    }

    /**
     * Removes documents from the collection using the specified TTL index after a sufficient amount
     * of time has passed according to its expiry specification.
     */
    void doTTLForIndex(OperationContext* opCtx, NamespaceString collectionNSS, BSONObj idx) {
        if (collectionNSS.isDropPendingNamespace()) {
            return;
        }
        if (!userAllowedWriteNS(collectionNSS).isOK()) {
            LOGV2_ERROR(
                22539,
                "namespace '{namespace}' doesn't allow deletes, skipping ttl job for: {index}",
                "Namespace doesn't allow deletes, skipping TTL job",
                logAttrs(collectionNSS),
                "index"_attr = idx);
            return;
        }

        const BSONObj key = idx["key"].Obj();
        const StringData name = idx["name"].valueStringData();
        if (key.nFields() != 1) {
            LOGV2_ERROR(22540,
                        "key for ttl index can only have 1 field, skipping ttl job for: {index}",
                        "Key for ttl index can only have 1 field, skipping TTL job",
                        "index"_attr = idx);
            return;
        }

        LOGV2_DEBUG(22533,
                    1,
                    "ns: {collectionNSS} key: {key} name: {name}",
                    "collectionNSS"_attr = collectionNSS,
                    "key"_attr = key,
                    "name"_attr = name);

        AutoGetCollection collection(opCtx, collectionNSS, MODE_IX);
        if (MONGO_unlikely(hangTTLMonitorWithLock.shouldFail())) {
            LOGV2(22534, "Hanging due to hangTTLMonitorWithLock fail point");
            hangTTLMonitorWithLock.pauseWhileSet(opCtx);
        }

        if (!collection) {
            // Collection was dropped.
            return;
        }

        if (!repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, collectionNSS)) {
            return;
        }

        ResourceConsumption::ScopedMetricsCollector scopedMetrics(opCtx,
                                                                  collectionNSS.db().toString());

        const IndexDescriptor* desc = collection->getIndexCatalog()->findIndexByName(opCtx, name);
        if (!desc) {
            LOGV2_DEBUG(22535,
                        1,
                        "index not found (index build in progress? index dropped?), skipping ttl "
                        "job for: {idx}",
                        "idx"_attr = idx);
            return;
        }

        // Re-read 'idx' from the descriptor, in case the collection or index definition changed
        // before we re-acquired the collection lock.
        idx = desc->infoObj();

        if (IndexType::INDEX_BTREE != IndexNames::nameToType(desc->getAccessMethodName())) {
            LOGV2_ERROR(22541,
                        "special index can't be used as a ttl index, skipping ttl job for: {index}",
                        "Special index can't be used as a TTL index, skipping TTL job",
                        "index"_attr = idx);
            return;
        }

        BSONElement secondsExpireElt = idx[IndexDescriptor::kExpireAfterSecondsFieldName];
        if (!secondsExpireElt.isNumber()) {
            LOGV2_ERROR(22542,
                        "ttl indexes require the {expireField} field to be numeric but received a "
                        "type of {typeName_secondsExpireElt_type}, skipping ttl job for: {idx}",
                        "TTL indexes require the expire field to be numeric, skipping TTL job",
                        "field"_attr = IndexDescriptor::kExpireAfterSecondsFieldName,
                        "type"_attr = typeName(secondsExpireElt.type()),
                        "index"_attr = idx);
            return;
        }

        const Date_t kDawnOfTime =
            Date_t::fromMillisSinceEpoch(std::numeric_limits<long long>::min());
        const Date_t expirationTime = Date_t::now() - Seconds(secondsExpireElt.numberLong());
        const BSONObj startKey = BSON("" << kDawnOfTime);
        const BSONObj endKey = BSON("" << expirationTime);
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
            BSON(keyFieldName << BSON("$gte" << kDawnOfTime << "$lte" << expirationTime));
        auto findCommand = std::make_unique<FindCommand>(collectionNSS);
        findCommand->setFilter(query);
        auto canonicalQuery = CanonicalQuery::canonicalize(opCtx, std::move(findCommand));
        invariant(canonicalQuery.getStatus());

        auto params = std::make_unique<DeleteStageParams>();
        params->isMulti = true;
        params->canonicalQuery = canonicalQuery.getValue().get();

        auto exec =
            InternalPlanner::deleteWithIndexScan(opCtx,
                                                 &collection.getCollection(),
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
            LOGV2_DEBUG(22536, 1, "deleted: {numDeleted}", "numDeleted"_attr = numDeleted);
        } catch (const ExceptionFor<ErrorCodes::QueryPlanKilled>&) {
            // It is expected that a collection drop can kill a query plan while the TTL monitor is
            // deleting an old document, so ignore this error.
            return;
        } catch (const DBException& exception) {
            LOGV2_WARNING(22543,
                          "ttl query execution for index {index} failed with status: {error}",
                          "TTL query execution failed",
                          "index"_attr = idx,
                          "error"_attr = redact(exception.toStatus()));
            return;
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

}  // namespace mongo
