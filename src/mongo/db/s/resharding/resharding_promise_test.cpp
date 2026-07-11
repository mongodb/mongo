// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/resharding/resharding_promise.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/future.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

struct TestState {
    int value = 0;
};

using TestRegistry = ReshardingPromiseRegistry<TestState>;

class ReshardingPromiseTest : public unittest::Test {};

TEST_F(ReshardingPromiseTest, VoidPromiseSetErrorAfterEmplaceIsIdempotent) {
    TestRegistry registry;
    ReshardingPromise<void> promise(registry, [](WithLock, const TestState&) {});
    auto future = promise.getFuture();

    promise.emplaceValue(WithLock::withoutLock());
    promise.setError(WithLock::withoutLock(), Status(ErrorCodes::InternalError, "too late"));

    ASSERT_OK(future.getNoThrow());
}

TEST_F(ReshardingPromiseTest, VoidPromiseEmplaceAfterSetErrorIsIdempotent) {
    TestRegistry registry;
    ReshardingPromise<void> promise(registry, [](WithLock, const TestState&) {});
    auto future = promise.getFuture();

    promise.setError(WithLock::withoutLock(), Status(ErrorCodes::InternalError, "error"));
    promise.emplaceValue(WithLock::withoutLock());

    ASSERT_EQ(future.getNoThrow().code(), ErrorCodes::InternalError);
}

TEST_F(ReshardingPromiseTest, VoidPromiseDoubleSetErrorKeepsFirstError) {
    TestRegistry registry;
    ReshardingPromise<void> promise(registry, [](WithLock, const TestState&) {});
    auto future = promise.getFuture();

    promise.setError(WithLock::withoutLock(), Status(ErrorCodes::InternalError, "first"));
    promise.setError(WithLock::withoutLock(), Status(ErrorCodes::BadValue, "second"));

    ASSERT_EQ(future.getNoThrow().code(), ErrorCodes::InternalError);
}

TEST_F(ReshardingPromiseTest, TypedPromiseDoubleEmplaceKeepsFirstValue) {
    TestRegistry registry;
    ReshardingPromise<int> promise(registry, [](WithLock, const TestState&) {});
    auto future = promise.getFuture();

    promise.emplaceValue(WithLock::withoutLock(), 42);
    promise.emplaceValue(WithLock::withoutLock(), 99);

    ASSERT_EQ(future.getNoThrow().getValue(), 42);
}

}  // namespace
}  // namespace mongo
