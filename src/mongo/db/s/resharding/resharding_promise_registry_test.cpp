// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/s/resharding/resharding_promise.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/future.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

struct TestState {
    int value = 0;
};

using TestRegistry = ReshardingPromiseRegistry<TestState>;

class ReshardingPromiseRegistryTest : public unittest::Test {};

TEST_F(ReshardingPromiseRegistryTest, RecoverFulfillsPromisesWhoseThresholdWasReached) {
    TestRegistry registry;
    ReshardingPromise<void> p1(registry, [&](WithLock lk, const TestState& state) {
        if (state.value >= 1)
            p1.emplaceValue(lk);
    });
    ReshardingPromise<void> p2(registry, [&](WithLock lk, const TestState& state) {
        if (state.value >= 2)
            p2.emplaceValue(lk);
    });
    auto f1 = p1.getFuture();
    auto f2 = p2.getFuture();

    registry.recover(WithLock::withoutLock(), TestState{1});

    ASSERT_OK(f1.getNoThrow());
    ASSERT_FALSE(f2.isReady());
}

TEST_F(ReshardingPromiseRegistryTest, SetErrorFulfillsAllRegisteredPromises) {
    TestRegistry registry;
    ReshardingPromise<void> p1(registry, [](WithLock, const TestState&) {});
    ReshardingPromise<void> p2(registry, [](WithLock, const TestState&) {});
    auto f1 = p1.getFuture();
    auto f2 = p2.getFuture();

    registry.setError(WithLock::withoutLock(), Status(ErrorCodes::InternalError, "boom"));

    ASSERT_TRUE(f1.isReady());
    ASSERT_TRUE(f2.isReady());
    ASSERT_EQ(f1.getNoThrow().code(), ErrorCodes::InternalError);
    ASSERT_EQ(f2.getNoThrow().code(), ErrorCodes::InternalError);
}

TEST_F(ReshardingPromiseRegistryTest, SetErrorSkipsAlreadyFulfilledPromises) {
    TestRegistry registry;
    ReshardingPromise<void> promise(registry, [](WithLock, const TestState&) {});
    auto future = promise.getFuture();

    promise.emplaceValue(WithLock::withoutLock());
    registry.setError(WithLock::withoutLock(), Status(ErrorCodes::InternalError, "too late"));

    ASSERT_OK(future.getNoThrow());
}

}  // namespace
}  // namespace mongo
