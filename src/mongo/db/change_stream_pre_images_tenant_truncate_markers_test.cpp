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
#include "mongo/db/change_stream_pre_images_tenant_truncate_markers.h"

#include "mongo/db/change_stream_pre_image_test_helpers.h"
#include "mongo/db/local_catalog/catalog_test_fixture.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {
using namespace change_stream_pre_image_test_helper;

/**
 * Tests components of pre-image truncate marker initialization across a pre-image collection.
 */
class PreImageMarkerInitializationTest : public CatalogTestFixture,
                                         public ChangeStreamPreImageTestConstants {};

TEST_F(PreImageMarkerInitializationTest, SampleEmptyCollection) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);

    const auto preImagesRAII = acquirePreImagesCollectionForRead(opCtx);
    const auto lastRecordsMap =
        pre_image_marker_initialization_internal::sampleLastRecordPerNsUUID(opCtx, preImagesRAII);
    ASSERT_EQ(0, lastRecordsMap.size());
}

TEST_F(PreImageMarkerInitializationTest, SampleLastRecordSingleNsUUID) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);

    // Pre-image inserts aren't guaranteed to be serialized. Simulate inserting a more recent
    // pre-image before a slightly older one.
    insertDirectlyToPreImagesCollection(opCtx, kPreImage1);
    insertDirectlyToPreImagesCollection(opCtx, kPreImage3);
    insertDirectlyToPreImagesCollection(opCtx, kPreImage2);

    const auto preImagesRAII = acquirePreImagesCollectionForRead(opCtx);
    const auto lastRecordsMap =
        pre_image_marker_initialization_internal::sampleLastRecordPerNsUUID(opCtx, preImagesRAII);
    ASSERT_EQ(1, lastRecordsMap.size());
    auto it = lastRecordsMap.find(kNsUUID);
    ASSERT(it != lastRecordsMap.end());
    const auto [actualRid, actualWallTime] = it->second;
    const auto [expectedRid, expectedWallTime] = extractRecordIdAndWallTime(kPreImage3);
    ASSERT_EQ(expectedRid, actualRid);
    ASSERT_EQ(expectedWallTime, actualWallTime);
}

TEST_F(PreImageMarkerInitializationTest, SampleLastRecordMultipleNsUUIDs) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);

    // 2 pre-images for 'kNsUUID', 2 pre-images for 'kNsUUIDOther'.
    insertDirectlyToPreImagesCollection(opCtx, kPreImage1);
    insertDirectlyToPreImagesCollection(opCtx, kPreImage2);
    insertDirectlyToPreImagesCollection(opCtx, kPreImageOtherMin);
    insertDirectlyToPreImagesCollection(opCtx, kPreImageOther);

    const auto preImagesRAII = acquirePreImagesCollectionForRead(opCtx);
    const auto lastRecordsMap =
        pre_image_marker_initialization_internal::sampleLastRecordPerNsUUID(opCtx, preImagesRAII);
    ASSERT_EQ(2, lastRecordsMap.size());

    stdx::unordered_map<UUID, CollectionTruncateMarkers::RecordIdAndWallTime> expectedLastRecords{
        {kNsUUID, extractRecordIdAndWallTime(kPreImage2)},
        {kNsUUIDOther, extractRecordIdAndWallTime(kPreImageOther)}};

    ASSERT_EQ(expectedLastRecords.size(), lastRecordsMap.size());
    for (const auto& [uuid, ridAndWall] : expectedLastRecords) {
        auto it = lastRecordsMap.find(uuid);
        ASSERT(it != lastRecordsMap.end());
        const auto [actualRid, actualWallTime] = it->second;
        const auto [expectedRid, expectedWallTime] = ridAndWall;
        ASSERT_EQ(expectedRid, actualRid);
        ASSERT_EQ(expectedWallTime, actualWallTime);
    }
}

TEST_F(PreImageMarkerInitializationTest, PreImageSamplesFromEmptyCollection) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);

    const auto preImagesRAII = acquirePreImagesCollectionForRead(opCtx);

    std::vector<uint64_t> targetSampleOptions{0, 1, 2, 100};
    for (const auto& targetNumSamples : targetSampleOptions) {
        LOGV2(9528901,
              "Running test case to retrieve pre-image samples on an empty collection",
              "targetNumSamples"_attr = targetNumSamples);
        const auto samples = pre_image_marker_initialization_internal::collectPreImageSamples(
            opCtx, preImagesRAII, targetNumSamples);
        ASSERT_EQ(0, samples.size());
    }
}

