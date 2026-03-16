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
