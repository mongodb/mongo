/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
#include "mongo/db/change_stream_pre_images_tenant_truncate_markers.h"

#include "mongo/db/change_stream_pre_image_util.h"
#include "mongo/db/change_stream_pre_images_truncate_markers_per_nsUUID.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role.h"
#include "mongo/db/storage/collection_truncate_markers.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/concurrent_shared_values_map.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace {

// Acquires the pre-images collection given 'nsOrUUID'. When provided a UUID, throws
// NamespaceNotFound if the collection is dropped.
auto acquirePreImagesCollectionForRead(OperationContext* opCtx, NamespaceStringOrUUID nssOrUUID) {
    return acquireCollection(
        opCtx,
        CollectionAcquisitionRequest(std::move(nssOrUUID),
                                     PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                     repl::ReadConcernArgs::get(opCtx),
                                     AcquisitionPrerequisites::kRead),
        MODE_IS);
}
auto acquirePreImagesCollectionForWrite(OperationContext* opCtx, NamespaceStringOrUUID nssOrUUID) {
    return acquireCollection(
        opCtx,
        CollectionAcquisitionRequest(std::move(nssOrUUID),
                                     PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                     repl::ReadConcernArgs::get(opCtx),
                                     AcquisitionPrerequisites::kWrite),
        MODE_IX);
}

// Truncate ranges must be consistent data - no record within a truncate range should be written
// after the truncate. Otherwise, the data viewed by an open change stream could be corrupted,
// only seeing part of the range post truncate. The node can either be a secondary or primary at
// this point. Restrictions must be in place to ensure consistent ranges in both scenarios.
//      - Primaries can't truncate past the 'allDurable' Timestamp. 'allDurable'
//      guarantees out-of-order writes on the primary don't leave oplog holes.
//
//      - Secondaries can't truncate past the 'lastApplied' timestamp. Within an oplog batch,
//      entries are applied out of order, thus truncate markers may be created before the entire
//      batch is finished.
//      The 'allDurable' Timestamp is not sufficient given it is computed from within WT, which
//      won't always know there are entries with opTime < 'allDurable' which have yet to enter
//      the storage engine during secondary oplog application.
//
// Returns the maximum 'ts' a pre-image is allowed to have in order to be safely truncated.
Timestamp getMaxTSEligibleForTruncate(OperationContext* opCtx) {
    Timestamp allDurable =
        Timestamp(opCtx->getServiceContext()->getStorageEngine()->getAllDurableTimestamp());
    auto lastAppliedOpTime = repl::ReplicationCoordinator::get(opCtx)->getMyLastAppliedOpTime();
    return std::min(lastAppliedOpTime.getTimestamp(), allDurable);
}

// Performs a ranged truncate over each expired marker in 'truncateMarkersForNsUUID'. Updates the
// "Output" parameters to communicate the respective docs deleted, bytes deleted, and and maximum
// wall time of documents deleted to the caller.
void truncateExpiredMarkersForNsUUID(
    OperationContext* opCtx,
    std::shared_ptr<PreImagesTruncateMarkersPerNsUUID> truncateMarkersForNsUUID,
    const CollectionPtr& preImagesColl,
    const UUID& nsUUID,
    const RecordId& minRecordIdForNs,
    const Timestamp& maxTSEligibleForTruncate,
    int64_t& totalDocsDeletedOutput,
    int64_t& totalBytesDeletedOutput,
    Date_t& maxWallTimeForNsTruncateOutput) {
    while (auto marker = truncateMarkersForNsUUID->peekOldestMarkerIfNeeded(opCtx)) {
        if (change_stream_pre_image_util::getPreImageTimestamp(marker->lastRecord) >
            maxTSEligibleForTruncate) {
            // The truncate marker contains pre-images part of a data range not yet consistent
            // (i.e. there could be oplog holes or partially applied ranges of the oplog in the
            // range).
            return;
        }

        writeConflictRetry(opCtx, "truncate pre-images collection", preImagesColl->ns(), [&] {
            auto bytesDeleted = marker->bytes;
            auto docsDeleted = marker->records;

            change_stream_pre_image_util::truncateRange(opCtx,
                                                        preImagesColl,
                                                        minRecordIdForNs,
                                                        marker->lastRecord,
                                                        bytesDeleted,
                                                        docsDeleted);

            if (marker->wallTime > maxWallTimeForNsTruncateOutput) {
                maxWallTimeForNsTruncateOutput = marker->wallTime;
            }

            truncateMarkersForNsUUID->popOldestMarker();

            totalDocsDeletedOutput += docsDeleted;
            totalBytesDeletedOutput += bytesDeleted;
        });
    }
}

