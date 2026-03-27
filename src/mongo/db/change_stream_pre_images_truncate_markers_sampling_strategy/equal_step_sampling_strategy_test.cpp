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
#include "mongo/db/change_stream_pre_images_truncate_markers_sampling_strategy/equal_step_sampling_strategy.h"

#include "mongo/db/change_stream_pre_image_test_helpers.h"
#include "mongo/db/change_stream_pre_images_truncate_markers.h"
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

BSONObj recordIdToBSON(const RecordId& rid) {
    BSONObjBuilder bob;
    record_id_helpers::appendToBSONAs(rid, &bob, "");
    return bob.obj();
}

/**
 * Tests components of pre-image truncate marker initialization across a pre-image collection.
 */
class EqualStepSamplingStrategyTest : public CatalogTestFixture,
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

        EqualStepSamplingStrategy samplingStrategy(numSamples, 1);
        auto samples = samplingStrategy.sampleNSUUIDRangeEqually(
            operationContext(), preImagesCollection, kNsUUID, lastRidAndWall, numSamples);

        ASSERT_EQ(expectedPreImages.size(), samples.size());

        int i = 0;
        for (const auto& expectedPreImage : expectedPreImages) {
            ASSERT_EQ(extractRecordIdAndWallTime(expectedPreImage).id, samples[i].id)
                << "\nexpected count: " << samples.size() << "\ni: " << i
                << "\nexpected: " << expectedPreImage.toBSON()
                << "\nactual: " << recordIdToBSON(samples[i].id);
            i++;
        }
    }
};

using EqualStepSamplingStrategyDeathTest = EqualStepSamplingStrategyTest;

/**
 * Test for sampling an empty collection.
 */
TEST_F(EqualStepSamplingStrategyTest, PopulateMapWithEmptyCollection) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);

    // Populate by sampling an empty collection using the "equal step" sampling method.
    MarkersMap mapBySamples;
    auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);
    EqualStepSamplingStrategy samplingStrategy(CollectionTruncateMarkers::kRandomSamplesPerMarker,
                                               1);
    ASSERT_TRUE(samplingStrategy.performSampling(opCtx, preImagesCollection, mapBySamples));
    const auto mapSnapshot = mapBySamples.getUnderlyingSnapshot();
    ASSERT_EQ(0, mapSnapshot->size());
}

/**
 * Tests for 'sampleNSUUIDRangeEqually'.
 */
DEATH_TEST_REGEX_F(EqualStepSamplingStrategyDeathTest,
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

    EqualStepSamplingStrategy samplingStrategy(1, 1);
    samplingStrategy.sampleNSUUIDRangeEqually(
        opCtx, preImagesCollection, kNsUUID, lastRidAndWall, 0 /* numSamples */);
}

// Tests for sample size 1, which has a few special cases.
TEST_F(EqualStepSamplingStrategyTest, SampleNSUUIDRangeEquallySampleSize1) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);

    constexpr uint64_t kNumSamples = 1;

    // Collection contains a single entry for 'kNsUUID', and we expect it to be sampled.
    insertDirectlyToPreImagesCollection(opCtx, kPreImage1);
    assertEqualRangeSampleForNsUUIDContainsExactPreimages(
        kNumSamples, std::vector<ChangeStreamPreImage>{kPreImage1});

    // Collection contains two entries for 'kNsUUID', but we expect 'kPreImage2' to be the only
    // document sampled.
    insertDirectlyToPreImagesCollection(opCtx, kPreImage2);
    assertEqualRangeSampleForNsUUIDContainsExactPreimages(
        kNumSamples, std::vector<ChangeStreamPreImage>{kPreImage2});

    // Collection contains three entries for 'kNsUUID', but we expect 'kPreImage3' to be the only
    // document sampled.
    insertDirectlyToPreImagesCollection(opCtx, kPreImage3);
    assertEqualRangeSampleForNsUUIDContainsExactPreimages(
        kNumSamples, std::vector<ChangeStreamPreImage>{kPreImage3});
}

