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

#pragma once

#include "mongo/db/change_stream_pre_images_truncate_markers_per_nsUUID.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/collection_truncate_markers.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/concurrent_shared_values_map.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace pre_image_marker_initialization_internal {
/**
 * The RecordId and wall time extracted from a pre-image. Comprises a pre-image 'sample'.
 */
using RecordIdAndWallTime = CollectionTruncateMarkers::RecordIdAndWallTime;

/**
 * Pre-images samples for multiple collections, keyed by collection UUID.
 */
using SamplesMap = stdx::unordered_map<UUID, std::vector<RecordIdAndWallTime>, UUID::Hash>;

/**
 * Pre-images truncate markers, keyed by collection UUID.
 */
using MarkersMap = ConcurrentSharedValuesMap<UUID, PreImagesTruncateMarkersPerNsUUID, UUID::Hash>;

int64_t countTotalSamples(const SamplesMap& samplesMap);

/**
 * Sample the values in the preimages collection for the given 'nsUUID' value in approximately equal
 * Timestamp distance steps. Sampling will start by seeking to the minimum possible Timestamp value
 * for the 'nsUUID', which is for timestamp 0 and applyOpsIndex 0. If a record is found that still
 * belongs to the target 'nsUUID' value, it will be added to the sample. The Timestamp value will
 * then be increased in roughly equally-sized steps until we exceed the timestamp in
 * 'lastRidAndWall', or no further records for the 'nsUUID' value are found.
 * All samples are returned in a vector, which is sorted primarily by Timestamp value, then
 * 'applyOpsIndex' values of the samples. There will be at most 'numSamples' sample records
 * returned, and at least one.
 *
 * The sampling currently does not take into account the 'applyOpsIndex' values. It will only seek
 * to different Timestamp values, so the outcome will be suboptimal if there are large ranges with
 * the same Timestamp but different applyOpsIndex values.
 * It will work though if the Timestamp values in the collection have the same 't' (seconds) value,
 * but differ only in their 'i' (increment) part.
 */
std::vector<RecordIdAndWallTime> sampleNSUUIDRangeEqually(
    OperationContext* opCtx,
    const CollectionAcquisition& preImagesCollection,
    UUID nsUUID,
    const RecordIdAndWallTime& lastRidAndWall,
    uint64_t numSamples);

/**
 * Performs a loose reverse scan over the preimages collection and tracks the highest
 * timestamp/applyOpsIndex combination for every distinct 'nsUUID' value. Returns the
 * 'CollectionTruncateMarkers::RecordIdAndWallTime' for the most recent pre-image per each 'nsUUID'
 * in the pre-images collection.
 */
stdx::unordered_map<UUID, RecordIdAndWallTime, UUID::Hash> sampleLastRecordPerNsUUID(
    OperationContext* opCtx, const CollectionAcquisition& preImagesCollection);

/**
 * Attempts to gather 'targetNumSamples' across the pre-images collection.
 *
 * The samples are guaranteed to include the most recent (visible) pre-image per 'nsUUID'
 * within the pre-images collection - all additional samples are retrieved at random.
 *
 * Returns 'targetNumSamples', sorted by RecordId and mapped by 'nsUUID', with 2 exceptions
 * - (a) The collection is empty: No samples are returned.
 * - (b) There are more 'nsUUID's than 'targetNumSamples': 1 sample per nsUUID is returned.
 */
SamplesMap collectPreImageSamples(OperationContext* opCtx,
                                  const CollectionAcquisition& preImagesCollection,
                                  int64_t targetNumSamples);

/**
 * Populates the 'markersMap' by scanning the pre-images collection.
 */
void populateByScanning(OperationContext* opCtx,
                        const CollectionAcquisition& preImagesCollection,
                        int32_t minBytesPerMarker,
                        MarkersMap& markersMap);

/**
 * Given:
 *  . 'numRecords' and 'dataSize' - The expected size of the 'preImagesCollection'. These
 *      metrics are not guaranteed to be correct after an unclean shutdown.
 * . 'minBytesPerMarker' - The minimum number of bytes needed to compose a full truncate marker.
 * . 'randomSamplesPerMarker' - The number of samples necessary to estimate a full truncate marker.
 *
 * Populates the 'markersMap' by sampling the pre-images collection. If sampling cannot complete,
 * falls back to scanning the collection.
 *
 * Sampling Guarantee: Individual truncate markers and metrics for each 'nsUUID' may not be
 * accurate; but, cumulatively, the total number of records and bytes captured by the 'markersMap'
 * should reflect the 'numRecords' and 'dataSize'.
 */
void populateByRandomSampling(OperationContext* opCtx,
                              const CollectionAcquisition& preImagesCollection,
                              int64_t numRecords,
                              int64_t dataSize,
                              int32_t minBytesPerMarker,
                              uint64_t randomSamplesPerMarker,
                              MarkersMap& markersMap);

