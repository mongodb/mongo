// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/change_stream_pre_images_truncate_markers_sampling_strategy/sampling_strategy.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

namespace mongo::pre_image_marker_initialization_internal {
/**
 * SamplingStrategy that initializes truncate markers by sampling pre-images for each UUID in
 * approximately equal timestamp steps between the earliest and latest pre-image for that UUID.
 */
class EqualStepSamplingStrategy : public SamplingStrategy {
public:
    explicit EqualStepSamplingStrategy(uint64_t numSamplesPerMarker, int32_t minBytesPerMarker)
        : _numSamplesPerMarker(numSamplesPerMarker), _minBytesPerMarker(minBytesPerMarker) {}

    /**
     * Populates the initial truncate markers in 'markersMap' via sampling the documents in the
     * preimages collection in approximately equally-sized steps for each distinct 'nsUUID'. First
     * performs a loose backward scan to enumerate all distinct 'nsUUID' values in the preimage
     * collection, together with their maximum timestamp values. Afterwards, for each distinct
     * 'nsUUID' value, a sample of up to 'numSamplesPerMarker' records is collected, starting at the
     * lowest found timestamp and ending with the highest timestamp. The records in-between the
     * lowest and highest timestamps are accessed in approximately equally-sized steps.
     */
    [[nodiscard]]
    bool performSampling(OperationContext* opCtx,
                         const CollectionAcquisition& preImagesCollection,
                         MarkersMap& markersMap) override;

    /**
     * Sample the values in the preimages collection for the given 'nsUUID' value in approximately
     * equal Timestamp distance steps. Sampling will start by seeking to the minimum possible
     * Timestamp value for the 'nsUUID', which is for timestamp 0 and applyOpsIndex 0. If a record
     * is found that still belongs to the target 'nsUUID' value, it will be added to the sample. The
     * Timestamp value will then be increased in roughly equally-sized steps until we exceed the
     * timestamp in 'lastRidAndWall', or no further records for the 'nsUUID' value are found. All
     * samples are returned in a vector, which is sorted primarily by Timestamp value, then
     * 'applyOpsIndex' values of the samples. There will be at most 'numSamples' sample records
     * returned, and at least one.
     *
     * The sampling currently does not take into account the 'applyOpsIndex' values. It will only
     * seek to different Timestamp values, so the outcome will be suboptimal if there are large
     * ranges with the same Timestamp but different applyOpsIndex values. It will work though if the
     * Timestamp values in the collection have the same 't' (seconds) value, but differ only in
     * their 'i' (increment) part.
     */
    std::vector<RecordIdAndWallTime> sampleNSUUIDRangeEqually(
        OperationContext* opCtx,
        const CollectionAcquisition& preImagesCollection,
        UUID nsUUID,
        const RecordIdAndWallTime& lastRidAndWall,
        uint64_t numSamples);

    /**
     * Estimated/assumed size for every pre-image sampled.
     */
    static constexpr int kRecordSizeEstimate = 1024;

private:
    uint64_t _numSamplesPerMarker;
    int32_t _minBytesPerMarker;
};
}  // namespace mongo::pre_image_marker_initialization_internal