// Tests for sample size 2, which has a few special cases.
TEST_F(EqualStepSamplingStrategyTest, SampleNSUUIDRangeEquallySampleSize2) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);

    constexpr uint64_t kNumSamples = 2;

    // Collection contains a single entry for 'kNsUUID'.
    insertDirectlyToPreImagesCollection(opCtx, kPreImage1);
    assertEqualRangeSampleForNsUUIDContainsExactPreimages(
        kNumSamples, std::vector<ChangeStreamPreImage>{kPreImage1});

    // Collection contains two entries for 'kNsUUID', and we expect both to be contained in the
    // sample.
    insertDirectlyToPreImagesCollection(opCtx, kPreImage2);
    assertEqualRangeSampleForNsUUIDContainsExactPreimages(
        kNumSamples, std::vector<ChangeStreamPreImage>{kPreImage1, kPreImage2});

    // Collection contains three entries for 'kNsUUID', and we 'kPreImage1' and 'kPreImage3' to be
    // sampled.
    insertDirectlyToPreImagesCollection(opCtx, kPreImage3);
    assertEqualRangeSampleForNsUUIDContainsExactPreimages(
        kNumSamples, std::vector<ChangeStreamPreImage>{kPreImage1, kPreImage3});
}

// Tests for a sample size larger than the number of documents in the collection.
TEST_F(EqualStepSamplingStrategyTest, SampleNSUUIDRangeEquallySampleSizeLargerThanCollection) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);

    constexpr uint64_t kNumSamples = 20;

    // Collection contains a single entry for 'kNsUUID'.
    insertDirectlyToPreImagesCollection(opCtx, kPreImage1);
    assertEqualRangeSampleForNsUUIDContainsExactPreimages(
        kNumSamples, std::vector<ChangeStreamPreImage>{kPreImage1});

    // Collection contains two entries for 'kNsUUID', and we expect both to be contained in the
    // sample.
    insertDirectlyToPreImagesCollection(opCtx, kPreImage2);
    assertEqualRangeSampleForNsUUIDContainsExactPreimages(
        kNumSamples, std::vector<ChangeStreamPreImage>{kPreImage1, kPreImage2});

    // Collection contains three entries for 'kNsUUID', and we expect them all to be sampled.
    insertDirectlyToPreImagesCollection(opCtx, kPreImage3);
    assertEqualRangeSampleForNsUUIDContainsExactPreimages(
        kNumSamples, std::vector<ChangeStreamPreImage>{kPreImage1, kPreImage2, kPreImage3});
}

// Test sampling when the document timestamps in the collection are almost equally distributed.
TEST_F(EqualStepSamplingStrategyTest,
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

    // For a sample size of 10, expect documents with the following timestamps to be sampled.
    std::vector<ChangeStreamPreImage> expectedPreImages;
    for (int i : {0, 11, 22, 33, 44, 55, 66, 77, 88, 99}) {
        expectedPreImages.push_back(
            buildPreImage(kNsUUID, Timestamp(Seconds(baseTime + i), 0), 0, BSON("x" << i)));
    }

    constexpr uint64_t kNumSamples = 10;
    assertEqualRangeSampleForNsUUIDContainsExactPreimages(kNumSamples, expectedPreImages);
}

// Test sampling when the document timestamps in the collection are unevenly distributed.
TEST_F(EqualStepSamplingStrategyTest,
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

    // For a sample size of 15, expect documents with the following timestamps to be sampled.
    // Note that due to the skewed distribution, we'll actually get less than the requested number
    // of samples (13 instead of 15).
    std::vector<ChangeStreamPreImage> expectedPreImages;
    for (int i : {0, 196, 400, 576, 784, 961, 1156, 1369, 1600, 1849, 2025, 2209, 2401}) {
        expectedPreImages.push_back(
            buildPreImage(kNsUUID, Timestamp(Seconds(baseTime + i), 0), 0, BSON("x" << i)));
    }

    constexpr uint64_t kNumSamples = 15;
    assertEqualRangeSampleForNsUUIDContainsExactPreimages(kNumSamples, expectedPreImages);
}

