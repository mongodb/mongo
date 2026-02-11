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
#include "mongo/db/change_stream_pre_images_truncate_markers.h"

#include "mongo/db/change_stream_pre_image_test_helpers.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {
using namespace change_stream_pre_image_test_helper;
using namespace pre_image_marker_initialization_internal;

/**
 * Tests components of pre-image truncate marker initialization across a pre-image collection.
 */
class PreImageMarkerInitializationTest : public CatalogTestFixture,
                                         public ChangeStreamPreImageTestConstants {
public:
    void assertEqualRangeSampleForNsUUIDContainsExactPreimages(
        uint64_t numSamples, const std::vector<ChangeStreamPreImage>& expectedPreImages) {
        auto preImagesCollection = acquirePreImagesCollectionForRead(operationContext());

        const auto nsUUIDLastRecords =
            sampleLastRecordPerNsUUID(operationContext(), preImagesCollection);

        // Sampling last records should have produced exactly one entry for kNsUUID.
        ASSERT_EQ(1, nsUUIDLastRecords.size());
        ASSERT_TRUE(nsUUIDLastRecords.contains(kNsUUID));
        const auto& lastRidAndWall = nsUUIDLastRecords.find(kNsUUID)->second;

        auto samples = sampleNSUUIDRangeEqually(
            operationContext(), preImagesCollection, kNsUUID, lastRidAndWall, numSamples);

        ASSERT_EQ(expectedPreImages.size(), samples.size());

        int i = 0;
        for (const auto& expectedPreImage : expectedPreImages) {
            ASSERT_EQ(extractRecordIdAndWallTime(expectedPreImage).id, samples[i++].id);
        }
    }
};

using PreImageMarkerInitializationDeathTest = PreImageMarkerInitializationTest;

/**
 * Tests for 'sampleLastRecordPerNsUUID'.
 */
TEST_F(PreImageMarkerInitializationTest, SampleEmptyCollection) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);

    const auto preImagesRAII = acquirePreImagesCollectionForRead(opCtx);
    const auto lastRecordsMap = sampleLastRecordPerNsUUID(opCtx, preImagesRAII);
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
    const auto lastRecordsMap = sampleLastRecordPerNsUUID(opCtx, preImagesRAII);
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
    const auto lastRecordsMap = sampleLastRecordPerNsUUID(opCtx, preImagesRAII);
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

/**
 * Tests for 'collectPreImageSamples'.
 */
TEST_F(PreImageMarkerInitializationTest, PreImageSamplesFromEmptyCollection) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);

    const auto preImagesRAII = acquirePreImagesCollectionForRead(opCtx);

    std::vector<uint64_t> targetSampleOptions{0, 1, 2, 100};
    for (const auto& targetNumSamples : targetSampleOptions) {
        LOGV2(9528901,
              "Running test case to retrieve pre-image samples on an empty collection",
              "targetNumSamples"_attr = targetNumSamples);
        const auto samples = collectPreImageSamples(opCtx, preImagesRAII, targetNumSamples);
        ASSERT_EQ(0, samples.size());
    }
}

// Tests there is at least 1 sample per nsUUID, even if less samples are requested.
TEST_F(PreImageMarkerInitializationTest, PreImageSamplesAlwaysContainLastRecordPerNsUUID) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);

    auto assert1SampleForNsUUID = [](const UUID& nsUUID,
                                     const ChangeStreamPreImage& expectedPreImage,
                                     const SamplesMap& samples) {
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
        const auto samples = collectPreImageSamples(opCtx, preImagesRAII, 0);
        assert1SampleForNsUUID(kNsUUID, kPreImage1, samples);
    }

    {
        // Test Case: 2 pre-images for 'kNsUUID' in the pre-images collection with 0 samples
        // requested yields 1 sample.
        insertDirectlyToPreImagesCollection(opCtx, kPreImage2);
        auto preImagesRAII = acquirePreImagesCollectionForRead(opCtx);
        const auto samples = collectPreImageSamples(opCtx, preImagesRAII, 0);
        assert1SampleForNsUUID(kNsUUID, kPreImage2, samples);
    }

    {
        // Test Case: 2 pre-images for 'kNsUUID', 1 for 'kNsOtherUUID' in the pre-images collection
        // with 0 samples requested yields 1 sample per nsUUID.
        insertDirectlyToPreImagesCollection(opCtx, kPreImageOther);
        auto preImagesRAII = acquirePreImagesCollectionForRead(opCtx);
        const auto samples = collectPreImageSamples(opCtx, preImagesRAII, 0);
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
        const auto samples = collectPreImageSamples(opCtx, preImagesRAII, targetNumSamples);
        ASSERT_EQ(targetNumSamples, countTotalSamples(samples));
    }
}

