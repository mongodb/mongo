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

private:
    uint64_t _numSamplesPerMarker;
    int32_t _minBytesPerMarker;
};
}  // namespace mongo::pre_image_marker_initialization_internal
