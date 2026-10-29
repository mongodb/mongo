// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/record_id_bound.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/ttl/ttl_collection_cache.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/background.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <mutex>
#include <string>

namespace [[MONGO_MOD_PRIVATE]] mongo {
class TTLMonitor : public BackgroundJob {
public:
    TTLMonitor();

    static TTLMonitor* get(ServiceContext* serviceCtx);

    static void set(ServiceContext* serviceCtx, std::unique_ptr<TTLMonitor> monitor);

    static Status onUpdateTTLMonitorSleepSeconds(int newSleepSeconds);

    std::string name() const override {
        return "TTLMonitor";
    }

    void run() override;

    /**
     * Signals the thread to quit and then waits until it does.
     */
    void shutdown();

    void updateSleepSeconds(Seconds newSeconds);

    [[MONGO_MOD_PRIVATE]] long long getTTLPasses_forTest();
    [[MONGO_MOD_PRIVATE]] long long getTTLSubPasses_forTest();
    [[MONGO_MOD_PRIVATE]] long long getTTLDurationMicros_forTest();
    [[MONGO_MOD_PRIVATE]] long long getTTLDeletedDocuments_forTest();
    [[MONGO_MOD_PRIVATE]] long long getTTLDeletedKeys_forTest();
    [[MONGO_MOD_PRIVATE]] long long getTTLExaminedDocuments_forTest();
    [[MONGO_MOD_PRIVATE]] long long getTTLExaminedKeys_forTest();
    [[MONGO_MOD_PRIVATE]] long long getInvalidTTLIndexSkips_forTest();

private:
    friend class TTLTest;

    /**
     * Deletes all expired documents. May consist of several sub-passes.
     */
    void _doTTLPass(OperationContext* opCtx, Date_t at);

    /**
     * A sub-pass iterates over a list of TTL indexes until there are no more expired documents to
     * delete or 'ttlSubPassTargetSecs' is exceeded.
     *
     * Once it is confirmed there are no more expired documents on an index, the index will not be
     * visited again for the remainder of the sub-pass.
     *
     *
     * Returns true if there are more expired documents to delete. False otherwise.
     */
    bool _doTTLSubPass(OperationContext* opCtx, Date_t at);

    /**
     * Given a TTL index, attempts to delete all expired documents through the index until
     * - hitting the batched delete document or time limit for a single TTL index
     * - removing all expired documents corresponding to the TTLIndex
     * - reaching an error
     *
     * Returns true if there are more expired documents to delete (a batched delete limit is met).
     * Returns false if there are no more expired documents to delete at this time - including when
     * errors specific to the TTL index are reached.
     *
     * When batching is disabled, always returns false since all expired documents are guaranteed to
     * be removed.
     *
     * In some cases (i.e: on temporary resharding collections, collections pending to be dropped,
     * etc), the TTLMonitor is prohibitied from removing expired documents - returns false in these
     * scenarios as well.
     */
    bool _doTTLIndexDelete(OperationContext* opCtx,
                           Date_t at,
                           TTLCollectionCache* ttlCollectionCache,
                           const UUID& uuid,
                           const TTLCollectionCache::Info& info);

    /**
     * Removes documents from the collection using the specified TTL index after a sufficient
     * amount of time has passed according to its expiry specification.
     *
     * Returns true if there are more expired documents to delete through the index at this time.
     * False otherwise.
     *
     * When batching is disabled, always returns false since all expired documents are guaranteed to
     * be removed.
     */
    bool _deleteExpiredWithIndex(OperationContext* opCtx,
                                 Date_t at,
                                 TTLCollectionCache* ttlCollectionCache,
                                 const CollectionAcquisition& collection,
                                 std::string indexName);

    /*
     * Removes expired documents from a clustered collection using a bounded collection scan.
     * On time-series buckets collections, TTL operates on type 'ObjectId'. On general purpose
     * collections, TTL operates on type 'Date'.
     *
     * Returns true if there are more expired documents to delete through the clustered index at
     * this time. False otherwise.
     *
     * When batching is disabled, always returns false since all expired documents are guaranteed to
     * be removed.
     */
    bool _deleteExpiredWithCollscan(OperationContext* opCtx,
                                    Date_t at,
                                    TTLCollectionCache* ttlCollectionCache,
                                    const CollectionAcquisition& collection,
                                    int64_t expireAfterSeconds);

    /*
     * Removes expired documents from a timeseries collection containing extended range data using
     * two bounded collection scans. On time-series buckets collections, TTL operates on type
     * 'ObjectId'. On general purpose collections, TTL operates on type 'Date'.
     *
     * Returns true if there are more expired documents to delete through the clustered index at
     * this time. False otherwise.
     *
     * When batching is disabled, always returns false since all expired documents are guaranteed to
     * be removed.
     */
    bool _deleteExpiredWithCollscanForTimeseriesExtendedRange(
        OperationContext* opCtx,
        Date_t at,
        TTLCollectionCache* ttlCollectionCache,
        const CollectionAcquisition& collection,
        int64_t expireAfterSeconds);


    /*
     * Helper to perform a bounded deletion collection scan with an optional match expression.
     *
     * Returns true if there are more expired documents to delete through the clustered index at
     * this time. False otherwise.
     *
     * When batching is disabled, always returns false since all expired documents are guaranteed to
     * be removed.
     *
     * Filter is an optional MatchExpression to use for the delete. Pass in nullptr to disable
     * filtering.
     */
    bool _performDeleteExpiredWithCollscan(OperationContext* opCtx,
                                           const CollectionAcquisition& collection,
                                           const RecordIdBound& startBound,
                                           const RecordIdBound& endBound,
                                           bool forward,
                                           const MatchExpression* filter);

    /**
     * Schedules an async task that will recover the sharding metadata. It does not wait for the
     * recovery to complete.
     */
    void _scheduleMetadataRecovery(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   const Status& staleStatus);

    // Protects the state below.
    mutable std::mutex _stateMutex;

    // Signaled to wake up the thread, if the thread is waiting. This condition variable is used to
    // notify the thread of either:
    // * The server is shutting down.
    // * The ttlMonitorSleepSecs variable has changed.
    // If the server is shutting down the monitor will stop.
    mutable stdx::condition_variable _notificationCV;

    bool _shuttingDown = false;
    Seconds _ttlMonitorSleepSecs;

    // Set of namespaces for which a sharding metadata recovery is pending.
    stdx::unordered_set<NamespaceString> _namespacesRequiringMetadataRefresh;

    // Executor used to schedule sharding metadata recovery tasks.
    std::shared_ptr<executor::TaskExecutor> _metadataRefreshTaskExecutor;
};

}  // namespace mongo