//  Populates the 'markersMap' by scanning the pre-images collection.
void scanToPopulate(
    OperationContext* opCtx,
    const boost::optional<TenantId> tenantId,
    const CollectionAcquisition& preImagesCollection,
    int32_t minBytesPerMarker,
    ConcurrentSharedValuesMap<UUID, PreImagesTruncateMarkersPerNsUUID, UUID::Hash>& markersMap) {
    const auto nsUUIDs = change_stream_pre_image_util::getNsUUIDs(opCtx, preImagesCollection);
    for (const auto& nsUUID : nsUUIDs) {
        auto initialSetOfMarkers = PreImagesTruncateMarkersPerNsUUID::createInitialMarkersScanning(
            opCtx, preImagesCollection, nsUUID, minBytesPerMarker);

        markersMap.getOrEmplace(nsUUID,
                                tenantId,
                                std::move(initialSetOfMarkers.markers),
                                initialSetOfMarkers.leftoverRecordsCount,
                                initialSetOfMarkers.leftoverRecordsBytes,
                                minBytesPerMarker,
                                CollectionTruncateMarkers::MarkersCreationMethod::Scanning);
    }
}

// Container for samples of pre-images keyed by their 'nsUUID'.
using NsUUIDToSamplesMap = stdx::
    unordered_map<UUID, std::vector<CollectionTruncateMarkers::RecordIdAndWallTime>, UUID::Hash>;

void distributeUnaccountedBytesAndRecords(
    const stdx::unordered_set<UUID, UUID::Hash>& nsUUIDs,
    int64_t preImagesCollNumRecords,
    int64_t preImagesCollDataSize,
    int64_t recordsInMarkersMap,
    int64_t bytesInMarkersMap,
    ConcurrentSharedValuesMap<UUID, PreImagesTruncateMarkersPerNsUUID, UUID::Hash>& markersMap) {
    const auto unaccountedRecords = preImagesCollNumRecords - recordsInMarkersMap;
    const auto unaccountedBytes = preImagesCollDataSize - bytesInMarkersMap;

    if (unaccountedRecords < 0 || unaccountedBytes < 0) {
        // The markers account for more records / bytes than the estimated pre-images collection.
        // Something was off about the sampling logic and this scenario should be investigated.
        LOGV2_WARNING(
            7658603,
            "Pre-images inital truncate markers account for more bytes and/or records than "
            "expected",
            "recordsAcrossMarkers"_attr = recordsInMarkersMap,
            "bytesAcrossMarkers"_attr = bytesInMarkersMap,
            "expectedRecords"_attr = preImagesCollNumRecords,
            "expectedBytes"_attr = preImagesCollDataSize);
        return;
    }

    const auto numNsUUIDs = nsUUIDs.size();
    if (unaccountedRecords == 0 || unaccountedBytes == 0 || numNsUUIDs == 0) {
        return;
    }

    const auto unaccountedRecordsPerNsUUID = unaccountedRecords / numNsUUIDs;
    const auto unaccountedBytesPerNsUUID = unaccountedBytes / numNsUUIDs;
    for (const auto& nsUUID : nsUUIDs) {
        auto nsUUIDMarkers = markersMap.find(nsUUID);
        invariant(nsUUIDMarkers);
        nsUUIDMarkers->updateMarkers(
            unaccountedBytesPerNsUUID, RecordId{}, Date_t{}, unaccountedRecordsPerNsUUID);
    }

    // Account for remaining records and bytes that weren't evenly divisible by 'numNsUUIDs'.
    const auto remainderRecords = unaccountedRecords % numNsUUIDs;
    const auto remainderBytes = unaccountedBytes % numNsUUIDs;
    const auto arbitraryNsUUID = *nsUUIDs.begin();
    auto arbitraryNsUUIDMarkers = markersMap.find(arbitraryNsUUID);
    arbitraryNsUUIDMarkers->updateMarkers(remainderBytes, RecordId{}, Date_t{}, remainderRecords);
}