/**
 * Populates the initial truncate markers in 'markersMap' via sampling the documents in the
 * preimages collection in approximately equally-sized steps for each distinct 'nsUUID'. First
 * performs a loose backward scan to enumerate all distinct 'nsUUID' values in the preimage
 * collection, together with their maximum timestamp values. Afterwards, for each distinct 'nsUUID'
 * value, a sample of up to 'numSamplesPerMarker' records is collected, starting at the lowest found
 * timestamp and ending with the highest timestamp. The records in-between the lowest and highest
 * timestamps are accessed in approximately equally-sized steps.
 */
void populateByEqualStepSampling(OperationContext* opCtx,
                                 const CollectionAcquisition& preImagesCollection,
                                 uint64_t numSamplesPerMarker,
                                 MarkersMap& markersMap);
}  // namespace pre_image_marker_initialization_internal

/**
 * Statistics for a truncate pass over the system's pre-images collection.
 */
struct PreImagesTruncateStats {
    // The estimated number of bytes deleted in the truncate pass.
    // This number is an estimate based on the collection size estimates present in the collection
    // truncate markers, and it is only as accurate as the size/count information in the
    // 'CollectionTruncateMarker's is.
    int64_t bytesDeleted{0};

    // The estimated number of documents deleted in the truncate pass.
    // This number is an estimate based on the collection count estimates present in the collection
    // truncate markers, and it is only as accurate as the size/count information in the
    // 'CollectionTruncateMarker's is.
    int64_t docsDeleted{0};

    // The number of 'nsUUID's scanned in the truncate pass.
    int64_t scannedInternalCollections{0};

    // Instantaneous maximum timestamp eligible for truncation. Expired documents will only be
    // truncated when their timestamp is less than or equal to it.
    Timestamp maxTimestampEligibleForTruncate;

    // The maximum wall time from the pre-images truncated across the collection.
    Date_t maxStartWallTime{};
};

/**
 * Manages truncate markers specific to the system's pre-images collection.
 */
class PreImagesTruncateMarkers {
    PreImagesTruncateMarkers(const PreImagesTruncateMarkers&) = delete;
    PreImagesTruncateMarkers& operator=(const PreImagesTruncateMarkers&) = delete;

public:
    /**
     * Creates a 'PreImagesTruncateMarkers' instance populated with truncate markers that span the
     * system's pre-images collection. However, these markers are not safe to use until
     * 'refreshMarkers' is called to update the highest seen recordId and wall time for each
     * nsUUID. Otherwise, pre-images would not be truncated until new inserts come in for each
     * nsUUID.
     *
     * Note: Pre-images inserted concurrently with creation might not be covered by the resulting
     * truncate markers.
     *
     * The 'preImagesCollection' is required to exist. Otherwise the constructor will trigger an
     * invariant failure. The 'OperationContext' parameter is only used for the initial population
     * of the truncate markers, but is not used afterwards.
     */
    PreImagesTruncateMarkers(OperationContext* opCtx,
                             const CollectionAcquisition& preImagesCollection);

    /**
     * Opens a fresh snapshot and ensures the all pre-images visible in the snapshot are
     * covered by truncate markers.
     *
     * Required to ensure truncate markers are viable for truncation.
     */
    void refreshMarkers(OperationContext* opCtx);

    PreImagesTruncateStats truncateExpiredPreImages(OperationContext* opCtx);

    /**
     * Updates or creates the 'PreImagesTruncateMarkersPerNsUUID' to account for a
     * newly inserted pre-image generated from the user's collection with UUID 'nsUUID'.
     *
     * 'numRecords' should always be 1 except for during initialization.
     *
     * Callers are responsible for calling this only once the data inserted is committed.
     */
    void updateOnInsert(const RecordId& recordId,
                        const UUID& nsUUID,
                        Date_t wallTime,
                        int64_t bytesInserted,
                        int64_t numRecords = 1);

    UUID getPreImagesCollectionUUID() const {
        return _preImagesCollectionUUID;
    }

    const pre_image_marker_initialization_internal::MarkersMap& getMarkersMap_forTest() const {
        return _markersMap;
    }

private:
    const UUID _preImagesCollectionUUID;

    /**
     * The pre-images collection spans pre-images generated across all pre-image enabled
     * collections. The pre-images collection is sorted so that all pre-images from the same
     * 'nsUUID' are stored consecutively. There is a separate set of truncate markers for each
     * 'nsUUID'.
     *
     * Maps pre-images of a given 'nsUUID' to their truncate markers.
     */
    pre_image_marker_initialization_internal::MarkersMap _markersMap;
};

}  // namespace mongo
