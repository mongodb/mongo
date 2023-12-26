/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/change_stream_pre_images_truncate_manager.h"

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <algorithm>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <cmath>
#include <deque>
#include <type_traits>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <boost/optional/optional.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/change_stream_pre_image_util.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/record_id_bound.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/collection_truncate_markers.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace {
struct InitialSamplingEstimates {
    int64_t numRecords;
    int64_t dataSize;
    int64_t estimatedRecordsPerMarker;
    int64_t estimatedBytesPerMarker;
    int64_t minBytesPerMarker;
};

// Container for samples of pre-images keyed by their 'nsUUID'.
using NsUUIDToSamplesMap = stdx::
    unordered_map<UUID, std::vector<CollectionTruncateMarkers::RecordIdAndWallTime>, UUID::Hash>;

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
    auto cursor = rs->getCursor(opCtx, true /** forward **/);
    boost::optional<Record> record{};
    while ((record = cursor->next())) {
        UUID currentNsUUID = change_stream_pre_image_util::getPreImageNsUUID(record->data.toBson());
        RecordId maxRecordIdForCurrentNsUUID =
            change_stream_pre_image_util::getAbsoluteMaxPreImageRecordIdBoundForNs(currentNsUUID)
                .recordId();

        // A forward 'seekNear' will return the previous entry if one does not match exactly. This
        // should ensure that the 'record's id is greater than the 'maxRecordIdForCurrentNsUUID' and
        // no less than the initial record for 'currentNsUUID'.
        record = cursor->seekNear(maxRecordIdForCurrentNsUUID);
        invariant(record);
        invariant(currentNsUUID ==
                  change_stream_pre_image_util::getPreImageNsUUID(record->data.toBson()));
        appendSample(record->data.toBson(), record->id, samplesMap);
    }
}

int64_t getBytesAccountedFor(
    const CollectionTruncateMarkers::InitialSetOfMarkers& initialSetOfMarkers) {
    int64_t totalBytes{0};
    for (const auto& marker : initialSetOfMarkers.markers) {
        totalBytes = totalBytes + marker.bytes;
    }
    totalBytes = totalBytes + initialSetOfMarkers.leftoverRecordsBytes;
    return totalBytes;
}

int64_t getRecordsAccountedFor(
    const CollectionTruncateMarkers::InitialSetOfMarkers& initialSetOfMarkers) {
    int64_t totalRecords{0};
    for (const auto& marker : initialSetOfMarkers.markers) {
        totalRecords = totalRecords + marker.records;
    }
    totalRecords = totalRecords + initialSetOfMarkers.leftoverRecordsCount;
    return totalRecords;
}

// Returns a map of NsUUID to corresponding samples from the 'preImagesCollectionPtr'.
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

// Each 'PreImagesTruncateMarkersPerNsUUID' accounts for a set of "whole truncate markers" as well
// as the leftover bytes and records not yet captured in a "whole" truncate marker (aka a partial
// marker).
//
// The 'initialEstimates' specifies the estimated number of samples needed to generate a whole
// marker.
//
// Given a set of samples for each 'nsUUID', returns a map with 'PreImagesTruncateMarkersPerNsUUID'
// for each 'nsUUID'. The created 'PreImagesTruncateMarkersPerNsUUID's will only generate whole
// markers. All partial markers will be empty in the result.
PreImagesTruncateManager::TenantTruncateMarkers createWholeMarkersFromSamples(
    OperationContext* opCtx,
    boost::optional<TenantId> tenantId,
    const NsUUIDToSamplesMap& samplesMap,
    const InitialSamplingEstimates& initialEstimates,
    int64_t& wholeMarkersCreatedOutput) {
    PreImagesTruncateManager::TenantTruncateMarkers truncateMarkersMap;
    for (const auto& [nsUUID, samples] : samplesMap) {
        auto initialWholeMarkers =
            PreImagesTruncateMarkersPerNsUUID::createInitialMarkersFromSamples(
                opCtx,
                nsUUID,
                samples,
                initialEstimates.estimatedRecordsPerMarker,
                initialEstimates.estimatedBytesPerMarker);
        wholeMarkersCreatedOutput = wholeMarkersCreatedOutput + initialWholeMarkers.markers.size();

        auto truncateMarkersForNsUUID = std::make_shared<PreImagesTruncateMarkersPerNsUUID>(
            tenantId,
            std::move(initialWholeMarkers.markers),
            0,
            0,
            initialEstimates.minBytesPerMarker,
            CollectionTruncateMarkers::MarkersCreationMethod::Sampling);
        truncateMarkersMap.emplace(nsUUID, std::move(truncateMarkersForNsUUID));
    }
    return truncateMarkersMap;
}