// Tests there is at least 1 sample per nsUUID, even if less samples are requested.
TEST_F(PreImageMarkerInitializationTest, PreImageSamplesAlwaysContainLastRecordPerNsUUID) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);

    auto assert1SampleForNsUUID =
        [](const UUID& nsUUID,
           const ChangeStreamPreImage& expectedPreImage,
           const stdx::unordered_map<UUID,
                                     std::vector<CollectionTruncateMarkers::RecordIdAndWallTime>,
                                     UUID::Hash>& samples) {
            auto it = samples.find(nsUUID);
            ASSERT(it != samples.end());
            auto vec = it->second;
            ASSERT_EQ(1, it->second.size());
            const auto actualSample = it->second[0];
            const auto expectedSample = extractRecordIdAndWallTime(expectedPreImage);
            ASSERT_EQ(expectedSample.id, actualSample.id);
            ASSERT_EQ(expectedSample.wall, actualSample.wall);
        };

    {
        // Test Case: 1 pre-image for 'kNsUUID' in the pre-images collection with 0 samples
        // requested yields 1 sample.
        insertDirectlyToPreImagesCollection(opCtx, kPreImage1);
        auto preImagesRAII = acquirePreImagesCollectionForRead(opCtx);
        const auto samples = pre_image_marker_initialization_internal::collectPreImageSamples(
            opCtx, preImagesRAII, 0);
        assert1SampleForNsUUID(kNsUUID, kPreImage1, samples);
    }

    {
        // Test Case: 2 pre-images for 'kNsUUID' in the pre-images collection with 0 samples
        // requested yields 1 sample.
        insertDirectlyToPreImagesCollection(opCtx, kPreImage2);
        auto preImagesRAII = acquirePreImagesCollectionForRead(opCtx);
        const auto samples = pre_image_marker_initialization_internal::collectPreImageSamples(
            opCtx, preImagesRAII, 0);
        assert1SampleForNsUUID(kNsUUID, kPreImage2, samples);
    }

    {
        // Test Case: 2 pre-images for 'kNsUUID', 1 for 'kNsOtherUUID' in the pre-images collection
        // with 0 samples requested yields 1 sample per nsUUID.
        insertDirectlyToPreImagesCollection(opCtx, kPreImageOther);
        auto preImagesRAII = acquirePreImagesCollectionForRead(opCtx);
        const auto samples = pre_image_marker_initialization_internal::collectPreImageSamples(
            opCtx, preImagesRAII, 0);
        assert1SampleForNsUUID(kNsUUID, kPreImage2, samples);
        assert1SampleForNsUUID(kNsUUIDOther, kPreImageOther, samples);
    }
}

// Demonstrates that pre-image sampling can result in repeated results of the same pre-image on a
// non-empty collection.
TEST_F(PreImageMarkerInitializationTest, PreImageSamplesRepeatSamples) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);

    const Timestamp baseTimestamp = Timestamp(1, 1);
    const Date_t baseDate = dateFromISOString("2024-01-01T00:00:01.000Z").getValue();

    const auto kNsUUID = UUID::gen();
    const auto preImage = makePreImage(kNsUUID, baseTimestamp, baseDate);
    insertDirectlyToPreImagesCollection(opCtx, preImage);

    const auto preImagesRAII = acquirePreImagesCollectionForRead(opCtx);

    std::vector<int64_t> targetSampleOptions{1, 10};
    for (const auto& targetNumSamples : targetSampleOptions) {
        LOGV2(9528902,
              "Running test case to retrieve pre-image samples on an empty collection",
              "targetNumSamples"_attr = targetNumSamples);
        const auto samples = pre_image_marker_initialization_internal::collectPreImageSamples(
            opCtx, preImagesRAII, targetNumSamples);
        ASSERT_EQ(targetNumSamples,
                  pre_image_marker_initialization_internal::countTotalSamples(samples));
    }
}

