// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/change_stream_pre_images_truncate_markers_sampling_strategy/scanning_sampling_strategy.h"

#include "mongo/db/change_stream_pre_image_util.h"

namespace mongo::pre_image_marker_initialization_internal {
bool ScanningSamplingStrategy::performSampling(OperationContext* opCtx,
                                               const CollectionAcquisition& preImagesCollection,
                                               MarkersMap& markersMap) {
    const auto nsUUIDs = change_stream_pre_image_util::getNsUUIDs(opCtx, preImagesCollection);
    for (const auto& nsUUID : nsUUIDs) {
        auto initialSetOfMarkers = PreImagesTruncateMarkersPerNsUUID::createInitialMarkersScanning(
            opCtx, preImagesCollection, nsUUID, _minBytesPerMarker);

        markersMap.getOrEmplace(nsUUID, nsUUID, std::move(initialSetOfMarkers), _minBytesPerMarker);
    }

    return true;
}
};  // namespace mongo::pre_image_marker_initialization_internal
// namespace mongo