void distributeUnaccountedBytesAndRecords(
    OperationContext* opCtx,
    RecordStore* rs,
    int64_t recordsAccountedForByMarkers,
    int64_t bytesAccountedForByMarkers,
    PreImagesTruncateManager::TenantTruncateMarkers& tenantTruncateMarkers) {
    auto numRecords = rs->numRecords(opCtx);
    auto dataSize = rs->dataSize(opCtx);
    auto totalLeftoverRecords = numRecords - recordsAccountedForByMarkers;
    auto totalLeftoverBytes = dataSize - bytesAccountedForByMarkers;

    if (totalLeftoverRecords < 0 || totalLeftoverBytes < 0) {
        // The 'numRecords' and 'dataSize' are both retrieved by the SizeStorer, which
        // can be incorrect after startup. If the records/ bytes accounted for were retrieved via
        // scanning, its completely possible they are more accurate than the metrics reported. If
        // they were retrieved from sampling, this scenario should be investigated further.
        //
        // Early exit if there are no more bytes / records to distribute across partial markers.
        LOGV2_INFO(7658603,
                   "Pre-images inital truncate markers account for more bytes and/or records than "
                   "reported by the size storer",
                   "initialMarkersRecordsAccountedFor"_attr = recordsAccountedForByMarkers,
                   "initialMarkersBytesAccountedFor"_attr = bytesAccountedForByMarkers,
                   "reportedNumRecords"_attr = numRecords,
                   "reportedDataSize"_attr = dataSize);
        return;
    }

    auto numNsUUIDs = tenantTruncateMarkers.size();
    if (totalLeftoverRecords == 0 || totalLeftoverBytes == 0 || numNsUUIDs == 0) {
        return;
    }

    auto leftoverRecordsPerNsUUID = totalLeftoverRecords / numNsUUIDs;
    auto leftoverBytesPerNsUUID = totalLeftoverBytes / numNsUUIDs;

    for (auto& [nsUUID, preImagesTruncateMarkersPerNsUUID] : tenantTruncateMarkers) {
        preImagesTruncateMarkersPerNsUUID->updateMarkers(
            leftoverBytesPerNsUUID, RecordId{}, Date_t{}, leftoverRecordsPerNsUUID);
    }

    // Arbitrarily append the remaining records and bytes to one of the marker sets.
    int64_t remainderRecords = totalLeftoverRecords % numNsUUIDs;
    int64_t remainderBytes = totalLeftoverBytes % numNsUUIDs;
    tenantTruncateMarkers.begin()->second->updateMarkers(
        remainderBytes, RecordId{}, Date_t{}, remainderRecords);
}

void distributeUnaccountedBytesAndRecords(
    OperationContext* opCtx,
    RecordStore* rs,
    const InitialSamplingEstimates& initialSamplingEstimates,
    int64_t numWholeMarkersCreated,
    PreImagesTruncateManager::TenantTruncateMarkers& tenantTruncateMarkers) {
    auto recordsAccountedForByWholeMarkers =
        numWholeMarkersCreated * initialSamplingEstimates.estimatedRecordsPerMarker;
    auto bytesAccountedForByWholeMarkers =
        numWholeMarkersCreated * initialSamplingEstimates.estimatedBytesPerMarker;

    distributeUnaccountedBytesAndRecords(opCtx,
                                         rs,
                                         recordsAccountedForByWholeMarkers,
                                         bytesAccountedForByWholeMarkers,
                                         tenantTruncateMarkers);
}