TEST_F(PreImageMarkerInitializationTest, PopulateMapWithEmptyCollection) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);
    const auto minBytesPerMarker = gPreImagesCollectionTruncateMarkersMinBytes;

    {
        // Populate by scanning an empty collection.
        ConcurrentSharedValuesMap<UUID, PreImagesTruncateMarkersPerNsUUID, UUID::Hash> mapByScan;
        auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);
        pre_image_marker_initialization_internal::populateByScanning(
            opCtx, preImagesCollection, minBytesPerMarker, mapByScan);
        const auto mapSnapshot = mapByScan.getUnderlyingSnapshot();
        ASSERT_EQ(0, mapSnapshot->size());
    }

    {
        // Populate by sampling an empty collection where the initial 'totalRecords' and
        // 'totalBytes' estimates are accurate.
        ConcurrentSharedValuesMap<UUID, PreImagesTruncateMarkersPerNsUUID, UUID::Hash> mapBySamples;
        auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);
        pre_image_marker_initialization_internal::populateBySampling(
            opCtx,
            preImagesCollection,
            0 /* totalRecords */,
            0 /* totalBytes */,
            minBytesPerMarker,
            CollectionTruncateMarkers::kRandomSamplesPerMarker,
            mapBySamples);
        const auto mapSnapshot = mapBySamples.getUnderlyingSnapshot();
        ASSERT_EQ(0, mapSnapshot->size());
    }

    {
        // Populate by sampling an empty collection where the initial 'totalRecords' and
        // 'totalBytes' estimates aren't accurate. The size tracked in-memory can be inaccurate
        // after unclean shutdowns.
        ConcurrentSharedValuesMap<UUID, PreImagesTruncateMarkersPerNsUUID, UUID::Hash> mapBySamples;
        auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);
        pre_image_marker_initialization_internal::populateBySampling(
            opCtx,
            preImagesCollection,
            100 /* totalRecords */,
            200 /* totalBytes */,
            minBytesPerMarker,
            CollectionTruncateMarkers::kRandomSamplesPerMarker,
            mapBySamples);
        const auto mapSnapshot = mapBySamples.getUnderlyingSnapshot();
        ASSERT_EQ(0, mapSnapshot->size());
    }
}

TEST_F(PreImageMarkerInitializationTest, PopulateMapWithSinglePreImage) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);
    insertDirectlyToPreImagesCollection(opCtx, kPreImage1);
    auto assertPreImageIsTracked =
        [](const UUID& expectedUUID,
           const ConcurrentSharedValuesMap<UUID, PreImagesTruncateMarkersPerNsUUID, UUID::Hash>&
               csvMap,
           const ChangeStreamPreImage& expectedPreImage) {
            const auto mapSnapshot = csvMap.getUnderlyingSnapshot();
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
        ConcurrentSharedValuesMap<UUID, PreImagesTruncateMarkersPerNsUUID, UUID::Hash> mapByScan;
        auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);
        pre_image_marker_initialization_internal::populateByScanning(
            opCtx, preImagesCollection, 1 /* minBytesPerMarker */, mapByScan);
        assertPreImageIsTracked(kNsUUID, mapByScan, kPreImage1);
    }

    {
        // Populate by scanning, pre-image not covered in marker.
        ConcurrentSharedValuesMap<UUID, PreImagesTruncateMarkersPerNsUUID, UUID::Hash> mapByScan;
        auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);
        pre_image_marker_initialization_internal::populateByScanning(
            opCtx, preImagesCollection, bytes(kPreImage1) + 100 /* minBytesPerMarker */, mapByScan);
        assertPreImageIsTracked(kNsUUID, mapByScan, kPreImage1);
    }

    {
        ConcurrentSharedValuesMap<UUID, PreImagesTruncateMarkersPerNsUUID, UUID::Hash> mapBySamples;
        auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);
        pre_image_marker_initialization_internal::populateBySampling(
            opCtx,
            preImagesCollection,
            1 /* totalRecords */,
            bytes(kPreImage1) /* totalBytes */,
            1 /* minBytesPerMarker */,
            1 /* randomSamplesPerMarker */,
            mapBySamples);
        assertPreImageIsTracked(kNsUUID, mapBySamples, kPreImage1);
    }
}

}  // namespace
}  // namespace mongo