int64_t countTotalSamples(const NsUUIDToSamplesMap& samplesMap) {
    int64_t totalSamples{0};
    for (const auto& [_, nsUUIDSamples] : samplesMap) {
        totalSamples = totalSamples + nsUUIDSamples.size();
    }
    return totalSamples;
}

void appendSample(const BSONObj& preImageObj, const RecordId& rId, NsUUIDToSamplesMap& samplesMap) {
    auto uuid = change_stream_pre_image_util::getPreImageNsUUID(preImageObj);
    if (auto it = samplesMap.find(uuid); it != samplesMap.end()) {
        it->second.push_back(CollectionTruncateMarkers::RecordIdAndWallTime{
            rId, PreImagesTruncateMarkersPerNsUUID::getWallTime(preImageObj)});
    } else {
        // It's possible concurrent inserts have occurred since the initial point sampling
        // to establish the number of NsUUIDs.
        samplesMap[uuid] = {CollectionTruncateMarkers::RecordIdAndWallTime{
            rId, PreImagesTruncateMarkersPerNsUUID::getWallTime(preImageObj)}};
    }
}

// Iterates over each 'nsUUID' captured by the pre-images in 'rs', and populates the 'samplesMap' to
// include the 'RecordIdAndWallTime' for the most recent pre-image inserted for each 'nsUUID'.
void sampleLastRecordPerNsUUID(OperationContext* opCtx,
                               RecordStore* rs,
                               NsUUIDToSamplesMap& samplesMap) {
    auto cursor = rs->getCursor(opCtx, false /** forward **/);
    boost::optional<Record> record = cursor->next();
    while (record) {
        // As a reverse cursor, the first record we see for a namespace is the highest.
        UUID currentNsUUID = change_stream_pre_image_util::getPreImageNsUUID(record->data.toBson());
        appendSample(record->data.toBson(), record->id, samplesMap);

        RecordId minRecordIdForNsUUID =
            change_stream_pre_image_util::getAbsoluteMinPreImageRecordIdBoundForNs(currentNsUUID)
                .recordId();

        // A reverse exclusive 'seek' will return the previous entry in the collection. This should
        // ensure that the record's id is less than the 'minRecordIdForNsUUID', which positions it
        // exactly at the highest record of the previous collection UUID.
        record = cursor->seek(minRecordIdForNsUUID, SeekableRecordCursor::BoundInclusion::kExclude);
        invariant(!record ||
                  currentNsUUID !=
                      change_stream_pre_image_util::getPreImageNsUUID(record->data.toBson()));
    }
}