PreImagesTruncateManager::TenantTruncateMarkers generateTruncateMarkersForTenantScanning(
    OperationContext* opCtx,
    boost::optional<TenantId> tenantId,
    const CollectionAcquisition& preImagesCollection) {
    auto rs = preImagesCollection.getCollectionPtr()->getRecordStore();

    PreImagesTruncateManager::TenantTruncateMarkers truncateMap;
    auto minBytesPerMarker = gPreImagesCollectionTruncateMarkersMinBytes;

    // Number of bytes and records accounted for by truncate markers.
    int64_t numBytesAcrossMarkers{0};
    int64_t numRecordsAcrossMarkers{0};

    boost::optional<UUID> currentCollectionUUID = boost::none;

    // Step 1: perform a forward scan of the collection. This could take a while for larger
    // collections.
    Date_t firstWallTime{};
    while ((currentCollectionUUID = change_stream_pre_image_util::findNextCollectionUUID(
                opCtx,
                &preImagesCollection.getCollectionPtr(),
                currentCollectionUUID,
                firstWallTime))) {
        auto initialSetOfMarkers = PreImagesTruncateMarkersPerNsUUID::createInitialMarkersScanning(
            opCtx, preImagesCollection, currentCollectionUUID.get(), minBytesPerMarker);

        numBytesAcrossMarkers = numBytesAcrossMarkers + getBytesAccountedFor(initialSetOfMarkers);
        numRecordsAcrossMarkers =
            numRecordsAcrossMarkers + getRecordsAccountedFor(initialSetOfMarkers);

        auto truncateMarkers = std::make_shared<PreImagesTruncateMarkersPerNsUUID>(
            tenantId,
            std::move(initialSetOfMarkers.markers),
            initialSetOfMarkers.leftoverRecordsCount,
            initialSetOfMarkers.leftoverRecordsBytes,
            minBytesPerMarker,
            CollectionTruncateMarkers::MarkersCreationMethod::Scanning);
        truncateMap.emplace(currentCollectionUUID.get(), truncateMarkers);
    }

    // Step 2: See if there are records unaccounted for in the initial markers. This can happen if
    // there are concurrent inserts into a given 'nsUUID' after the segment was scanned.
    distributeUnaccountedBytesAndRecords(
        opCtx, rs, numRecordsAcrossMarkers, numBytesAcrossMarkers, truncateMap);

    return truncateMap;
}