/**
 * Tests for scanning / sampling.
 */
TEST_F(PreImageMarkerInitializationTest, PopulateMapWithEmptyCollection) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);
    const auto minBytesPerMarker = gPreImagesCollectionTruncateMarkersMinBytes;

    {
        // Populate by scanning an empty collection.
        MarkersMap mapByScan;
        auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);
        populateByScanning(opCtx, preImagesCollection, minBytesPerMarker, mapByScan);
        const auto mapSnapshot = mapByScan.getUnderlyingSnapshot();
        ASSERT_EQ(0, mapSnapshot->size());
    }

    {
        // Populate by sampling an empty collection where the initial 'totalRecords' and
        // 'totalBytes' estimates are accurate.
        MarkersMap mapBySamples;
        auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);
        populateByRandomSampling(opCtx,
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
        MarkersMap mapBySamples;
        auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);
        populateByRandomSampling(opCtx,
                                 preImagesCollection,
                                 100 /* totalRecords */,
                                 200 /* totalBytes */,
                                 minBytesPerMarker,
                                 CollectionTruncateMarkers::kRandomSamplesPerMarker,
                                 mapBySamples);
        const auto mapSnapshot = mapBySamples.getUnderlyingSnapshot();
        ASSERT_EQ(0, mapSnapshot->size());
    }

    {
        // Populate by sampling an empty collection using the "equal step" sampling method.
        MarkersMap mapBySamples;
        auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);
        populateByEqualStepSampling(opCtx,
                                    preImagesCollection,
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
    auto assertPreImageIsTracked = [](const UUID& expectedUUID,
                                      const MarkersMap& csvMap,
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
        MarkersMap mapByScan;
        auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);
        populateByScanning(opCtx, preImagesCollection, 1 /* minBytesPerMarker */, mapByScan);
        assertPreImageIsTracked(kNsUUID, mapByScan, kPreImage1);
    }

    {
        // Populate by scanning, pre-image not covered in marker.
        MarkersMap mapByScan;
        auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);
        populateByScanning(
            opCtx, preImagesCollection, bytes(kPreImage1) + 100 /* minBytesPerMarker */, mapByScan);
        assertPreImageIsTracked(kNsUUID, mapByScan, kPreImage1);
    }

    {
        MarkersMap mapBySamples;
        auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);
        populateByRandomSampling(opCtx,
                                 preImagesCollection,
                                 1 /* totalRecords */,
                                 bytes(kPreImage1) /* totalBytes */,
                                 1 /* minBytesPerMarker */,
                                 1 /* randomSamplesPerMarker */,
                                 mapBySamples);
        assertPreImageIsTracked(kNsUUID, mapBySamples, kPreImage1);
    }

    {
        MarkersMap mapBySamples;
        auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);
        populateByEqualStepSampling(opCtx, preImagesCollection, 1 /* numSamples */, mapBySamples);
        assertPreImageIsTracked(kNsUUID, mapBySamples, kPreImage1);
    }
}

DEATH_TEST_REGEX_F(PreImageMarkerInitializationDeathTest,
                   populateByEqualStepSamplingSampleSize0,
                   "Tripwire assertion.*11423701") {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);
    insertDirectlyToPreImagesCollection(opCtx, kPreImage1);

    auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);

    MarkersMap markersMap;
    populateByEqualStepSampling(opCtx, preImagesCollection, 0 /* numSamples */, markersMap);
}

/**
 * Tests for 'sampleNSUUIDRangeEqually.
 */