// Returns a map of NsUUID to corresponding samples from the 'preImagesCollection'.
//
// Guarantees:
//  (1) The result will contain at least 1 sample per 'nsUUID' in the pre-images collection.
//  (2) For each 'nsUUID', the samples will be ordered as they appear in the underlying pre-images
//  collection.
NsUUIDToSamplesMap gatherOrderedSamplesAcrossNsUUIDs(
    OperationContext* opCtx, const CollectionAcquisition& preImagesCollection, int64_t numSamples) {
    // First, try to obtain 1 sample per 'nsUUID'.
    NsUUIDToSamplesMap samplesMap;
    sampleLastRecordPerNsUUID(
        opCtx, preImagesCollection.getCollectionPtr()->getRecordStore(), samplesMap);
    auto numLastRecords = countTotalSamples(samplesMap);

    Timer lastProgressTimer;

    auto samplingLogIntervalSeconds = gCollectionSamplingLogIntervalSeconds.load();
    auto numSamplesRemaining = numSamples - numLastRecords;
    auto exec = InternalPlanner::sampleCollection(
        opCtx, preImagesCollection, PlanYieldPolicy::YieldPolicy::YIELD_AUTO);

    BSONObj doc;
    RecordId rId;
    for (int i = 0; i < numSamplesRemaining; i++) {
        if (exec->getNext(&doc, &rId) == PlanExecutor::IS_EOF) {
            // This really shouldn't happen unless the collection is empty and the size storer was
            // really off on its collection size estimate.
            break;
        }
        appendSample(doc, rId, samplesMap);
        if (samplingLogIntervalSeconds > 0 &&
            lastProgressTimer.elapsed() >= Seconds(samplingLogIntervalSeconds)) {
            LOGV2(7658600,
                  "Pre-images collection random sampling progress",
                  "namespace"_attr = preImagesCollection.nss(),
                  "completed"_attr = (i + 1),
                  "totalRandomSamples"_attr = numSamplesRemaining,
                  "totalSamples"_attr = numSamples);
            lastProgressTimer.reset();
        }
    }

    for (auto& [_, samples] : samplesMap) {
        std::sort(
            samples.begin(),
            samples.end(),
            [](const CollectionTruncateMarkers::RecordIdAndWallTime& a,
               const CollectionTruncateMarkers::RecordIdAndWallTime& b) { return a.id < b.id; });
    }

    return samplesMap;
}

void updateMarkersMapAggregates(
    const CollectionTruncateMarkers::InitialSetOfMarkers& initialSetOfMarkers,
    int64_t& aggNumRecords,
    int64_t& aggDataSize) {
    for (const auto& marker : initialSetOfMarkers.markers) {
        aggNumRecords = aggNumRecords + marker.records;
        aggDataSize = aggDataSize + marker.bytes;
    }
    aggNumRecords = aggNumRecords + initialSetOfMarkers.leftoverRecordsCount;
    aggDataSize = aggDataSize + initialSetOfMarkers.leftoverRecordsBytes;
}