PreImagesTruncateManager::TenantTruncateMarkers generateTruncateMarkersForTenantSampling(
    OperationContext* opCtx,
    boost::optional<TenantId> tenantId,
    const CollectionAcquisition& preImagesCollection,
    InitialSamplingEstimates&& initialEstimates) {

    uint64_t numSamples =
        (CollectionTruncateMarkers::kRandomSamplesPerMarker * initialEstimates.numRecords) /
        initialEstimates.estimatedRecordsPerMarker;

    ///////////////////////////////////////////////////////////////
    //
    //  PHASE 1: Gather ordered sample points across the 'nsUUIDs' captured in the pre-images
    //  collection.
    //
    //
    //  {nsUUID: <ordered samples, at least 1 per nsUUID>}
    //
    ///////////////////////////////////////////////////////////////
    auto orderedSamples = gatherOrderedSamplesAcrossNsUUIDs(opCtx, preImagesCollection, numSamples);
    auto totalSamples = countTotalSamples(orderedSamples);
    if (totalSamples != (int64_t)numSamples) {
        // Given the distribution of pre-images to 'nsUUID', the number of samples collected cannot
        // effectively represent the pre-images collection. Default to scanning instead.
        LOGV2(7658601,
              "Reverting to scanning for initial pre-images truncate markers. The number of "
              "samples collected does not match the desired number of samples",
              "samplesTaken"_attr = totalSamples,
              "samplesDesired"_attr = numSamples);
        return generateTruncateMarkersForTenantScanning(opCtx, tenantId, preImagesCollection);
    }

    ////////////////////////////////////////////////////////////////
    //
    //  Phase 2: Create the whole truncate markers from the samples generated according to the
    //  'initialEstimates'.
    //
    ////////////////////////////////////////////////////////////////

    int64_t wholeMarkersCreated{0};
    auto tenantTruncateMarkers = createWholeMarkersFromSamples(
        opCtx, tenantId, orderedSamples, initialEstimates, wholeMarkersCreated);

    ////////////////////////////////////////////////////////////////
    //
    //  Phase 3: Update 'tenantTruncateMarkers' partial markers with the remaining bytes and records
    //  not accounted for in the 'wholeMarkersCreated' and distribute them across the 'nsUUID's.
    //
    ////////////////////////////////////////////////////////////////
    auto rs = preImagesCollection.getCollectionPtr()->getRecordStore();
    distributeUnaccountedBytesAndRecords(
        opCtx, rs, initialEstimates, wholeMarkersCreated, tenantTruncateMarkers);

    return tenantTruncateMarkers;
}

// Guarantee: Individual truncate markers and metrics for each 'nsUUID' may not be accurate, but
// cumulatively, the total 'dataSize' and 'numRecords' captured by the set of
// 'TenantTruncateMarkers' should reflect the actual 'dataSize' and 'numRecords' reported by the
// SizeStorer.
PreImagesTruncateManager::TenantTruncateMarkers generateTruncateMarkersForTenant(
    OperationContext* opCtx,
    boost::optional<TenantId> tenantId,
    const CollectionAcquisition& preImageCollection) {
    auto minBytesPerMarker = gPreImagesCollectionTruncateMarkersMinBytes;
    auto rs = preImageCollection.getCollectionPtr()->getRecordStore();
    long long numRecords = rs->numRecords(opCtx);
    long long dataSize = rs->dataSize(opCtx);

    // The creationMethod returned is the initial creationMethod to try. However, there is no
    // guarantee at this point initialisation won't default to another creation method later in the
    // initalisation process.
    auto creationMethod = CollectionTruncateMarkers::computeInitialCreationMethod(
        numRecords, dataSize, minBytesPerMarker);
    LOGV2_INFO(7658604,
               "Decided on initial creation method for pre-images truncate markers initialization",
               "creationMethod"_attr = CollectionTruncateMarkers::toString(creationMethod),
               "dataSize"_attr = dataSize,
               "numRecords"_attr = numRecords,
               "ns"_attr = preImageCollection.nss());

    switch (creationMethod) {
        case CollectionTruncateMarkers::MarkersCreationMethod::EmptyCollection:
            // Default to scanning since 'dataSize' and 'numRecords' could be incorrect.
        case CollectionTruncateMarkers::MarkersCreationMethod::Scanning:
            return generateTruncateMarkersForTenantScanning(opCtx, tenantId, preImageCollection);
        case CollectionTruncateMarkers::MarkersCreationMethod::Sampling: {
            // Use the collection's average record size to estimate the number of records in
            // each marker, and thus estimate the combined size of the records.
            double avgRecordSize = double(dataSize) / double(numRecords);
            double estimatedRecordsPerMarker = std::ceil(minBytesPerMarker / avgRecordSize);
            double estimatedBytesPerMarker = estimatedRecordsPerMarker * avgRecordSize;

            return generateTruncateMarkersForTenantSampling(
                opCtx,
                tenantId,
                preImageCollection,
                InitialSamplingEstimates{numRecords,
                                         dataSize,
                                         (int64_t)estimatedRecordsPerMarker,
                                         (int64_t)estimatedBytesPerMarker,
                                         minBytesPerMarker});
        }
        default:
            MONGO_UNREACHABLE;
    }
}

