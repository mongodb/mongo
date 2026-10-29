// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/change_stream_pre_images_truncate_markers_sampling_strategy/sampling_strategy.h"

#include "mongo/db/change_stream_pre_image_test_helpers.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {
using namespace change_stream_pre_image_test_helper;
using namespace pre_image_marker_initialization_internal;

/**
 * A stub SamplingStrategy that returns a configurable result and tracks whether it was called.
 */
class StubSamplingStrategy : public SamplingStrategy {
public:
    explicit StubSamplingStrategy(bool returnValue) : _returnValue(returnValue) {}

    [[nodiscard]] bool performSampling(OperationContext* opCtx,
                                       const CollectionAcquisition& preImagesCollection,
                                       MarkersMap& markersMap) override {
        _called = true;
        return _returnValue;
    }

    bool wasCalled() const {
        return _called;
    }

private:
    const bool _returnValue;
    bool _called = false;
};

class PrimaryWithFallbackSamplingStrategyTest : public CatalogTestFixture,
                                                public ChangeStreamPreImageTestConstants {};

TEST_F(PrimaryWithFallbackSamplingStrategyTest, PrimarySucceeds_FallbackNotCalled) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);
    auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);

    auto primary = std::make_unique<StubSamplingStrategy>(true);
    auto fallback = std::make_unique<StubSamplingStrategy>(true);
    auto* primaryPtr = primary.get();
    auto* fallbackPtr = fallback.get();

    PrimaryWithFallbackSamplingStrategy strategy(std::move(primary), std::move(fallback));

    MarkersMap markersMap;
    ASSERT_TRUE(strategy.performSampling(opCtx, preImagesCollection, markersMap));
    ASSERT_TRUE(primaryPtr->wasCalled());
    ASSERT_FALSE(fallbackPtr->wasCalled());
}

TEST_F(PrimaryWithFallbackSamplingStrategyTest, PrimaryFails_FallbackSucceeds) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);
    auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);

    auto primary = std::make_unique<StubSamplingStrategy>(false);
    auto fallback = std::make_unique<StubSamplingStrategy>(true);
    auto* primaryPtr = primary.get();
    auto* fallbackPtr = fallback.get();

    PrimaryWithFallbackSamplingStrategy strategy(std::move(primary), std::move(fallback));

    MarkersMap markersMap;
    ASSERT_TRUE(strategy.performSampling(opCtx, preImagesCollection, markersMap));
    ASSERT_TRUE(primaryPtr->wasCalled());
    ASSERT_TRUE(fallbackPtr->wasCalled());
}

TEST_F(PrimaryWithFallbackSamplingStrategyTest, BothFail_ReturnsFalse) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);
    auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);

    auto primary = std::make_unique<StubSamplingStrategy>(false);
    auto fallback = std::make_unique<StubSamplingStrategy>(false);
    auto* primaryPtr = primary.get();
    auto* fallbackPtr = fallback.get();

    PrimaryWithFallbackSamplingStrategy strategy(std::move(primary), std::move(fallback));

    MarkersMap markersMap;
    ASSERT_FALSE(strategy.performSampling(opCtx, preImagesCollection, markersMap));
    ASSERT_TRUE(primaryPtr->wasCalled());
    ASSERT_TRUE(fallbackPtr->wasCalled());
}

}  // namespace
}  // namespace mongo