// Guarantee: Individual truncate markers and metrics for each 'nsUUID' may not be accurate, but
// cumulatively, the total number of records and bytes captured by the 'markersMap' should reflect
// the reported 'preImagesCollNumRecords' and 'preImagesCollDataSize' of the pre-images collection.
//
// Samples the contents of the pre-images collection to populate the 'markersMap'.
void sampleToPopulate(
    OperationContext* opCtx,
    const boost::optional<TenantId> tenantId,
    const CollectionAcquisition& preImagesCollection,
    int64_t preImagesCollDataSize,
    int64_t preImagesCollNumRecords,
    int32_t minBytesPerMarker,
    ConcurrentSharedValuesMap<UUID, PreImagesTruncateMarkersPerNsUUID, UUID::Hash>& markersMap) {
    double avgRecordSize = double(preImagesCollDataSize) / double(preImagesCollNumRecords);
    double estimatedRecordsPerMarker = std::ceil(minBytesPerMarker / avgRecordSize);
    double estimatedBytesPerMarker = estimatedRecordsPerMarker * avgRecordSize;

    uint64_t numSamples =
        (CollectionTruncateMarkers::kRandomSamplesPerMarker * preImagesCollNumRecords) /
        estimatedRecordsPerMarker;

    if (numSamples == 0) {
        LOGV2(8198000,
              "Reverting to scanning for initial pre-images truncate markers. The number of "
              "samples needed is 0",
              "tenantId"_attr = tenantId);

        scanToPopulate(opCtx, tenantId, preImagesCollection, minBytesPerMarker, markersMap);
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    //
    //  PHASE 1: Gather ordered sample points across the 'nsUUIDs' captured in the pre-images
    //  collection.
    //
    //
    //  {nsUUID: <ordered samples, at least 1 per nsUUID>}
    //
    ////////////////////////////////////////////////////////////////////////////////////////////
    const auto orderedSamples =
        gatherOrderedSamplesAcrossNsUUIDs(opCtx, preImagesCollection, numSamples);
    const auto totalSamples = countTotalSamples(orderedSamples);
    if (totalSamples != (int64_t)numSamples) {
        // Given the distribution of pre-images to 'nsUUID', the number of samples collected cannot
        // effectively represent the pre-images collection. Default to scanning instead.
        LOGV2(7658601,
              "Reverting to scanning for initial pre-images truncate markers. The number of "
              "samples collected does not match the desired number of samples",
              "samplesTaken"_attr = totalSamples,
              "samplesDesired"_attr = numSamples,
              "tenantId"_attr = tenantId);
        return scanToPopulate(opCtx, tenantId, preImagesCollection, minBytesPerMarker, markersMap);
    }

    ////////////////////////////////////////////////////////////////////////////////////////////
    //
    //  Phase 2: Use the samples to generate and install initial sets of whole markers for each
    //  nsUUID. The aggregate number of records and bytes across the map aren't expected to match
    //  the reported 'preImagesCollDataSize' and 'preImagesCollNumRecords' yet.
    //
    ////////////////////////////////////////////////////////////////////////////////////////////
    int64_t bytesInMarkersMap{0};
    int64_t recordsInMarkersMap{0};
    stdx::unordered_set<UUID, UUID::Hash> nsUUIDs;
    for (const auto& [nsUUID, samples] : orderedSamples) {
        nsUUIDs.emplace(nsUUID);

        auto initialSetOfMarkers =
            PreImagesTruncateMarkersPerNsUUID::createInitialMarkersFromSamples(
                opCtx, nsUUID, samples, estimatedRecordsPerMarker, estimatedBytesPerMarker);

        updateMarkersMapAggregates(initialSetOfMarkers, recordsInMarkersMap, bytesInMarkersMap);

        markersMap.getOrEmplace(nsUUID,
                                tenantId,
                                std::move(initialSetOfMarkers.markers),
                                0,
                                0,
                                minBytesPerMarker,
                                CollectionTruncateMarkers::MarkersCreationMethod::Sampling);
    }

    ////////////////////////////////////////////////////////////////////////////////////////////
    //
    //  Phase 3: Fix the 'markersMap' so the aggregate number of records and bytes across the
    //  map make up the expected number of records and bytes in the pre-images collection.
    //
    ////////////////////////////////////////////////////////////////////////////////////////////
    distributeUnaccountedBytesAndRecords(nsUUIDs,
                                         preImagesCollNumRecords,
                                         preImagesCollDataSize,
                                         recordsInMarkersMap,
                                         bytesInMarkersMap,
                                         markersMap);
}

// Populates the 'markersMap' with truncate markers covering the entire pre-images collection.
// Only pre-images visible in the thread's initial snapshot of the pre-images collection are
// guaranteed to be covered.
void populateMarkersMap(
    OperationContext* opCtx,
    boost::optional<TenantId> tenantId,
    const CollectionAcquisition& preImagesCollection,
    ConcurrentSharedValuesMap<UUID, PreImagesTruncateMarkersPerNsUUID, UUID::Hash>& markersMap) {

    const auto minBytesPerMarker = gPreImagesCollectionTruncateMarkersMinBytes;

    // Cached size of the pre-images collection. Not guaranteed to be accurate after server restart.
    const auto numRecords = preImagesCollection.getCollectionPtr()->numRecords(opCtx);
    const auto dataSize = preImagesCollection.getCollectionPtr()->dataSize(opCtx);

    // The first method to try when populating the 'markersMap'. Note: sampling can fall back to
    // scanning if the cached collection sizes aren't accurate.
    const auto initialCreationMethod = CollectionTruncateMarkers::computeInitialCreationMethod(
        numRecords, dataSize, minBytesPerMarker);
    LOGV2_INFO(7658604,
               "Decided on initial creation method for pre-images truncate markers initialization",
               "initialCreationMethod"_attr =
                   CollectionTruncateMarkers::toString(initialCreationMethod),
               "dataSize"_attr = dataSize,
               "numRecords"_attr = numRecords,
               "ns"_attr = preImagesCollection.nss(),
               "tenantId"_attr = tenantId);

    if (initialCreationMethod == CollectionTruncateMarkers::MarkersCreationMethod::Sampling) {
        sampleToPopulate(opCtx,
                         tenantId,
                         preImagesCollection,
                         dataSize,
                         numRecords,
                         minBytesPerMarker,
                         markersMap);
    } else {
        // Even if the collection is expected to be empty, try scanning since a table scan provides
        // more accurate results.
        scanToPopulate(opCtx, tenantId, preImagesCollection, minBytesPerMarker, markersMap);
    }
}
}  // namespace

