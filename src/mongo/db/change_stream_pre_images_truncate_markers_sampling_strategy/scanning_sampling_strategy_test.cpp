// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/change_stream_pre_images_truncate_markers_sampling_strategy/scanning_sampling_strategy.h"

#include "mongo/db/change_stream_pre_image_test_helpers.h"
#include "mongo/db/change_stream_pre_images_truncate_markers.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {
using namespace change_stream_pre_image_test_helper;
using namespace pre_image_marker_initialization_internal;

/**
 * Tests components of pre-image truncate marker initialization across a pre-image collection.
 */
class ScanningSamplingStrategyTest : public CatalogTestFixture,
                                     public ChangeStreamPreImageTestConstants {};

/**
 * Tests for scanning / sampling.
 */
TEST_F(ScanningSamplingStrategyTest, PopulateMapWithEmptyCollection) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);

    // Populate by scanning an empty collection.
    MarkersMap markersMap;
    auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);
    ScanningSamplingStrategy samplingStrategy(1);
    ASSERT_TRUE(samplingStrategy.performSampling(opCtx, preImagesCollection, markersMap));
    const auto mapSnapshot = markersMap.getUnderlyingSnapshot();
    ASSERT_EQ(0, mapSnapshot->size());
}

TEST_F(ScanningSamplingStrategyTest, PopulateMapWithSinglePreImage) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);
    insertDirectlyToPreImagesCollection(opCtx, kPreImage1);

    auto assertPreImageIsTracked = [](const UUID& expectedUUID,
                                      const MarkersMap& markersMap,
                                      const ChangeStreamPreImage& expectedPreImage) {
        const auto mapSnapshot = markersMap.getUnderlyingSnapshot();
        ASSERT_GTE(1, mapSnapshot->size());
        auto it = mapSnapshot->find(expectedUUID);
        ASSERT(it != mapSnapshot->end());
        auto perNsUUIDMarkers = it->second;
        ASSERT_FALSE(perNsUUIDMarkers->isEmpty());

        bool trackingPreImage = activelyTrackingPreImage(*perNsUUIDMarkers, expectedPreImage);
        ASSERT_TRUE(trackingPreImage) << fmt::format(
            "Expected pre-image to be actively tracked in truncate markers. Pre-image: {}, "
            "truncateMarkers: {}",
            expectedPreImage.toBSON().toString(),
            toBSON(*perNsUUIDMarkers).toString());
    };

    {
        // Populate by scanning, pre-image covered by full marker.
        MarkersMap markersMap;
        auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);
        ScanningSamplingStrategy samplingStrategy(1);
        ASSERT_TRUE(samplingStrategy.performSampling(opCtx, preImagesCollection, markersMap));
        assertPreImageIsTracked(kNsUUID, markersMap, kPreImage1);
    }

    {
        // Populate by scanning, pre-image not covered in a whole marker because
        // minBytesPerMarker is larger than the pre-image size.
        MarkersMap markersMap;
        auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);
        ScanningSamplingStrategy samplingStrategy(bytes(kPreImage1) + 100);
        ASSERT_TRUE(samplingStrategy.performSampling(opCtx, preImagesCollection, markersMap));
        assertPreImageIsTracked(kNsUUID, markersMap, kPreImage1);
    }
}

}  // namespace
}  // namespace mongo
