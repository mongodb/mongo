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


#include "mongo/db/ttl/ttl.h"

// IWYU pragma: no_include "cxxabi.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/db/admission/execution_admission_context.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/fsync_locked.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/exec/classic/batched_delete_stage.h"
#include "mongo/db/exec/classic/delete_stage.h"
#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/index_names.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/index_key_validate.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/local_catalog/shard_role_catalog/operation_sharding_state.h"
#include "mongo/db/local_catalog/shard_role_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_debug.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/write_ops/insert.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/ttl/ttl_collection_cache.h"
#include "mongo/db/ttl/ttl_gen.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/background.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log_with_sampling.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

#include <cstdint>
#include <limits>
#include <mutex>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangTTLMonitorWithLock);
MONGO_FAIL_POINT_DEFINE(hangTTLMonitorBetweenPasses);

// A TTL pass completes when there are no more expired documents to remove. A single TTL pass may
// consist of multiple sub-passes. Each sub-pass deletes all the expired documents it can up to
// 'ttlSubPassTargetSecs'. It is possible for a sub-pass to complete before all expired documents
// have been removed.
auto& ttlPasses = *MetricBuilder<Counter64>{"ttl.passes"};
auto& ttlSubPasses = *MetricBuilder<Counter64>{"ttl.subPasses"};
auto& ttlDeletedDocuments = *MetricBuilder<Counter64>{"ttl.deletedDocuments"};

// Tracks the number of TTL deletes skipped due to a TTL secondary index being present, but not
// valid for TTL removal. A non-zero value indicates there is a TTL non-conformant index present and
// users must manually modify the secondary index to utilize automatic TTL deletion.
auto& ttlInvalidTTLIndexSkips = *MetricBuilder<Counter64>{"ttl.invalidTTLIndexSkips"};

namespace {
const auto getTTLMonitor = ServiceContext::declareDecoration<std::unique_ptr<TTLMonitor>>();

// TODO (SERVER-64506): support change streams' pre- and post-images.
bool isBatchingEnabled(const CollectionPtr& collectionPtr) {
    return ttlMonitorBatchDeletes.load() && !collectionPtr->isChangeStreamPreAndPostImagesEnabled();
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
                          Date_t at,
                          std::int64_t expireAfterSeconds) {
    if (auto timeseries = coll->getTimeseriesOptions()) {
        const auto bucketMaxSpan = Seconds(*timeseries->getBucketMaxSpanSeconds());

        // Don't delete data unless it is safely out of range of the bucket maximum time
        // range. On time-series collections, the _id (and thus RecordId) is the minimum
        // time value of a bucket. A bucket may have newer data, so we cannot safely delete
        // the entire bucket yet until the maximum bucket range has passed, even if the
        // minimum value can be expired.
        return at - Seconds(expireAfterSeconds) - bucketMaxSpan;
    }

    return at - Seconds(expireAfterSeconds);
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

    const IndexDescriptor* desc = collection->getIndexCatalog()->findIndexByName(opCtx, indexName);
    if (!desc) {
        LOGV2_DEBUG(22535, 1, "index not found; skipping ttl job", "index"_attr = spec);
        return nullptr;
    }

    if (IndexType::INDEX_BTREE != IndexNames::nameToType(desc->getAccessMethodName())) {
        LOGV2_ERROR(22541,
                    "special index can't be used as a TTL index, skipping TTL job",
                    "index"_attr = spec);
        ttlInvalidTTLIndexSkips.increment();
        return nullptr;
    }

    if (auto status = index_key_validate::validateIndexSpecTTL(spec); !status.isOK()) {
        ttlInvalidTTLIndexSkips.increment();
        LOGV2_ERROR(6909100,
                    "Skipping TTL job due to invalid index spec",
                    "reason"_attr = status.reason(),
                    "ns"_attr = collection->ns(),
                    "uuid"_attr = collection->uuid(),
                    "index"_attr = spec);
        return nullptr;
    }

    return desc;
}

}  // namespace

TTLMonitor::TTLMonitor()
    : BackgroundJob(false /* selfDelete */),
      _ttlMonitorSleepSecs(Seconds{ttlMonitorSleepSecs.load()}) {}

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

Status TTLMonitor::onUpdateTTLMonitorSleepSeconds(int newSleepSeconds) {
    if (auto client = Client::getCurrent()) {
        if (auto ttlMonitor = TTLMonitor::get(client->getServiceContext())) {
            ttlMonitor->updateSleepSeconds(Seconds{newSleepSeconds});
        }
    }
    return Status::OK();
}