PreImagesTenantMarkers PreImagesTenantMarkers::createMarkers(
    OperationContext* opCtx,
    boost::optional<TenantId> tenantId,
    const CollectionAcquisition& preImagesCollection) {
    invariant(preImagesCollection.exists());
    PreImagesTenantMarkers preImagesTenantMarkers(tenantId, preImagesCollection.uuid());
    populateMarkersMap(opCtx, tenantId, preImagesCollection, preImagesTenantMarkers._markersMap);
    return preImagesTenantMarkers;
}

void PreImagesTenantMarkers::refreshMarkers(OperationContext* opCtx) {
    shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();

    // Use writeConflictRetry since acquiring the collection can yield a WriteConflictException if
    // it races with concurrent catalog changes.
    writeConflictRetry(
        opCtx, "Refreshing the pre image truncate markers in a new snapshot", _tenantNss, [&] {
            // writeConflictRetry automatically abandon's the snapshot before retrying.
            //
            const auto preImagesCollection = acquirePreImagesCollectionForRead(
                opCtx, NamespaceStringOrUUID{_tenantNss.dbName(), _tenantUUID});
            const auto rs = preImagesCollection.getCollectionPtr()->getRecordStore();

            NsUUIDToSamplesMap highestRecordIdAndWallTimeSamples;
            sampleLastRecordPerNsUUID(opCtx, rs, highestRecordIdAndWallTimeSamples);

            for (const auto& [nsUUID, recordIdAndWallTimeVec] : highestRecordIdAndWallTimeSamples) {
                const auto& [highestRid, highestWallTime] = recordIdAndWallTimeVec[0];
                updateOnInsert(highestRid, nsUUID, highestWallTime, 0, 0);
            }
        });
}