DEATH_TEST_REGEX_F(PreImageMarkerInitializationDeathTest,
                   SampleNSUUIDRangeEquallySampleSize0,
                   "Tripwire assertion.*11423700") {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);
    insertDirectlyToPreImagesCollection(opCtx, kPreImage1);

    auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);

    const auto nsUUIDLastRecords = sampleLastRecordPerNsUUID(opCtx, preImagesCollection);

    // Sampling last records should have produced exactly one entry for kNsUUID.
    ASSERT_EQ(1, nsUUIDLastRecords.size());
    ASSERT_TRUE(nsUUIDLastRecords.contains(kNsUUID));
    const auto& lastRidAndWall = nsUUIDLastRecords.find(kNsUUID)->second;

    sampleNSUUIDRangeEqually(
        opCtx, preImagesCollection, kNsUUID, lastRidAndWall, 0 /* numSamples */);
}

// Tests for sample size 1, which has a few special cases.
TEST_F(PreImageMarkerInitializationTest, SampleNSUUIDRangeEquallySampleSize1) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);

    constexpr uint64_t kNumSamples = 1;

    // Collection contains a single entry for 'kNsUUID', and we expect it to be sampled.
    insertDirectlyToPreImagesCollection(opCtx, kPreImage1);
    {
        assertEqualRangeSampleForNsUUIDContainsExactPreimages(
            kNumSamples, std::vector<ChangeStreamPreImage>{kPreImage1});
    }

    // Collection contains two entries for 'kNsUUID', but we expect 'kPreImage2' to be the only
    // document sampled.
    insertDirectlyToPreImagesCollection(opCtx, kPreImage2);
    {
        assertEqualRangeSampleForNsUUIDContainsExactPreimages(
            kNumSamples, std::vector<ChangeStreamPreImage>{kPreImage2});
    }

    // Collection contains three entries for 'kNsUUID', but we expect 'kPreImage3' to be the only
    // document sampled.
    insertDirectlyToPreImagesCollection(opCtx, kPreImage3);
    {
        assertEqualRangeSampleForNsUUIDContainsExactPreimages(
            kNumSamples, std::vector<ChangeStreamPreImage>{kPreImage3});
    }
}

// Tests for sample size 2, which has a few special cases.
TEST_F(PreImageMarkerInitializationTest, SampleNSUUIDRangeEquallySampleSize2) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);

    constexpr uint64_t kNumSamples = 2;

    // Collection contains a single entry for 'kNsUUID'.
    insertDirectlyToPreImagesCollection(opCtx, kPreImage1);
    {
        assertEqualRangeSampleForNsUUIDContainsExactPreimages(
            kNumSamples, std::vector<ChangeStreamPreImage>{kPreImage1});
    }

    // Collection contains two entries for 'kNsUUID', and we expect both to be contained in the
    // sample.
    insertDirectlyToPreImagesCollection(opCtx, kPreImage2);
    {
        assertEqualRangeSampleForNsUUIDContainsExactPreimages(
            kNumSamples, std::vector<ChangeStreamPreImage>{kPreImage1, kPreImage2});
    }

    // Collection contains three entries for 'kNsUUID', and we 'kPreImage1' and 'kPreImage3' to be
    // sampled.
    insertDirectlyToPreImagesCollection(opCtx, kPreImage3);
    {
        assertEqualRangeSampleForNsUUIDContainsExactPreimages(
            kNumSamples, std::vector<ChangeStreamPreImage>{kPreImage1, kPreImage3});
    }
}

// Tests for a sample size larger than the number of documents in the collection.
TEST_F(PreImageMarkerInitializationTest, SampleNSUUIDRangeEquallySampleSizeLargerThanCollection) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);

    constexpr uint64_t kNumSamples = 20;

    // Collection contains a single entry for 'kNsUUID'.
    insertDirectlyToPreImagesCollection(opCtx, kPreImage1);
    {
        assertEqualRangeSampleForNsUUIDContainsExactPreimages(
            kNumSamples, std::vector<ChangeStreamPreImage>{kPreImage1});
    }

    // Collection contains two entries for 'kNsUUID', and we expect both to be contained in the
    // sample.
    insertDirectlyToPreImagesCollection(opCtx, kPreImage2);
    {
        assertEqualRangeSampleForNsUUIDContainsExactPreimages(
            kNumSamples, std::vector<ChangeStreamPreImage>{kPreImage1, kPreImage2});
    }

    // Collection contains three entries for 'kNsUUID', and we expect them all to be sampled.
    insertDirectlyToPreImagesCollection(opCtx, kPreImage3);
    {
        assertEqualRangeSampleForNsUUIDContainsExactPreimages(
            kNumSamples, std::vector<ChangeStreamPreImage>{kPreImage1, kPreImage2, kPreImage3});
    }
}

