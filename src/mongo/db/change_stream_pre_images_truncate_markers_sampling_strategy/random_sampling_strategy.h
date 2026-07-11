// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/change_stream_pre_images_truncate_markers_sampling_strategy/sampling_strategy.h"
#include "mongo/util/modules.h"

namespace mongo::pre_image_marker_initialization_internal {

/**
 * SamplingStrategy that attempts to initialize truncate markers by taking random
 * samples from the pre-images collection.
 */
class RandomSamplingStrategy : public SamplingStrategy {
public:
    explicit RandomSamplingStrategy(uint64_t numSamplesPerMarker, int32_t minBytesPerMarker)
        : _numSamplesPerMarker(numSamplesPerMarker), _minBytesPerMarker(minBytesPerMarker) {}

    /**
     * Given:
     * - 'numRecords' and 'dataSize' - The expected size of the 'preImagesCollection'. These
     *   metrics are not guaranteed to be correct after an unclean shutdown.
     * - 'minBytesPerMarker' - The minimum number of bytes needed to compose a full truncate marker.
     * - 'randomSamplesPerMarker' - The number of samples necessary to estimate a full truncate
     * marker.
     *
     * Attempts to populate 'markersMap' by random sampling over the pre-images collection.
     *
     * Sampling Guarantee: Individual truncate markers and metrics for each 'nsUUID' may not be
     * accurate; but, cumulatively, the total number of records and bytes captured by the
     * 'markersMap' should reflect the 'numRecords' and 'dataSize'.
     *
     * On success, updates 'markersMap' and returns true. If random sampling cannot be applied
     * reliably, leaves 'markersMap' unchanged and returns false.
     */
    [[nodiscard]]
    bool performSampling(OperationContext* opCtx,
                         const CollectionAcquisition& preImagesCollection,
                         MarkersMap& markersMap) override;

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

private:
    uint64_t _numSamplesPerMarker;
    int32_t _minBytesPerMarker;
};
}  // namespace mongo::pre_image_marker_initialization_internal
