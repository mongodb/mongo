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