// Test sampling when the document timestamps in the collection are almost equally distributed.
TEST_F(PreImageMarkerInitializationTest,
       SampleNSUUIDRangeEquallyCollectionWithEquallyDistancedEntries) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);

    const long long baseTime =
        dateFromISOString("2024-01-01T00:00:01.000Z").getValue().toMillisSinceEpoch() / 1'000;

    auto buildPreImage = [](UUID uuid,
                            Timestamp ts,
                            int64_t applyOpsIndex,
                            const BSONObj& data) -> ChangeStreamPreImage {
        return {ChangeStreamPreImageId{uuid, ts, applyOpsIndex}, Date_t{}, data};
    };

    // Insert 100 documents with increasing timestamp values.
    for (int i = 0; i < 100; ++i) {
        auto preImage =
            buildPreImage(kNsUUID, Timestamp(Seconds(baseTime + i), 0), 0, BSON("x" << i));
        insertDirectlyToPreImagesCollection(opCtx, preImage);
    }

    auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);

    const auto nsUUIDLastRecords = sampleLastRecordPerNsUUID(opCtx, preImagesCollection);

    // Sampling last records should have produced exactly one entry for kNsUUID.
    ASSERT_EQ(1, nsUUIDLastRecords.size());
    ASSERT_TRUE(nsUUIDLastRecords.contains(kNsUUID));

    constexpr uint64_t kNumSamples = 10;

    // For a sample size of 10, expect documents with the following timestamps to be sampled.
    std::vector<ChangeStreamPreImage> expectedPreImages;
    for (int i : {0, 11, 22, 33, 44, 55, 66, 77, 88, 99}) {
        expectedPreImages.push_back(
            buildPreImage(kNsUUID, Timestamp(Seconds(baseTime + i), 0), 0, BSON("x" << i)));
    }
    assertEqualRangeSampleForNsUUIDContainsExactPreimages(kNumSamples, expectedPreImages);
}

// Test sampling when the document timestamps in the collection are unevenly distributed.
TEST_F(PreImageMarkerInitializationTest,
       SampleNSUUIDRangeEquallyCollectionWithUnevenlyDistancedEntries) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);

    const long long baseTime =
        dateFromISOString("2024-01-01T00:00:01.000Z").getValue().toMillisSinceEpoch() / 1'000;

    auto buildPreImage = [](UUID uuid,
                            Timestamp ts,
                            int64_t applyOpsIndex,
                            const BSONObj& data) -> ChangeStreamPreImage {
        return {ChangeStreamPreImageId{uuid, ts, applyOpsIndex}, Date_t{}, data};
    };

    // Insert 50 documents with geometrically increasing timestamp values.
    for (int i = 0; i < 50; ++i) {
        auto preImage =
            buildPreImage(kNsUUID, Timestamp(Seconds(baseTime + i * i), 0), 0, BSON("x" << i));
        insertDirectlyToPreImagesCollection(opCtx, preImage);
    }

    auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);

    const auto nsUUIDLastRecords = sampleLastRecordPerNsUUID(opCtx, preImagesCollection);

    // Sampling last records should have produced exactly one entry for kNsUUID.
    ASSERT_EQ(1, nsUUIDLastRecords.size());
    ASSERT_TRUE(nsUUIDLastRecords.contains(kNsUUID));

    constexpr uint64_t kNumSamples = 15;

    // For a sample size of 15, expect documents with the following timestamps to be sampled.
    // Note that due to the skewed distribution, we'll actually get less than the requested number
    // of samples (13 instead of 15).
    std::vector<ChangeStreamPreImage> expectedPreImages;
    for (int i : {0, 196, 400, 576, 784, 961, 1156, 1369, 1600, 1849, 2025, 2209, 2401}) {
        expectedPreImages.push_back(
            buildPreImage(kNsUUID, Timestamp(Seconds(baseTime + i), 0), 0, BSON("x" << i)));
    }
    assertEqualRangeSampleForNsUUIDContainsExactPreimages(kNumSamples, expectedPreImages);
}

