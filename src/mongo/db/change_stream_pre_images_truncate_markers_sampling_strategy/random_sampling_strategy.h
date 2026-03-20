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
