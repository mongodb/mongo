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

#include "mongo/db/change_stream_pre_images_truncate_markers_sampling_strategy/sampling_strategy.h"

#include "mongo/db/change_stream_pre_image_id_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::pre_image_marker_initialization_internal {
namespace {
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
}  // namespace

int64_t countTotalSamples(const SamplesMap& samplesMap) {
    int64_t totalSamples{0};
    for (const auto& [_, nsUUIDSamples] : samplesMap) {
        totalSamples = totalSamples + nsUUIDSamples.size();
    }
    return totalSamples;
}

stdx::unordered_map<UUID, RecordIdAndWallTime, UUID::Hash> sampleLastRecordPerNsUUID(
    OperationContext* opCtx, const CollectionAcquisition& preImagesCollection) {
    stdx::unordered_map<UUID, RecordIdAndWallTime, UUID::Hash> lastRecords;

    const auto rs = preImagesCollection.getCollectionPtr()->getRecordStore();
    auto cursor =
        rs->getCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx), false /*forward*/);

    boost::optional<Record> record = cursor->next();

    while (record) {
        // As a reverse cursor, the first record we see for a namespace is the highest.
        UUID currentNsUUID =
            change_stream_pre_image_id_util::getPreImageNsUUID(record->data.toBson());
        auto sample = PreImagesTruncateMarkersPerNsUUID::getRecordIdAndWallTime(*record);
        lastRecords.insert({currentNsUUID, std::move(sample)});

        RecordId minRecordIdForCurrentNsUUID =
            change_stream_pre_image_id_util::getAbsoluteMinPreImageRecordIdBoundForNs(currentNsUUID)
                .recordId();

        // A reverse exclusive 'seek' will return the previous entry in the collection. This should
        // ensure that the record's id is less than the 'minRecordIdForCurrentNsUUID', which
        // positions it exactly at the highest record of the previous collection UUID.
        record = cursor->seek(minRecordIdForCurrentNsUUID,
                              SeekableRecordCursor::BoundInclusion::kExclude);
        invariant(!record ||
                  currentNsUUID !=
                      change_stream_pre_image_id_util::getPreImageNsUUID(record->data.toBson()));
    }
    return lastRecords;
}

void SamplingStrategy::installMarkersInMapFromSamples(
    OperationContext* opCtx,
    const CollectionAcquisition& preImagesCollection,
    const SamplesMap& samples,
    int64_t numRecords,
    int64_t dataSize,
    int32_t minBytesPerMarker,
    uint64_t randomSamplesPerMarker,
    double estimatedRecordsPerMarker,
    double estimatedBytesPerMarker,
    MarkersMap& markersMap) {
    // Use the samples to generate and install in 'markersMap' the initial sets of whole markers for
    // each distinct 'nsUUID' value. The aggregate number of records and bytes across the map aren't
    // expected to match the reported 'dataSize' and 'numRecords' yet.
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

        // 'ConcurrentSharedValuesMap<K, V>' does not provide 'insert()' or 'emplace()' methods.
        // Here it's guaranteed that no entry for 'nsUUID' exists in the map when calling
        // 'getOrEmplace()'.
        invariant(markersMap.find(nsUUID) == nullptr);
        markersMap.getOrEmplace(nsUUID, nsUUID, std::move(initialSetOfMarkers), minBytesPerMarker);
    }

    // Fix the entries in 'markersMap' so the aggregate number of records and bytes across the map
    // make up the expected number of records and bytes in the pre-images collection.
    distributeUnaccountedRecordsAndBytes(nsUUIDs,
                                         preImagesCollection.uuid(),
                                         numRecords,
                                         dataSize,
                                         recordsInMarkersMap,
                                         bytesInMarkersMap,
                                         markersMap);
}

void SamplingStrategy::distributeUnaccountedRecordsAndBytes(
    const stdx::unordered_set<UUID, UUID::Hash>& nsUUIDs,
    const UUID& preImagesCollectionUUID,
    int64_t numRecords,
    int64_t dataSize,
    int64_t recordsInMarkersMap,
    int64_t bytesInMarkersMap,
    MarkersMap& markersMap) {
    const auto unaccountedRecords = numRecords - recordsInMarkersMap;
    const auto unaccountedBytes = dataSize - bytesInMarkersMap;

    if (unaccountedRecords < 0 || unaccountedBytes < 0) {
        // The markers account for more records / bytes than the estimated pre-images collection.
        // Something was off about the sampling logic and this scenario should be investigated.
        LOGV2_WARNING(
            7658603,
            "Pre-images initial truncate markers account for more bytes and/or records than "
            "expected",
            "unaccountedRecords"_attr = unaccountedRecords,
            "unaccountedBytes"_attr = unaccountedBytes,
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

bool PrimaryWithFallbackSamplingStrategy::performSampling(
    OperationContext* opCtx,
    const CollectionAcquisition& preImagesCollection,
    MarkersMap& markersMap) {
    if (_primaryStrategy->performSampling(opCtx, preImagesCollection, markersMap)) {
        return true;
    }

    return _fallbackStrategy->performSampling(opCtx, preImagesCollection, markersMap);
}

}  // namespace mongo::pre_image_marker_initialization_internal