PreImagesTruncateStats PreImagesTenantMarkers::truncateExpiredPreImages(OperationContext* opCtx) {
    const auto markersMapSnapshot = _markersMap.getUnderlyingSnapshot();

    // Truncates are untimestamped. Allow multiple truncates to occur.
    shard_role_details::getRecoveryUnit(opCtx)->allowAllUntimestampedWrites();

    invariant(AdmissionContext::get(opCtx).getPriority() == AdmissionContext::Priority::kExempt,
              "Pre-image truncation is critical to cluster health and should not be throttled");

    // Acquire locks before iterating the truncate markers to prevent repeated locking and
    // unlocking for each truncate. By making each call to truncate individually
    // retriable, we reduce the amount of book keeping necessary to rollback truncate marker
    // modifications after a WriteConflictException.
    //
    // There are 2 assumptions which make it safe to hold locks in the current scope.
    //      (1) Since ticket acquisiton is bypassed, we don't contribute to ticket exhaustion by
    //      wrapping each truncate in it's own 'writeConflictRetry()' (see SERVER-65418 for more
    //      details).
    //      (2) The locks will never be yielded by a query, thus there can't be any concurrent DDL
    //      operations to invalidate our collection instance. This is only a risk when
    //      'abandonSnapshot()' is called, which can invalidate the acquired collection instance,
    //      like after a WriteConflictException.
    const auto preImagesCollection = acquirePreImagesCollectionForWrite(
        opCtx, NamespaceStringOrUUID{_tenantNss.dbName(), _tenantUUID});
    const auto& preImagesColl = preImagesCollection.getCollectionPtr();

    // All pre-images with 'ts' <= 'maxTSEligibleForTruncate' are candidates for truncation.
    // However, pre-images with 'ts' > 'maxTSEligibleForTruncate' are unsafe to truncate, as
    // there may be oplog holes or inconsistent data prior to it. Compute the value once, as it
    // requires making an additional call into the storage engine.
    Timestamp maxTSEligibleForTruncate = getMaxTSEligibleForTruncate(opCtx);
    PreImagesTruncateStats stats;
    for (auto& [nsUUID, truncateMarkersForNsUUID] : *markersMapSnapshot) {
        RecordId minRecordId =
            change_stream_pre_image_util::getAbsoluteMinPreImageRecordIdBoundForNs(nsUUID)
                .recordId();

        int64_t docsDeletedForNs = 0;
        int64_t bytesDeletedForNs = 0;
        Date_t maxWallTimeForNsTruncate{};
        truncateExpiredMarkersForNsUUID(opCtx,
                                        truncateMarkersForNsUUID,
                                        preImagesColl,
                                        nsUUID,
                                        minRecordId,
                                        maxTSEligibleForTruncate,
                                        docsDeletedForNs,
                                        bytesDeletedForNs,
                                        maxWallTimeForNsTruncate);

        // Best effort for removing all expired pre-images from 'nsUUID'. If there is a partial
        // marker which can be made into an expired marker, try to remove the new marker as
        // well.
        truncateMarkersForNsUUID->createPartialMarkerIfNecessary(opCtx);
        truncateExpiredMarkersForNsUUID(opCtx,
                                        truncateMarkersForNsUUID,
                                        preImagesColl,
                                        nsUUID,
                                        minRecordId,
                                        maxTSEligibleForTruncate,
                                        docsDeletedForNs,
                                        bytesDeletedForNs,
                                        maxWallTimeForNsTruncate);

        if (maxWallTimeForNsTruncate > stats.maxStartWallTime) {
            stats.maxStartWallTime = maxWallTimeForNsTruncate;
        }
        stats.docsDeleted = stats.docsDeleted + docsDeletedForNs;
        stats.bytesDeleted = stats.bytesDeleted + bytesDeletedForNs;
        stats.scannedInternalCollections++;

        // If the source collection doesn't exist and there's no more data to erase we can
        // safely remove the markers. Perform a final truncate to remove all elements just in
        // case.
        if (CollectionCatalog::get(opCtx)->lookupCollectionByUUID(opCtx, nsUUID) == nullptr &&
            truncateMarkersForNsUUID->isEmpty()) {

            RecordId maxRecordId =
                change_stream_pre_image_util::getAbsoluteMaxPreImageRecordIdBoundForNs(nsUUID)
                    .recordId();

            writeConflictRetry(opCtx, "final truncate", preImagesColl->ns(), [&] {
                // Call creates it's own writeUnitOfWork.
                change_stream_pre_image_util::truncateRange(
                    opCtx, preImagesColl, minRecordId, maxRecordId, 0, 0);
            });

            _markersMap.erase(nsUUID);
        }
    }

    return stats;
}

void PreImagesTenantMarkers::updateOnInsert(const RecordId& recordId,
                                            const UUID& nsUUID,
                                            Date_t wallTime,
                                            int64_t bytesInserted,
                                            int64_t numRecords) {
    auto nsUUIDMarkers = _markersMap.find(nsUUID);
    if (!nsUUIDMarkers) {
        nsUUIDMarkers = _markersMap.getOrEmplace(
            nsUUID,
            _tenantId,
            std::deque<CollectionTruncateMarkers::Marker>{},
            0,
            0,
            gPreImagesCollectionTruncateMarkersMinBytes,
            CollectionTruncateMarkers::MarkersCreationMethod::EmptyCollection);
    }
    nsUUIDMarkers->updateMarkers(bytesInserted, recordId, wallTime, numRecords);
}
}  // namespace mongo