// Test sampling when the document timestamps in the collection have the same Timestamp 't' value,
// but different Timestamp 'i' values.
TEST_F(PreImageMarkerInitializationTest,
       SampleNSUUIDRangeEquallyCollectionWithTimestampsDifferingOnlyByIncrements) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);

    const long long baseTime =
        dateFromISOString("2024-01-01T00:00:01.000Z").getValue().toMillisSinceEpoch() / 1'000;

    auto buildPreImage = [](UUID uuid,
                            Timestamp ts,
                            int64_t applyOpsIndex,
                            const BSONObj& data) -> ChangeStreamPreImage {
        return {ChangeStreamPreImageId{uuid, ts, applyOpsIndex}, Date_t{}, data};
    };

    // Insert 100 documents with increasing timestamp values (Timestamp 't' values are identical
    // here, only the 'i' values are different).
    for (int i = 0; i < 100; ++i) {
        auto preImage = buildPreImage(kNsUUID, Timestamp(Seconds(baseTime), i), 0, BSON("x" << i));
        insertDirectlyToPreImagesCollection(opCtx, preImage);
    }

    auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);

    const auto nsUUIDLastRecords = sampleLastRecordPerNsUUID(opCtx, preImagesCollection);

    // Sampling last records should have produced exactly one entry for kNsUUID.
    ASSERT_EQ(1, nsUUIDLastRecords.size());
    ASSERT_TRUE(nsUUIDLastRecords.contains(kNsUUID));

    constexpr uint64_t kNumSamples = 10;

    // For a sample size of 10, expect documents with the following timestamps to be sampled.
    std::vector<ChangeStreamPreImage> expectedPreImages;
    for (int i : {0, 11, 22, 33, 44, 55, 66, 77, 88, 99}) {
        expectedPreImages.push_back(
            buildPreImage(kNsUUID, Timestamp(Seconds(baseTime), i), 0, BSON("x" << i)));
    }
    assertEqualRangeSampleForNsUUIDContainsExactPreimages(kNumSamples, expectedPreImages);
}

// Test for sampling documents that have the same Timestamp value, but different 'applyOpsIndex'
// values.
TEST_F(PreImageMarkerInitializationTest,
       SampleNSUUIDRangeEquallySameTimestampDifferentApplyOpsIndex) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);

    const long long baseTime =
        dateFromISOString("2024-01-01T00:00:01.000Z").getValue().toMillisSinceEpoch() / 1'000;

    auto buildPreImage = [](UUID uuid,
                            Timestamp ts,
                            int64_t applyOpsIndex,
                            const BSONObj& data) -> ChangeStreamPreImage {
        return {ChangeStreamPreImageId{uuid, ts, applyOpsIndex}, Date_t{}, data};
    };

    // Insert 50 documents with same timestamp, but different 'applyOpsIndex' values.
    for (int i = 0; i < 50; ++i) {
        auto preImage = buildPreImage(
            kNsUUID, Timestamp(Seconds(baseTime), 0), i /* applyOpsIndex */, BSON("x" << i));
        insertDirectlyToPreImagesCollection(opCtx, preImage);
    }

    auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);

    const auto nsUUIDLastRecords = sampleLastRecordPerNsUUID(opCtx, preImagesCollection);

    // Sampling last records should have produced exactly one entry for kNsUUID.
    ASSERT_EQ(1, nsUUIDLastRecords.size());
    ASSERT_TRUE(nsUUIDLastRecords.contains(kNsUUID));

    constexpr uint64_t kNumSamples = 10;

    // The sampling algorithm is based on Timestamp distances. It does not produce good results in
    // case all documents have the same Timestamp value, and only differ in terms of 'applyOpsIndex'
    // values.
    std::vector<ChangeStreamPreImage> expectedPreImages;
    for (int i : {0, 49}) {
        expectedPreImages.push_back(buildPreImage(
            kNsUUID, Timestamp(Seconds(baseTime), 0), i /* applyOpsIndex */, BSON("x" << i)));
    }
    assertEqualRangeSampleForNsUUIDContainsExactPreimages(kNumSamples, expectedPreImages);
}

}  // namespace
}  // namespace mongo
