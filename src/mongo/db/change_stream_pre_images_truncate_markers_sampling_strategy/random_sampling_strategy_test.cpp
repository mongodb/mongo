// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/change_stream_pre_images_truncate_markers_sampling_strategy/random_sampling_strategy.h"

#include "mongo/db/change_stream_pre_image_test_helpers.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
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
class RandomSamplingStrategyTest : public CatalogTestFixture,
                                   public ChangeStreamPreImageTestConstants {};

TEST_F(RandomSamplingStrategyTest, PopulateMapWithEmptyCollection) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);

    {
        // Populate by sampling an empty collection where the initial 'totalRecords' and
        // 'totalBytes' estimates are accurate.
        MarkersMap mapBySamples;
        auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);

        RandomSamplingStrategy samplingStrategy(CollectionTruncateMarkers::kRandomSamplesPerMarker,
                                                gPreImagesCollectionTruncateMarkersMinBytes);
        ASSERT_FALSE(samplingStrategy.performSampling(opCtx, preImagesCollection, mapBySamples));
        const auto mapSnapshot = mapBySamples.getUnderlyingSnapshot();
        ASSERT_EQ(0, mapSnapshot->size());
    }

    {
        // Populate by sampling an empty collection where the initial 'totalRecords' and
        // 'totalBytes' estimates aren't accurate. The size tracked in-memory can be inaccurate
        // after unclean shutdowns.
        MarkersMap mapBySamples;
        auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);
        RandomSamplingStrategy samplingStrategy(CollectionTruncateMarkers::kRandomSamplesPerMarker,
                                                gPreImagesCollectionTruncateMarkersMinBytes);
        ASSERT_FALSE(samplingStrategy.performSampling(opCtx, preImagesCollection, mapBySamples));
        const auto mapSnapshot = mapBySamples.getUnderlyingSnapshot();
        ASSERT_EQ(0, mapSnapshot->size());
    }
}

/**
 * Tests for 'collectPreImageSamples'.
 */
TEST_F(RandomSamplingStrategyTest, PreImageSamplesFromEmptyCollection) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);

    const auto preImagesRAII = acquirePreImagesCollectionForRead(opCtx);

    std::vector<uint64_t> targetSampleOptions{0, 1, 2, 100};
    for (const auto& targetNumSamples : targetSampleOptions) {
        LOGV2(9528901,
              "Running test case to retrieve pre-image samples on an empty collection",
              "targetNumSamples"_attr = targetNumSamples);
        RandomSamplingStrategy samplingStrategy(targetNumSamples, 1);
        const auto samples =
            samplingStrategy.collectPreImageSamples(opCtx, preImagesRAII, targetNumSamples);
        ASSERT_EQ(0, samples.size());
    }
}

// Tests there is at least 1 sample per nsUUID, even if less samples are requested.
TEST_F(RandomSamplingStrategyTest, PreImageSamplesAlwaysContainLastRecordPerNsUUID) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);

    RandomSamplingStrategy samplingStrategy(1, 1);

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
        const auto samples = samplingStrategy.collectPreImageSamples(opCtx, preImagesRAII, 0);
        assert1SampleForNsUUID(kNsUUID, kPreImage1, samples);
    }

    {
        // Test Case: 2 pre-images for 'kNsUUID' in the pre-images collection with 0 samples
        // requested yields 1 sample.
        insertDirectlyToPreImagesCollection(opCtx, kPreImage2);
        auto preImagesRAII = acquirePreImagesCollectionForRead(opCtx);
        const auto samples = samplingStrategy.collectPreImageSamples(opCtx, preImagesRAII, 0);
        assert1SampleForNsUUID(kNsUUID, kPreImage2, samples);
    }

    {
        // Test Case: 2 pre-images for 'kNsUUID', 1 for 'kNsOtherUUID' in the pre-images collection
        // with 0 samples requested yields 1 sample per nsUUID.
        insertDirectlyToPreImagesCollection(opCtx, kPreImageOther);
        auto preImagesRAII = acquirePreImagesCollectionForRead(opCtx);
        const auto samples = samplingStrategy.collectPreImageSamples(opCtx, preImagesRAII, 0);
        assert1SampleForNsUUID(kNsUUID, kPreImage2, samples);
        assert1SampleForNsUUID(kNsUUIDOther, kPreImageOther, samples);
    }
}

// Demonstrates that pre-image sampling can result in repeated results of the same pre-image on a
// non-empty collection.
TEST_F(RandomSamplingStrategyTest, PreImageSamplesRepeatSamples) {
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
        RandomSamplingStrategy samplingStrategy(targetNumSamples, 1);
        const auto samples =
            samplingStrategy.collectPreImageSamples(opCtx, preImagesRAII, targetNumSamples);
        ASSERT_EQ(targetNumSamples, countTotalSamples(samples));
    }
}

}  // namespace
}  // namespace mongo