// Performs a ranged truncate over each expired marker in 'truncateMarkersForNss'. Updates the
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

// Dumps the contents of 'installedTruncateMarkersSnapshot' and 'highestRecordIdAndWallTimeSamples'.
// If there is an 'nsUUID' reported in the 'installedTruncateMarkers' with no corresponding entry in
// 'highestSampledRecords', something went wrong during initialization.
BSONObj dumpInstalledMarkersAndHighestRecordSamples(
    const NsUUIDToSamplesMap& highestRecordIdAndWallTimeSamples,
    const PreImagesTruncateManager::TenantTruncateMarkers* installedTruncateMarkersSnapshot) {
    BSONObjBuilder b;
    {
        BSONArrayBuilder samplesArrayBuilder;
        for (const auto& [nsUUID, nsUUIDHighestRidAndWallTime] :
             highestRecordIdAndWallTimeSamples) {
            const auto& [highestRid, highestWallTime] = nsUUIDHighestRidAndWallTime[0];
            BSONObjBuilder sampleBuilder;
            sampleBuilder.append("nsUUID", nsUUID.toString());
            sampleBuilder.append("ts",
                                 change_stream_pre_image_util::getPreImageTimestamp(highestRid));
            sampleBuilder.append("wallTime", highestWallTime);
            samplesArrayBuilder.append(sampleBuilder.obj());
        }
        b.appendArray("highestSampledRecords", samplesArrayBuilder.obj());
    }
    {
        BSONArrayBuilder truncateMarkerNsUUIDsArrayBuilder;
        for (const auto& [nsUUID, _] : *installedTruncateMarkersSnapshot) {
            BSONObjBuilder truncateMarkerDebugBuilder;
            truncateMarkerDebugBuilder.append("nsUUID", nsUUID.toString());
            truncateMarkerNsUUIDsArrayBuilder.append(truncateMarkerDebugBuilder.obj());
        }
        b.appendArray("installedTruncateMarkers", truncateMarkerNsUUIDsArrayBuilder.obj());
    }
    return b.obj();
}
}  // namespace

void PreImagesTruncateManager::ensureMarkersInitialized(
    OperationContext* opCtx,
    boost::optional<TenantId> tenantId,
    const CollectionAcquisition& preImagesColl) {

    auto tenantTruncateMarkers = _tenantMap.find(tenantId);
    if (!tenantTruncateMarkers) {
        _registerAndInitialiseMarkersForTenant(opCtx, tenantId, preImagesColl);
    }
}

PreImagesTruncateManager::TruncateStats PreImagesTruncateManager::truncateExpiredPreImages(
    OperationContext* opCtx,
    boost::optional<TenantId> tenantId,
    const CollectionPtr& preImagesColl) {
    TruncateStats stats;
    auto tenantTruncateMarkers = _tenantMap.find(tenantId);
    if (!tenantTruncateMarkers) {
        return stats;
    }

    auto snapShottedTruncateMarkers = tenantTruncateMarkers->getUnderlyingSnapshot();

    // All pre-images with 'ts' <= 'maxTSEligibleForTruncate' are candidates for truncation.
    // However, pre-images with 'ts' > 'maxTSEligibleForTruncate' are unsafe to truncate, as there
    // may be oplog holes or inconsistent data prior to it.
    // Compute the value once, as it requires making an additional call into the storage engine.
    Timestamp maxTSEligibleForTruncate = getMaxTSEligibleForTruncate(opCtx);
    for (auto& [nsUUID, truncateMarkersForNsUUID] : *snapShottedTruncateMarkers) {
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
        // marker which can be made into an expired marker, try to remove the new marker as well.
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

        // If the source collection doesn't exist and there's no more data to erase we can safely
        // remove the markers. Perform a final truncate to remove all elements just in case.
        if (CollectionCatalog::get(opCtx)->lookupCollectionByUUID(opCtx, nsUUID) == nullptr &&
            truncateMarkersForNsUUID->isEmpty()) {

            RecordId maxRecordId =
                change_stream_pre_image_util::getAbsoluteMaxPreImageRecordIdBoundForNs(nsUUID)
                    .recordId();

            writeConflictRetry(opCtx, "final truncate", preImagesColl->ns(), [&] {
                change_stream_pre_image_util::truncateRange(
                    opCtx, preImagesColl, minRecordId, maxRecordId, 0, 0);
            });

            tenantTruncateMarkers->erase(nsUUID);
        }
    }

    return stats;
}