// Test sampling when the document timestamps in the collection have the same Timestamp 't' value,
// but different Timestamp 'i' values.
TEST_F(EqualStepSamplingStrategyTest,
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

    // For a sample size of 10, expect documents with the following timestamps to be sampled.
    std::vector<ChangeStreamPreImage> expectedPreImages;
    for (int i : {0, 11, 22, 33, 44, 55, 66, 77, 88, 99}) {
        expectedPreImages.push_back(
            buildPreImage(kNsUUID, Timestamp(Seconds(baseTime), i), 0, BSON("x" << i)));
    }

    constexpr uint64_t kNumSamples = 10;
    assertEqualRangeSampleForNsUUIDContainsExactPreimages(kNumSamples, expectedPreImages);
}

// Test for sampling documents that have the same Timestamp value, but different 'applyOpsIndex'
// values.
TEST_F(EqualStepSamplingStrategyTest, SampleNSUUIDRangeEquallySameTimestampDifferentApplyOpsIndex) {
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

    // The different 'applyOpsIndex' values here are not perfectly distributed because the step size
    // used for sampling is calculated using integer division, which leads to cumulative rounding
    // errors when adding the (rounded-down) step size multiple times.
    std::vector<ChangeStreamPreImage> expectedPreImages;
    for (int i : {0, 5, 10, 15, 20, 25, 30, 35, 40, 49}) {
        expectedPreImages.push_back(buildPreImage(
            kNsUUID, Timestamp(Seconds(baseTime), 0), i /* applyOpsIndex */, BSON("x" << i)));
    }

    constexpr uint64_t kNumSamples = 10;
    assertEqualRangeSampleForNsUUIDContainsExactPreimages(kNumSamples, expectedPreImages);
}

/**
 * Tests for overriding server parameter 'changeStreamPreImagesSamplePointsPerUUID'.
 */
TEST_F(EqualStepSamplingStrategyTest, SampleSizeOneDefinedByServerParameter) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);

    insertDirectlyToPreImagesCollection(opCtx, kPreImage1);

    // Set the number of samples per nsUUID via server parameter, and sample with the "official"
    // process that selects the sampling strategy. This will select the "equal step" sampling
    // strategy.
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagUseReplicatedTruncatesForDeletions", true);
    RAIIServerParameterControllerForTest sampleSizeController{
        "changeStreamPreImagesSamplePointsPerUUID", 1};

    auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);

    PreImagesTruncateMarkers truncateMarkers(opCtx, preImagesCollection);
    const auto markersSnapshot = truncateMarkers.getMarkersMap_forTest().getUnderlyingSnapshot();

    ASSERT_TRUE(markersSnapshot->contains(kNsUUID));

    const auto& truncateMarkersForNsUUID = markersSnapshot->at(kNsUUID);
    const auto& markers = truncateMarkersForNsUUID->getMarkers_forTest();
    ASSERT_EQ(1, markers.size());
    ASSERT_EQ(32768, markers[0].records);
}

TEST_F(EqualStepSamplingStrategyTest, SampleSizeTwentyDefinedByServerParameter) {
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

    // Set the number of samples per nsUUID via server parameter, and sample with the "official"
    // process that selects the sampling strategy. This will select the "equal step" sampling
    // strategy.
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagUseReplicatedTruncatesForDeletions", true);
    RAIIServerParameterControllerForTest sampleSizeController{
        "changeStreamPreImagesSamplePointsPerUUID", 20};

    auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);

    PreImagesTruncateMarkers truncateMarkers(opCtx, preImagesCollection);
    const auto markersSnapshot = truncateMarkers.getMarkersMap_forTest().getUnderlyingSnapshot();

    ASSERT_TRUE(markersSnapshot->contains(kNsUUID));

    const auto& truncateMarkersForNsUUID = markersSnapshot->at(kNsUUID);
    const auto& markers = truncateMarkersForNsUUID->getMarkers_forTest();
    ASSERT_EQ(18, markers.size());
    ASSERT_EQ(32768, markers[0].records);
}

}  // namespace
}  // namespace mongo
