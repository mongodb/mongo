// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/rpc_local_timing.h"

#include "mongo/unittest/unittest.h"
#include "mongo/util/tick_source_mock.h"

namespace mongo {
namespace {

using testing::Eq;
using testing::Ge;
using testing::Optional;

class RpcLocalTimingTest : public unittest::Test {
protected:
    TickSourceMock<Microseconds> clock;
    RpcLocalTiming timing{&clock};
};

TEST_F(RpcLocalTimingTest, ExcludesResponseWait) {
    clock.advance(Microseconds{3000});
    timing.beginResponseWait();
    clock.advance(Microseconds{10'000'000});
    timing.endResponseWait();
    clock.advance(Microseconds{2000});
    EXPECT_THAT(timing.finish(), Optional(Microseconds{5000}));
}

TEST_F(RpcLocalTimingTest, FinishIsIdempotent) {
    clock.advance(Microseconds{25});
    EXPECT_THAT(timing.finish(), Optional(Microseconds{25}));
    EXPECT_THAT(timing.finish(), Eq(std::nullopt));
}

TEST_F(RpcLocalTimingTest, RepeatedBeginResponseWaitIsNoOp) {
    timing.beginResponseWait();
    clock.advance(Microseconds{10});
    timing.beginResponseWait();
    clock.advance(Microseconds{10});
    timing.endResponseWait();
    clock.advance(Microseconds{100});
    EXPECT_THAT(timing.finish(), Optional(Microseconds{100}));
}

TEST_F(RpcLocalTimingTest, EndResponseWaitWithoutBeginIsNoOp) {
    clock.advance(Microseconds{50});
    timing.endResponseWait();
    clock.advance(Microseconds{100});
    EXPECT_THAT(timing.finish(), Optional(Microseconds{150}));
}

TEST_F(RpcLocalTimingTest, WaitsAfterFinishAreNoOps) {
    clock.advance(Microseconds{10});
    EXPECT_THAT(timing.finish(), Optional(Microseconds{10}));
    timing.beginResponseWait();
    timing.endResponseWait();
    EXPECT_THAT(timing.finish(), Eq(std::nullopt));
}

TEST_F(RpcLocalTimingTest, DefaultConstructorUsesSystemTickSource) {
    RpcLocalTiming systemTiming;
    EXPECT_THAT(systemTiming.finish(), Optional(Ge(Microseconds{0})));
}

}  // namespace
}  // namespace mongo