void TTLMonitor::updateSleepSeconds(Seconds newSeconds) {
    {
        stdx::lock_guard lk(_stateMutex);
        _ttlMonitorSleepSecs = newSeconds;
    }
    _notificationCV.notify_all();
}

void TTLMonitor::run() {
    ThreadClient tc(name(), getGlobalServiceContext()->getService(ClusterRole::ShardServer));
    AuthorizationSession::get(cc())->grantInternalAuthorization();

    while (true) {
        {
            auto startTime = Date_t::now();
            // Wait until either ttlMonitorSleepSecs passes, a shutdown is requested, or the
            // sleeping time has changed.
            stdx::unique_lock<stdx::mutex> lk(_stateMutex);
            auto deadline = startTime + _ttlMonitorSleepSecs;

            MONGO_IDLE_THREAD_BLOCK;
            while (Date_t::now() <= deadline && !_shuttingDown) {
                _notificationCV.wait_until(lk, deadline.toSystemTimePoint());
                // Recompute the deadline in case the sleep time has changed since we started.
                auto newDeadline = startTime + _ttlMonitorSleepSecs;
                if (deadline != newDeadline) {
                    LOGV2_INFO(7005501,
                               "TTL sleep deadline has changed",
                               "oldDeadline"_attr = deadline,
                               "newDeadline"_attr = newDeadline);
                    deadline = newDeadline;
                }
            }

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
            const auto opCtxPtr = cc().makeOperationContext();
            writeConflictRetry(opCtxPtr.get(), "TTL pass", NamespaceString::kEmpty, [&] {
                hangTTLMonitorBetweenPasses.pauseWhileSet(opCtxPtr.get());

                _doTTLPass(opCtxPtr.get(), Date_t::now());
            });
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
        stdx::lock_guard<stdx::mutex> lk(_stateMutex);
        _shuttingDown = true;
        _notificationCV.notify_all();
    }
    wait();
    LOGV2(3684101, "Finished shutting down TTL collection monitor thread");
}

void TTLMonitor::_doTTLPass(OperationContext* opCtx, Date_t at) {
    // Don't do work if we are a secondary (TTL will be handled by primary)
    auto replCoordinator = repl::ReplicationCoordinator::get(opCtx);
    if (replCoordinator && replCoordinator->getSettings().isReplSet() &&
        !replCoordinator->getMemberState().primary()) {
        return;
    }

    // Increment the metric after the TTL work has been finished.
    ON_BLOCK_EXIT([&] { ttlPasses.increment(); });

    bool moreToDelete = true;
    while (moreToDelete) {
        // Sub-passes may not delete all documents in the interest of fairness. If a sub-pass
        // indicates that it did not delete everything possible, we continue performing sub-passes.
        // This maintains the semantic that a full TTL pass deletes everything it possibly can
        // before sleeping periodically.
        moreToDelete = _doTTLSubPass(opCtx, at);
    }
}

bool TTLMonitor::_doTTLSubPass(OperationContext* opCtx, Date_t at) {
    // If part of replSet but not in a readable state (e.g. during initial sync), skip.
    if (repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet() &&
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
                bool moreToDelete = _doTTLIndexDelete(opCtx, at, &ttlCollectionCache, uuid, info);
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
                                   Date_t at,
                                   TTLCollectionCache* ttlCollectionCache,
                                   const UUID& uuid,
                                   const TTLCollectionCache::Info& info) {
    auto collectionCatalog = CollectionCatalog::get(opCtx);

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

    if (nss->isTemporaryReshardingCollection()) {
        // For resharding, the donor shard primary is responsible for performing the TTL
        // deletions.
        return false;
    }

    try {
        uassertStatusOK(userAllowedWriteNS(opCtx, *nss));

        const auto shardVersion = ShardVersionFactory::make(ChunkVersion::IGNORED());
        auto scopedRole = ScopedSetShardRole(opCtx, *nss, shardVersion, boost::none);
        const auto coll =
            acquireCollection(opCtx,
                              CollectionAcquisitionRequest(*nss,
                                                           {boost::none, shardVersion},
                                                           repl::ReadConcernArgs::get(opCtx),
                                                           AcquisitionPrerequisites::kWrite),
                              MODE_IX);

        // The collection with `uuid` might be renamed before the lock and the wrong namespace
        // would be locked and looked up so we double check here.
        if (!coll.exists() || coll.uuid() != uuid)
            return false;

        const auto& collectionPtr = coll.getCollectionPtr();

        if (MONGO_unlikely(hangTTLMonitorWithLock.shouldFail())) {
            LOGV2(22534,
                  "Hanging due to hangTTLMonitorWithLock fail point",
                  "ttlPasses"_attr = ttlPasses.get());
            hangTTLMonitorWithLock.pauseWhileSet(opCtx);
        }

        if (!repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, *nss)) {
            return false;
        }

        if (info.isClustered()) {
            const auto& collOptions = collectionPtr->getCollectionOptions();
            uassert(5400701,
                    "collection is not clustered but is described as being TTL",
                    collOptions.clusteredIndex);
            invariant(collectionPtr->isClustered());

            auto expireAfterSeconds = collOptions.expireAfterSeconds;
            if (!expireAfterSeconds) {
                ttlCollectionCache->deregisterTTLClusteredIndex(coll.uuid());
                return false;
            }

            if (collectionPtr->getRequiresTimeseriesExtendedRangeSupport()) {
                return _deleteExpiredWithCollscanForTimeseriesExtendedRange(
                    opCtx, at, ttlCollectionCache, coll, *expireAfterSeconds);
            } else {
                return _deleteExpiredWithCollscan(
                    opCtx, at, ttlCollectionCache, coll, *expireAfterSeconds);
            }
        } else {
            return _deleteExpiredWithIndex(
                opCtx, at, ttlCollectionCache, coll, info.getIndexName());
        }
    } catch (const ExceptionFor<ErrorCategory::StaleShardVersionError>& ex) {
        // The TTL index tried to delete some information from a sharded collection
        // through a direct operation against the shard but the filtering metadata was
        // not available or the index version in the cache was stale.
        //
        // The current TTL task cannot be completed. However, if the critical section is
        // not held the code below will fire an asynchronous refresh, hoping that the
        // next time this task is re-executed the filtering information is already
        // present. It will also invalidate the cache, causing the index information to be refreshed
        // on the next attempt.
        if (auto staleInfo = ex.extraInfo<StaleConfigInfo>();
            staleInfo && !staleInfo->getCriticalSectionSignal()) {
            auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
            ExecutorFuture<void>(executor)
                .then([serviceContext = opCtx->getServiceContext(), nss, staleInfo] {
                    ThreadClient tc("TTLShardVersionRecovery",
                                    serviceContext->getService(ClusterRole::ShardServer));
                    auto uniqueOpCtx = tc->makeOperationContext();
                    auto opCtx = uniqueOpCtx.get();

                    // Updates version in cache in case index version is stale.
                    if (staleInfo->getVersionWanted()) {
                        Grid::get(opCtx)->catalogCache()->onStaleCollectionVersion(
                            *nss, staleInfo->getVersionWanted());
                    }

                    FilteringMetadataCache::get(opCtx)
                        ->onCollectionPlacementVersionMismatch(
                            opCtx,
                            *nss,
                            staleInfo->getVersionWanted()
                                ? boost::make_optional(
                                      staleInfo->getVersionWanted()->placementVersion())
                                : boost::none)
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
        if (!opCtx->checkForInterruptNoAssert().isOK()) {
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
                                         Date_t at,
                                         TTLCollectionCache* ttlCollectionCache,
                                         const CollectionAcquisition& collection,
                                         std::string indexName) {
    const auto& collectionPtr = collection.getCollectionPtr();
    if (!collectionPtr->isIndexPresent(indexName)) {
        ttlCollectionCache->deregisterTTLIndexByName(collection.uuid(), indexName);
        return false;
    }

    BSONObj spec = collectionPtr->getIndexSpec(indexName);
    const IndexDescriptor* desc =
        getValidTTLIndex(opCtx, ttlCollectionCache, collectionPtr, spec, indexName);

    if (!desc) {
        return false;
    }

    LOGV2_DEBUG(22533,
                1,
                "running TTL job for index",
                logAttrs(collection.nss()),
                "key"_attr = desc->keyPattern(),
                "name"_attr = indexName);

    auto expireAfterSeconds = spec[IndexDescriptor::kExpireAfterSecondsFieldName].safeNumberLong();
    const Date_t kDawnOfTime = Date_t::fromMillisSinceEpoch(std::numeric_limits<long long>::min());
    const auto expirationDate = safeExpirationDate(opCtx, collectionPtr, at, expireAfterSeconds);
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
    auto findCommand = std::make_unique<FindCommandRequest>(collection.nss());
    findCommand->setFilter(query);
    auto canonicalQuery = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = ExpressionContextBuilder{}.fromRequest(opCtx, *findCommand).build(),
        .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});

    auto params = std::make_unique<DeleteStageParams>();
    OpDebug opDebug;
    params->opDebug = &opDebug;
    params->isMulti = true;
    params->canonicalQuery = canonicalQuery.get();

    // Maintain a consistent view of whether batching is enabled - batching depends on
    // parameters that can be set at runtime, and it is illegal to try to get
    // BatchedDeleteStageStats from a non-batched delete.
    const bool batchingEnabled = isBatchingEnabled(collection.getCollectionPtr());

    Timer timer;
    auto exec = InternalPlanner::deleteWithIndexScan(opCtx,
                                                     collection,
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
        PlanSummaryStats summaryStats;
        const auto& explainer = exec->getPlanExplainer();
        explainer.getSummaryStats(&summaryStats);
        if (shouldLogSlowOpWithSampling(opCtx,
                                        logv2::LogComponent::kIndex,
                                        duration,
                                        Milliseconds(serverGlobalParams.slowMS.load()))
                .first) {
            LOGV2(5479200,
                  "Deleted expired documents using index",
                  logAttrs(collection.nss()),
                  "index"_attr = indexName,
                  "numDeleted"_attr = numDeleted,
                  "numKeysDeleted"_attr = opDebug.additiveMetrics.keysDeleted.value_or(0ll),
                  "numKeysExamined"_attr = summaryStats.totalKeysExamined,
                  "numDocsExamined"_attr = summaryStats.totalDocsExamined,
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
                                            Date_t at,
                                            TTLCollectionCache* ttlCollectionCache,
                                            const CollectionAcquisition& collection,
                                            int64_t expireAfterSeconds) {
    LOGV2_DEBUG(5400704, 1, "running TTL job for clustered collection", logAttrs(collection.nss()));
    const auto& collectionPtr = collection.getCollectionPtr();

    const auto startId = makeCollScanStartBound(collectionPtr, Date_t{});

    const auto expirationDate = safeExpirationDate(opCtx, collectionPtr, at, expireAfterSeconds);
    const auto endId = makeCollScanEndBound(collectionPtr, expirationDate);

    return _performDeleteExpiredWithCollscan(
        opCtx, collection, startId, endId, /*forward*/ true, /*filter*/ nullptr);
}

bool TTLMonitor::_deleteExpiredWithCollscanForTimeseriesExtendedRange(
    OperationContext* opCtx,
    Date_t at,
    TTLCollectionCache* ttlCollectionCache,
    const CollectionAcquisition& collection,
    int64_t expireAfterSeconds) {
    // We cannot rely on the _id index for time-series data with extended time ranges. In theory
    // data eligible for deletion could be located anywhere in the collection. It would not be
    // performant to consider any bucket document. We instead run the deletion in two separate
    // batches: [epoch, at-expiry] and [2038, 2106]. The second range will include data prior to
    // the epoch unless they are too far from the epoch that cause then to be truncated into the
    // [at-expiry, 2038] range that we don't consider for deletion. This is an acceptible tradeoff
    // until we have a new _id format for time-series.
    LOGV2_DEBUG(9736801,
                1,
                "running TTL job for timeseries collection with extended range",
                logAttrs(collection.nss()));

    const auto& collectionPtr = collection.getCollectionPtr();
    bool passTargetMet = false;

    auto timeSeriesOptions = collectionPtr->getTimeseriesOptions();
    std::string timeField =
        std::string{timeseries::kControlMaxFieldNamePrefix} + timeSeriesOptions->getTimeField();
    LTEMatchExpression filter(boost::optional<StringData>{timeField},
                              Value{at - Seconds(expireAfterSeconds)});

    // Delete from the beginning of the clustered _id index. In the typical case we consider
    // anything from the epoch to at-expiry eligible for deletion. We add a filter to ensure we
    // don't delete any data after 2038 that is not eligible for deletion.
    {
        const auto startId = makeCollScanStartBound(collectionPtr, Date_t{});
        const auto expirationDate =
            safeExpirationDate(opCtx, collectionPtr, at, expireAfterSeconds);
        const auto endId = makeCollScanEndBound(collectionPtr, expirationDate);

        passTargetMet |= _performDeleteExpiredWithCollscan(
            opCtx, collection, startId, endId, /*forward*/ true, &filter);
    }

    // Delete from the end of the clustered _id index. In the typical case nothing should be
    // deleted. But data prior to 1970 is sorted at the end and is eligible for deletion. We add a
    // filter to ensure we only delete such data.    {
    {
        // 0x80000000 (in seconds) is the first value that no longer fits in a signed 32bit integer.
        // We subtract the bucket span to get the beginning of the range we should consider
        // deleting.
        const auto startId = makeCollScanStartBound(
            collectionPtr,
            Date_t::fromMillisSinceEpoch((static_cast<long long>(0x80000000) -
                                          *timeSeriesOptions->getBucketMaxSpanSeconds()) *
                                         1000));

        const auto endId = makeCollScanEndBound(
            collectionPtr, Date_t::fromMillisSinceEpoch(static_cast<long long>(0xFFFFFFFF) * 1000));

        passTargetMet |= _performDeleteExpiredWithCollscan(
            opCtx, collection, startId, endId, /*forward*/ false, &filter);
    }
    return passTargetMet;
}


bool TTLMonitor::_performDeleteExpiredWithCollscan(OperationContext* opCtx,
                                                   const CollectionAcquisition& collection,
                                                   const RecordIdBound& startBound,
                                                   const RecordIdBound& endBound,
                                                   bool forward,
                                                   const MatchExpression* filter) {
    auto params = std::make_unique<DeleteStageParams>();
    OpDebug opDebug;
    params->opDebug = &opDebug;
    params->isMulti = true;

    // Maintain a consistent view of whether batching is enabled - batching depends on
    // parameters that can be set at runtime, and it is illegal to try to get
    // BatchedDeleteStageStats from a non-batched delete.
    const bool batchingEnabled = isBatchingEnabled(collection.getCollectionPtr());

    // Deletes records using a bounded collection scan from the beginning of time to the
    // expiration time (inclusive).
    Timer timer;
    auto exec = InternalPlanner::deleteWithCollectionScan(
        opCtx,
        collection,
        std::move(params),
        PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
        forward ? InternalPlanner::Direction::FORWARD : InternalPlanner::Direction::BACKWARD,
        startBound,
        endBound,
        CollectionScanParams::ScanBoundInclusion::kIncludeBothStartAndEndRecords,
        getBatchedDeleteStageParams(batchingEnabled),
        filter);

    try {
        const auto numDeleted = exec->executeDelete();
        ttlDeletedDocuments.increment(numDeleted);

        const auto duration = Milliseconds(timer.millis());
        PlanSummaryStats summaryStats;
        const auto& explainer = exec->getPlanExplainer();
        explainer.getSummaryStats(&summaryStats);
        if (shouldLogSlowOpWithSampling(opCtx,
                                        logv2::LogComponent::kIndex,
                                        duration,
                                        Milliseconds(serverGlobalParams.slowMS.load()))
                .first) {
            LOGV2(5400702,
                  "Deleted expired documents using clustered index scan",
                  logAttrs(collection.nss()),
                  "numDeleted"_attr = numDeleted,
                  "numKeysDeleted"_attr = opDebug.additiveMetrics.keysDeleted.value_or(0ll),
                  "numKeysExamined"_attr = summaryStats.totalKeysExamined,
                  "numDocsExamined"_attr = summaryStats.totalDocsExamined,
                  "duration"_attr = duration,
                  "extendedRange"_attr =
                      collection.getCollectionPtr()->getRequiresTimeseriesExtendedRangeSupport());
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

void startTTLMonitor(ServiceContext* serviceContext, bool setupOnly) {
    std::unique_ptr<TTLMonitor> ttlMonitor = std::make_unique<TTLMonitor>();
    if (!setupOnly)
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

long long TTLMonitor::getTTLPasses_forTest() {
    return ttlPasses.get();
}

long long TTLMonitor::getTTLSubPasses_forTest() {
    return ttlSubPasses.get();
}

long long TTLMonitor::getInvalidTTLIndexSkips_forTest() {
    return ttlInvalidTTLIndexSkips.get();
}

}  // namespace mongo
