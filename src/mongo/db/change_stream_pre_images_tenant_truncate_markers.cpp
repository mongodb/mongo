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

#include "mongo/db/admission/execution_admission_context.h"
#include "mongo/db/change_stream_pre_image_util.h"
#include "mongo/db/change_stream_pre_images_truncate_markers_per_nsUUID.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/storage/collection_truncate_markers.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/util/concurrent_shared_values_map.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace {

// Acquires the pre-images collection given 'nsOrUUID'. When provided a UUID, throws
// NamespaceNotFound if the collection is dropped.
auto acquirePreImagesCollectionForRead(OperationContext* opCtx, const UUID& uuid) {
    return acquireCollection(
        opCtx,
        CollectionAcquisitionRequest(
            NamespaceStringOrUUID{NamespaceString::kChangeStreamPreImagesNamespace.dbName(), uuid},
            PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
            repl::ReadConcernArgs::get(opCtx),
            AcquisitionPrerequisites::kRead),
        MODE_IS);
}
auto acquirePreImagesCollectionForWrite(OperationContext* opCtx, const UUID& uuid) {
    return acquireCollection(
        opCtx,
        CollectionAcquisitionRequest(
            NamespaceStringOrUUID{NamespaceString::kChangeStreamPreImagesNamespace.dbName(), uuid},
            PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
            repl::ReadConcernArgs::get(opCtx),
            AcquisitionPrerequisites::kUnreplicatedWrite),
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

}  // namespace

namespace pre_image_marker_initialization_internal {
// Given the expected 'numRecords' and 'dataSize' of the pre-images collection, and the number of
// 'recordsInMarkersMap' and 'bytesInMarkersMap', distributes the difference across truncate markers
// so the resulting 'markersMap' accounts for the total 'numRecords' and 'dataSize'.
void distributeUnaccountedRecordsAndBytes(
    const stdx::unordered_set<UUID, UUID::Hash>& nsUUIDs,
    const UUID& preImagesCollectionUUID,
    int64_t numRecords,
    int64_t dataSize,
    int64_t recordsInMarkersMap,
    int64_t bytesInMarkersMap,
    ConcurrentSharedValuesMap<UUID, PreImagesTruncateMarkersPerNsUUID, UUID::Hash>& markersMap) {
    const auto unaccountedRecords = numRecords - recordsInMarkersMap;
    const auto unaccountedBytes = dataSize - bytesInMarkersMap;

    if (unaccountedRecords < 0 || unaccountedBytes < 0) {
        // The markers account for more records / bytes than the estimated pre-images collection.
        // Something was off about the sampling logic and this scenario should be investigated.
        LOGV2_WARNING(
            7658603,
            "Pre-images inital truncate markers account for more bytes and/or records than "
            "expected",
            "preImagesCollectionUUID"_attr = preImagesCollectionUUID,
            "recordsAcrossMarkers"_attr = recordsInMarkersMap,
            "bytesAcrossMarkers"_attr = bytesInMarkersMap,
            "expectedRecords"_attr = numRecords,
            "expectedBytes"_attr = dataSize);
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

int64_t countTotalSamples(
    const stdx::unordered_map<UUID, std::vector<RecordIdAndWallTime>, UUID::Hash>& samplesMap) {
    int64_t totalSamples{0};
    for (const auto& [_, nsUUIDSamples] : samplesMap) {
        totalSamples = totalSamples + nsUUIDSamples.size();
    }
    return totalSamples;
}

void appendSample(
    const BSONObj& preImageObj,
    const RecordId& rId,
    stdx::unordered_map<UUID, std::vector<RecordIdAndWallTime>, UUID::Hash>& samplesMap) {
    const auto uuid = change_stream_pre_image_util::getPreImageNsUUID(preImageObj);
    const auto wallTime = PreImagesTruncateMarkersPerNsUUID::getWallTime(preImageObj);
    const auto ridAndWall = RecordIdAndWallTime{rId, wallTime};

    if (auto it = samplesMap.find(uuid); it != samplesMap.end()) {
        it->second.push_back(std::move(ridAndWall));
    } else {
        samplesMap.emplace(uuid, std::vector<RecordIdAndWallTime>{std::move(ridAndWall)});
    }
}

void updateMarkersMapAggregates(
    const PreImagesTruncateMarkersPerNsUUID::InitialSetOfMarkers& initialSetOfMarkers,
    int64_t& aggNumRecords,
    int64_t& aggDataSize) {
    for (const auto& marker : initialSetOfMarkers.markers) {
        aggNumRecords = aggNumRecords + marker.records;
        aggDataSize = aggDataSize + marker.bytes;
    }
    aggNumRecords = aggNumRecords + initialSetOfMarkers.leftoverRecordsCount;
    aggDataSize = aggDataSize + initialSetOfMarkers.leftoverRecordsBytes;
}

stdx::unordered_map<UUID, RecordIdAndWallTime, UUID::Hash> sampleLastRecordPerNsUUID(
    OperationContext* opCtx, const CollectionAcquisition& preImagesCollection) {
    const auto rs = preImagesCollection.getCollectionPtr()->getRecordStore();
    stdx::unordered_map<UUID, RecordIdAndWallTime, UUID::Hash> lastRecords;

    auto cursor =
        rs->getCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx), false /** forward **/);
    boost::optional<Record> record = cursor->next();

    while (record) {
        // As a reverse cursor, the first record we see for a namespace is the highest.
        UUID currentNsUUID = change_stream_pre_image_util::getPreImageNsUUID(record->data.toBson());
        auto sample = PreImagesTruncateMarkersPerNsUUID::getRecordIdAndWallTime(*record);
        lastRecords.insert({currentNsUUID, std::move(sample)});

        RecordId minRecordIdForCurrentNsUUID =
            change_stream_pre_image_util::getAbsoluteMinPreImageRecordIdBoundForNs(currentNsUUID)
                .recordId();

        // A reverse exclusive 'seek' will return the previous entry in the collection. This
        // should ensure that the record's id is less than the 'minRecordIdForCurrentNsUUID', which
        // positions it exactly at the highest record of the previous collection UUID.
        record = cursor->seek(minRecordIdForCurrentNsUUID,
                              SeekableRecordCursor::BoundInclusion::kExclude);
        invariant(!record ||
                  currentNsUUID !=
                      change_stream_pre_image_util::getPreImageNsUUID(record->data.toBson()));
    }
    return lastRecords;
}

stdx::unordered_map<UUID, std::vector<RecordIdAndWallTime>, UUID::Hash> collectPreImageSamples(
    OperationContext* opCtx,
    const CollectionAcquisition& preImagesCollection,
    int64_t targetNumSamples) {
    const auto nsUUIDLastRecords = sampleLastRecordPerNsUUID(opCtx, preImagesCollection);

    stdx::unordered_map<UUID, std::vector<RecordIdAndWallTime>, UUID::Hash> samples;
    for (const auto& [uuid, ridAndWall] : nsUUIDLastRecords) {
        // Ensure 'samples' capture the last record of each nsUUID.
        samples.emplace(uuid, std::vector<RecordIdAndWallTime>{ridAndWall});
    }

    const int64_t numLastRecords = nsUUIDLastRecords.size();
    const int64_t numRandomSamples = targetNumSamples - numLastRecords;
    if (numRandomSamples <= 0) {
        return samples;
    }

    auto exec = InternalPlanner::sampleCollection(
        opCtx, preImagesCollection, PlanYieldPolicy::YieldPolicy::YIELD_AUTO);
    Timer lastProgressTimer;
    const auto samplingLogIntervalSeconds = gCollectionSamplingLogIntervalSeconds.load();
    BSONObj preImageObj;
    RecordId rId;
    for (int i = 0; i < numRandomSamples; i++) {
        if (exec->getNext(&preImageObj, &rId) == PlanExecutor::IS_EOF) {
            // This really shouldn't happen unless the collection is empty and the size storer was
            // really off on its collection size estimate.
            break;
        }
        appendSample(preImageObj, rId, samples);
        if (samplingLogIntervalSeconds > 0 &&
            lastProgressTimer.elapsed() >= Seconds(samplingLogIntervalSeconds)) {
            LOGV2(7658600,
                  "Pre-images collection random sampling progress",
                  "namespace"_attr = preImagesCollection.nss(),
                  "preImagesCollectionUUID"_attr = preImagesCollection.uuid(),
                  "randomSamplesCompleted"_attr = (i + 1),
                  "targetRandomSamples"_attr = numRandomSamples,
                  "targetNumSamples"_attr = targetNumSamples);
            lastProgressTimer.reset();
        }
    }

    // Order each sample.
    for (auto& [_, samplesPerNsUUID] : samples) {
        std::sort(
            samplesPerNsUUID.begin(),
            samplesPerNsUUID.end(),
            [](const RecordIdAndWallTime& a, const RecordIdAndWallTime& b) { return a.id < b.id; });
    }

    return samples;
}

void populateByScanning(
    OperationContext* opCtx,
    const CollectionAcquisition& preImagesCollection,
    int32_t minBytesPerMarker,
    ConcurrentSharedValuesMap<UUID, PreImagesTruncateMarkersPerNsUUID, UUID::Hash>& markersMap) {
    const auto nsUUIDs = change_stream_pre_image_util::getNsUUIDs(opCtx, preImagesCollection);
    for (const auto& nsUUID : nsUUIDs) {
        auto initialSetOfMarkers = PreImagesTruncateMarkersPerNsUUID::createInitialMarkersScanning(
            opCtx, preImagesCollection, nsUUID, minBytesPerMarker);

        markersMap.getOrEmplace(nsUUID, nsUUID, std::move(initialSetOfMarkers), minBytesPerMarker);
    }
}

void populateBySampling(
    OperationContext* opCtx,
    const CollectionAcquisition& preImagesCollection,
    int64_t numRecords,
    int64_t dataSize,
    int32_t minBytesPerMarker,
    uint64_t randomSamplesPerMarker,
    ConcurrentSharedValuesMap<UUID, PreImagesTruncateMarkersPerNsUUID, UUID::Hash>& markersMap) {
    if (dataSize < 1 || numRecords < 1 || dataSize < numRecords) {
        // Safeguard against non-sensical and divide by 0 computations since 'numRecords' and
        // 'dataSize' aren't guaranteed to be accurate after unclean shutdown.
        LOGV2(9528900,
              "Reverting to scanning for initial pre-images truncate markers. The tracked number "
              "of records and bytes in the pre-images collection are not compatible with sampling",
              "preImagesCollectionUUID"_attr = preImagesCollection.uuid(),
              "numRecords"_attr = numRecords,
              "dataSize"_attr = dataSize);
        return populateByScanning(opCtx, preImagesCollection, minBytesPerMarker, markersMap);
    }
    double avgRecordSize = double(dataSize) / double(numRecords);
    double estimatedRecordsPerMarker = std::ceil(minBytesPerMarker / avgRecordSize);
    double estimatedBytesPerMarker = estimatedRecordsPerMarker * avgRecordSize;
    uint64_t numSamples = (randomSamplesPerMarker * numRecords) / estimatedRecordsPerMarker;

    if (numSamples == 0) {
        LOGV2(8198000,
              "Reverting to scanning for initial pre-images truncate markers. The number of "
              "samples needed is 0",
              "preImagesCollectionUUID"_attr = preImagesCollection.uuid());

        return populateByScanning(opCtx, preImagesCollection, minBytesPerMarker, markersMap);
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    //
    //  PHASE 1: Gather ordered sample points across the 'nsUUIDs' captured in the pre-images
    //  collection.
    ////////////////////////////////////////////////////////////////////////////////////////////

    const auto samples = collectPreImageSamples(opCtx, preImagesCollection, numSamples);
    const auto totalSamples = countTotalSamples(samples);
    if (totalSamples != (int64_t)numSamples) {
        // Given the distribution of pre-images to 'nsUUID', the number of samples collected cannot
        // effectively represent the pre-images collection. Default to scanning instead.
        LOGV2(7658601,
              "Reverting to scanning for initial pre-images truncate markers. The number of "
              "samples collected does not match the desired number of samples",
              "samplesTaken"_attr = totalSamples,
              "samplesDesired"_attr = numSamples,
              "preImagesCollectionUUID"_attr = preImagesCollection.uuid());
        return populateByScanning(opCtx, preImagesCollection, minBytesPerMarker, markersMap);
    }

    ////////////////////////////////////////////////////////////////////////////////////////////
    //
    //  Phase 2: Use the samples to generate and install initial sets of whole markers for each
    //  nsUUID. The aggregate number of records and bytes across the map aren't expected to match
    //  the reported 'dataSize' and 'numRecords' yet.
    //
    ////////////////////////////////////////////////////////////////////////////////////////////
    int64_t recordsInMarkersMap{0};
    int64_t bytesInMarkersMap{0};
    stdx::unordered_set<UUID, UUID::Hash> nsUUIDs;
    for (const auto& [nsUUID, nsUUIDSamples] : samples) {
        nsUUIDs.emplace(nsUUID);

        auto initialSetOfMarkers =
            PreImagesTruncateMarkersPerNsUUID::createInitialMarkersFromSamples(
                opCtx,
                preImagesCollection.uuid(),
                nsUUID,
                nsUUIDSamples,
                estimatedRecordsPerMarker,
                estimatedBytesPerMarker,
                randomSamplesPerMarker);

        updateMarkersMapAggregates(initialSetOfMarkers, recordsInMarkersMap, bytesInMarkersMap);

        markersMap.getOrEmplace(nsUUID, nsUUID, std::move(initialSetOfMarkers), minBytesPerMarker);
    }

    ////////////////////////////////////////////////////////////////////////////////////////////
    //
    //  Phase 3: Fix the 'markersMap' so the aggregate number of records and bytes across the
    //  map make up the expected number of records and bytes in the pre-images collection.
    //
    ////////////////////////////////////////////////////////////////////////////////////////////
    distributeUnaccountedRecordsAndBytes(nsUUIDs,
                                         preImagesCollection.uuid(),
                                         numRecords,
                                         dataSize,
                                         recordsInMarkersMap,
                                         bytesInMarkersMap,
                                         markersMap);
}

// Populates the 'markersMap' with truncate markers covering the entire pre-images collection.
// Only pre-images visible in the thread's initial snapshot of the pre-images collection are
// guaranteed to be covered.
void populateMarkersMap(
    OperationContext* opCtx,
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
               "preImagesCollectionUUID"_attr = preImagesCollection.uuid());

    if (initialCreationMethod == CollectionTruncateMarkers::MarkersCreationMethod::Sampling) {
        pre_image_marker_initialization_internal::populateBySampling(
            opCtx,
            preImagesCollection,
            numRecords,
            dataSize,
            minBytesPerMarker,
            CollectionTruncateMarkers::kRandomSamplesPerMarker,
            markersMap);
    } else {
        // Even if the collection is expected to be empty, try scanning since a table scan provides
        // more accurate results.
        pre_image_marker_initialization_internal::populateByScanning(
            opCtx, preImagesCollection, minBytesPerMarker, markersMap);
    }
}
}  // namespace pre_image_marker_initialization_internal

PreImagesTenantMarkers PreImagesTenantMarkers::createMarkers(
    OperationContext* opCtx, const CollectionAcquisition& preImagesCollection) {
    invariant(preImagesCollection.exists());
    PreImagesTenantMarkers preImagesTenantMarkers(preImagesCollection.uuid());
    pre_image_marker_initialization_internal::populateMarkersMap(
        opCtx, preImagesCollection, preImagesTenantMarkers._markersMap);
    return preImagesTenantMarkers;
}

void PreImagesTenantMarkers::refreshMarkers(OperationContext* opCtx) {
    shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();

    // Use writeConflictRetry since acquiring the collection can yield a WriteConflictException if
    // it races with concurrent catalog changes.
    writeConflictRetry(opCtx,
                       "Refreshing the pre image truncate markers in a new snapshot",
                       NamespaceString::kChangeStreamPreImagesNamespace,
                       [&] {
                           // writeConflictRetry automatically abandon's the snapshot before
                           // retrying.
                           //
                           const auto preImagesCollection =
                               acquirePreImagesCollectionForRead(opCtx, _preImagesCollectionUUID);

                           const auto nsUUIDs =
                               change_stream_pre_image_util::getNsUUIDs(opCtx, preImagesCollection);
                           for (const auto& nsUUID : nsUUIDs) {
                               // Account for records inserted into an 'nsUUID' not tracked during
                               // the initial construction of the markers.
                               auto nsUUIDMarkers = _markersMap.getOrEmplace(
                                   nsUUID,
                                   nsUUID,
                                   PreImagesTruncateMarkersPerNsUUID::InitialSetOfMarkers{},
                                   gPreImagesCollectionTruncateMarkersMinBytes);
                               nsUUIDMarkers->refreshHighestTrackedRecord(opCtx,
                                                                          preImagesCollection);
                           }
                       });
}

PreImagesTruncateStats PreImagesTenantMarkers::truncateExpiredPreImages(OperationContext* opCtx) {
    const auto markersMapSnapshot = _markersMap.getUnderlyingSnapshot();

    // Truncates are untimestamped. Allow multiple truncates to occur.
    shard_role_details::getRecoveryUnit(opCtx)->allowAllUntimestampedWrites();

    invariant(ExecutionAdmissionContext::get(opCtx).getPriority() ==
                  AdmissionContext::Priority::kExempt,
              "Pre-image truncation is critical to cluster health and should not be throttled");

    // Acquire locks before iterating the truncate markers to prevent repeated locking and
    // unlocking for each truncate. By making each call to truncate individually
    // retriable, we reduce the amount of book keeping necessary to rollback truncate marker
    // modifications after a WriteConflictException.
    //
    // There are 2 assumptions which make it safe to hold locks in the current scope.
    //      (1) Since ticket acquisition is bypassed, we don't contribute to ticket exhaustion by
    //      wrapping each truncate in it's own 'writeConflictRetry()' (see SERVER-65418 for more
    //      details).
    //      (2) The locks will never be yielded by a query, thus there can't be any concurrent DDL
    //      operations to invalidate our collection instance. This is only a risk when
    //      'abandonSnapshot()' is called, which can invalidate the acquired collection instance,
    //      like after a WriteConflictException.
    const auto preImagesCollection =
        acquirePreImagesCollectionForWrite(opCtx, _preImagesCollectionUUID);
    const auto& preImagesColl = preImagesCollection.getCollectionPtr();

    PreImagesTruncateStats stats;

    // All pre-images with 'ts' <= 'maxTSEligibleForTruncate' are candidates for truncation.
    // However, pre-images with 'ts' > 'maxTSEligibleForTruncate' are unsafe to truncate, as
    // there may be oplog holes or inconsistent data prior to it. Compute the value once, as it
    // requires making an additional call into the storage engine.
    Timestamp maxTSEligibleForTruncate = getMaxTSEligibleForTruncate(opCtx);
    stats.maxTimestampEligibleForTruncate = maxTSEligibleForTruncate;

    // Truncate markers can be generated with data that is later rolled back via rollback-to-stable.
    // This behavior is acceptable given the following:
    //      (1) Only expired data is truncated (expire by seconds or oldest oplog TS).
    //      (2) If a marker's 'lastRecord' is rolled back, it's wallTime or ts field will
    //      eventually expire. An expired marker's 'lastRecord' serves as an upper bound for the
    //      truncate range. Even if the 'lastRecord' doesn't exist anymore, all pre-images older
    //      than it are truncated for the nsUUID.
    //          . Caveat: Size metadata isn't accurate if pre-image inserts are rolled back. It will
    //          eventually converge to a correct state in absence of another rollback-to-stable.
    //      (3) If a truncate is issued on data that is later rolled back, unexpired pre-images will
    //      be rolled back in the process. From the stable timestamp, oplog entries will be replayed
    //      and re-inserted into truncate markers (mirroring truncate behavior in a stable state).
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
        nsUUIDMarkers =
            _markersMap.getOrEmplace(nsUUID,
                                     nsUUID,
                                     PreImagesTruncateMarkersPerNsUUID::InitialSetOfMarkers{},
                                     gPreImagesCollectionTruncateMarkersMinBytes);
    }
    nsUUIDMarkers->updateMarkers(bytesInserted, recordId, wallTime, numRecords);
}
}  // namespace mongo
