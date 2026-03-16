/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/change_stream_pre_images_truncate_markers_sampling_strategy/random_sampling_strategy.h"

#include "mongo/db/change_stream_pre_image_id_util.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/storage/storage_parameters_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::pre_image_marker_initialization_internal {
namespace {
void appendSample(const BSONObj& preImageObj, const RecordId& rId, SamplesMap& samplesMap) {
    const auto uuid = change_stream_pre_image_id_util::getPreImageNsUUID(preImageObj);
    const auto wallTime = PreImagesTruncateMarkersPerNsUUID::getWallTime(preImageObj);
    const auto ridAndWall = RecordIdAndWallTime{rId, wallTime};

    if (auto it = samplesMap.find(uuid); it != samplesMap.end()) {
        it->second.push_back(std::move(ridAndWall));
    } else {
        samplesMap.emplace(uuid, std::vector<RecordIdAndWallTime>{std::move(ridAndWall)});
    }
}
}  // namespace

SamplesMap RandomSamplingStrategy::collectPreImageSamples(
    OperationContext* opCtx,
    const CollectionAcquisition& preImagesCollection,
    int64_t targetNumSamples) {
    const auto nsUUIDLastRecords = sampleLastRecordPerNsUUID(opCtx, preImagesCollection);

    SamplesMap samples;
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

bool RandomSamplingStrategy::performSampling(OperationContext* opCtx,
                                             const CollectionAcquisition& preImagesCollection,
                                             MarkersMap& markersMap) {
    // Cached size of the pre-images collection. Not guaranteed to be accurate after server
    // restart.
    const auto numRecords = preImagesCollection.getCollectionPtr()->numRecords(opCtx);
    const auto dataSize = preImagesCollection.getCollectionPtr()->dataSize(opCtx);

    // The first method to try when populating the 'markersMap'. Note: sampling can fall back to
    // scanning if the cached collection sizes aren't accurate.
    const auto initialCreationMethod = CollectionTruncateMarkers::computeInitialCreationMethod(
        numRecords, dataSize, _minBytesPerMarker);
    LOGV2_INFO(7658604,
               "Decided on initial creation method for pre-images truncate markers initialization",
               "initialCreationMethod"_attr =
                   CollectionTruncateMarkers::toString(initialCreationMethod),
               "dataSize"_attr = dataSize,
               "numRecords"_attr = numRecords,
               "preImagesCollectionUUID"_attr = preImagesCollection.uuid());

    // If the initial creation method is not 'Sampling', early exit without performing sampling.
    if (initialCreationMethod != CollectionTruncateMarkers::MarkersCreationMethod::Sampling) {
        return false;
    }

    if (dataSize < 1 || numRecords < 1 || dataSize < numRecords) {
        // Safeguard against non-sensical and divide by 0 computations since 'numRecords' and
        // 'dataSize' aren't guaranteed to be accurate after unclean shutdown.
        LOGV2(9528900,
              "Reverting to scanning for initial pre-images truncate markers. The tracked number "
              "of records and bytes in the pre-images collection are not compatible with "
              "sampling",
              "preImagesCollectionUUID"_attr = preImagesCollection.uuid(),
              "numRecords"_attr = numRecords,
              "dataSize"_attr = dataSize);
        return false;
    }

    double avgRecordSize = double(dataSize) / double(numRecords);
    double estimatedRecordsPerMarker = std::ceil(_minBytesPerMarker / avgRecordSize);
    double estimatedBytesPerMarker = estimatedRecordsPerMarker * avgRecordSize;
    uint64_t numSamples = (_numSamplesPerMarker * numRecords) / estimatedRecordsPerMarker;

    if (numSamples == 0) {
        LOGV2(8198000,
              "Reverting to scanning for initial pre-images truncate markers. The number of "
              "samples needed is 0",
              "preImagesCollectionUUID"_attr = preImagesCollection.uuid());

        return false;
    }

    // Gather ordered sample points across the 'nsUUIDs' captured in the pre-images collection.
    const auto samples = collectPreImageSamples(opCtx, preImagesCollection, numSamples);
    const auto totalSamples = countTotalSamples(samples);
    if (totalSamples != (int64_t)numSamples) {
        // Given the distribution of pre-images to 'nsUUID', the number of samples collected
        // cannot effectively represent the pre-images collection. Default to scanning instead.
        LOGV2(7658601,
              "Reverting to scanning for initial pre-images truncate markers. The number of "
              "samples collected does not match the desired number of samples",
              "samplesTaken"_attr = totalSamples,
              "samplesDesired"_attr = numSamples,
              "preImagesCollectionUUID"_attr = preImagesCollection.uuid());
        return false;
    }

    // Use the samples to generate and install in 'markersMap' the initial sets of whole markers
    // for each distinct 'nsUUID' value. And then fix the install entries in 'markersMap' so the
    // aggregate number of records and bytes across the map make up the expected number of
    // records and bytes in the pre-images collection.
    installMarkersInMapFromSamples(opCtx,
                                   preImagesCollection,
                                   samples,
                                   numRecords,
                                   dataSize,
                                   _minBytesPerMarker,
                                   _numSamplesPerMarker,
                                   estimatedRecordsPerMarker,
                                   estimatedBytesPerMarker,
                                   markersMap);

    return true;
}
}  // namespace mongo::pre_image_marker_initialization_internal