void PreImagesTruncateManager::dropAllMarkersForTenant(boost::optional<TenantId> tenantId) {
    _tenantMap.erase(tenantId);
}

void PreImagesTruncateManager::updateMarkersOnInsert(OperationContext* opCtx,
                                                     boost::optional<TenantId> tenantId,
                                                     const ChangeStreamPreImage& preImage,
                                                     int64_t bytesInserted) {
    dassert(bytesInserted != 0);
    auto nsUuid = preImage.getId().getNsUUID();
    auto wallTime = preImage.getOperationTime();
    auto recordId = change_stream_pre_image_util::toRecordId(preImage.getId());

    shard_role_details::getRecoveryUnit(opCtx)->onCommit(
        [this,
         tenantId = std::move(tenantId),
         nsUuid = std::move(nsUuid),
         recordId = std::move(recordId),
         bytesInserted,
         wallTime](OperationContext* opCtx, boost::optional<Timestamp>) {
            auto tenantTruncateMarkers = _tenantMap.find(tenantId);
            if (!tenantTruncateMarkers) {
                return;
            }

            auto truncateMarkersForNsUUID = tenantTruncateMarkers->find(nsUuid);

            if (!truncateMarkersForNsUUID) {
                // There are 2 possible scenarios here:
                //  (1) The 'tenantTruncateMarkers' was created, but isn't done with
                //  initialisation. In this case, the truncate markers created for 'nsUUID' may or
                //  may not be overwritten in the initialisation process. This is okay, lazy
                //  initialisation is performed by the remover thread to avoid blocking writes on
                //  startup and is strictly best effort.
                //
                //  (2) Pre-images were either recently enabled on 'nsUUID' or 'nsUUID' was just
                //  created. Either way, the first pre-images enabled insert to call
                //  'getOrEmplace()' creates the truncate markers for the 'nsUUID'. Any following
                //  calls to 'getOrEmplace()' return a pointer to the existing truncate markers.
                truncateMarkersForNsUUID = tenantTruncateMarkers->getOrEmplace(
                    nsUuid,
                    tenantId,
                    std::deque<CollectionTruncateMarkers::Marker>{},
                    0,
                    0,
                    gPreImagesCollectionTruncateMarkersMinBytes,
                    CollectionTruncateMarkers::MarkersCreationMethod::EmptyCollection);
            }

            truncateMarkersForNsUUID->updateMarkers(bytesInserted, recordId, wallTime, 1);
        });
}

