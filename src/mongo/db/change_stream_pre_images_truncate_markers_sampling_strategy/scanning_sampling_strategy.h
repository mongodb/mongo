// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/change_stream_pre_images_truncate_markers_sampling_strategy/sampling_strategy.h"
#include "mongo/util/modules.h"
namespace mongo::pre_image_marker_initialization_internal {
/**
 * SamplingStrategy that initializes truncate markers by performing a full scan
 * over the pre-images collection instead of using sampling.
 */
class ScanningSamplingStrategy : public SamplingStrategy {
public:
    explicit ScanningSamplingStrategy(int32_t minBytesPerMarker)
        : _minBytesPerMarker(minBytesPerMarker) {}

    /**
     * Populates the 'markersMap' by scanning the pre-images collection.
     */
    [[nodiscard]]
    bool performSampling(OperationContext* opCtx,
                         const CollectionAcquisition& preImagesCollection,
                         MarkersMap& markersMap) override;

private:
    int32_t _minBytesPerMarker;
};

}  // namespace mongo::pre_image_marker_initialization_internal