void PreImagesTruncateManager::_registerAndInitialiseMarkersForTenant(
    OperationContext* opCtx,
    boost::optional<TenantId> tenantId,
    const CollectionAcquisition& preImagesCollection) {
    // (A) Register the 'tenantId' in the '_tenantMap' so inserts for namespaces created between (A)
    // and (C) are tracked in 'tenantMapEntry'.
    auto tenantMapEntry = _tenantMap.getOrEmplace(tenantId);

    // (B) Generate the initial set of truncate markers for the tenant.
    auto generatedTruncateMarkers =
        generateTruncateMarkersForTenant(opCtx, tenantId, preImagesCollection);

    // (C) Install the generated truncate markers into the 'tenantMapEntry'.
    tenantMapEntry->updateWith(
        [&](const PreImagesTruncateManager::TenantTruncateMarkers& tenantMapEntryPlaceHolder) {
            // Critical section where no other threads can modify the 'tenantMapEntry'.

            for (const auto& [nsUUID, nsTruncateMarkers] : tenantMapEntryPlaceHolder) {
                if (generatedTruncateMarkers.find(nsUUID) == generatedTruncateMarkers.end()) {
                    // Add this 'nsUUID' which was not present in (B)'s snapshot and was intercepted
                    // between (A) and (C).
                    LOGV2_DEBUG(8204001,
                                0,
                                "Appending pre-image truncate markers created for a namespace not "
                                "captured during truncate marker generation",
                                "nsUUID"_attr = nsUUID,
                                "tenantId"_attr = tenantId);

                    generatedTruncateMarkers.emplace(nsUUID, nsTruncateMarkers);
                }
            }

            // Overwrite truncate markers created in 'tenantMapEntryPlaceHolder' whose
            // nsUUIDs are already accounted for in 'generatedTruncateMarkers' - as
            // 'generatedTruncateMarkers' account for all the pre-images pre-dating the short
            // generation period.
            //
            // Merging two sets of truncate markers would create unnecessary complexity in the best
            // effort process.
            return generatedTruncateMarkers;
        });

    // (D) Finalize the truncate markers by ensuring they have up-to-date highest RecordId and wall
    // times.
    auto snapShottedTruncateMarkers = tenantMapEntry->getUnderlyingSnapshot();

    // We must refresh the snapshot and update the highest seen RecordId and wall time for each
    // nsUUID to ensure all inserts concurrent with marker generation are eventually truncated.
    // Example:
    //      (i) SnapshotA is used to create 'generatedTruncateMarkers'.
    //      (ii) PreImage100 is inserted into  NsUUID1 - the insert isn't visible
    //      in SnapshotA.
    //      (iii) There aren't any other inserts into NsUUID1. The highest wall time and RecordId
    //      for NsUUID1 MUST be updated so the markers track PreImage100, and know to eventually
    //      truncate it.
    shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();

    auto rs = preImagesCollection.getCollectionPtr()->getRecordStore();
    NsUUIDToSamplesMap highestRecordIdAndWallTimeSamples;
    sampleLastRecordPerNsUUID(opCtx, rs, highestRecordIdAndWallTimeSamples);

    for (auto& [nsUUID, truncateMarkersForNsUUID] : *snapShottedTruncateMarkers) {
        // At this point, truncation could not possible occur yet, so the lastRecordIdAndWallTimes
        // is expected to always contain an entry for the 'nsUUID'.
        auto nsUUIDHighestRidAndWallTime = highestRecordIdAndWallTimeSamples.find(nsUUID);
        if (nsUUIDHighestRidAndWallTime == highestRecordIdAndWallTimeSamples.end()) {
            // Pre-images inserted on the nsUUID won't be removed until either a new pre-image for
            // the nsUUID is inserted or the server is restarted. This should only be possible if
            // there were no pre-images for the nsUUID before the snapshot used to sample the
            // highest RecordId and wall time.
            const auto stateDump = kDebugBuild
                ? dumpInstalledMarkersAndHighestRecordSamples(highestRecordIdAndWallTimeSamples,
                                                              snapShottedTruncateMarkers.get())
                : BSONObj();
            LOGV2_WARNING(8204000,
                          "Unable to update the highest seen RecordId and wall time for truncate "
                          "markers on nsUUID during initialization",
                          "nsUUID"_attr = nsUUID.toString(),
                          "tenantId"_attr = tenantId,
                          "details"_attr = stateDump);

            dassert(nsUUIDHighestRidAndWallTime != highestRecordIdAndWallTimeSamples.end());
        }
        auto [highestRid, highestWallTime] = nsUUIDHighestRidAndWallTime->second[0];
        truncateMarkersForNsUUID->updateMarkers(0, highestRid, highestWallTime, 0);
    }
}

}  // namespace mongo
