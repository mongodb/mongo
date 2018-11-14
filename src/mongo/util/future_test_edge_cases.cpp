
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/util/future.h"

#include "mongo/stdx/thread.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include "mongo/util/future_test_utils.h"

namespace mongo {
namespace {

// This is the motivating case for SharedStateBase::isJustForContinuation. Without that logic, there
// would be a long chain of SharedStates, growing longer with each recursion. That logic exists to
// limit it to a fixed-size chain.
TEST(Future_EdgeCases, looping_onError) {
    int tries = 10;
    std::function<Future<int>()> read = [&] {
        return async([&] {
                   uassert(ErrorCodes::BadValue, "", --tries == 0);
                   return tries;
               })
            .onError([&](Status) { return read(); });
    };
    ASSERT_EQ(read().get(), 0);
}

// This tests for a bug in an earlier implementation of isJustForContinuation. Due to an off-by-one,
// it would replace the "then" continuation's SharedState. A different type is used for the return
// from then to cause it to fail a checked_cast close to the bug in debug builds.
TEST(Future_EdgeCases, looping_onError_with_then) {
    int tries = 10;
    std::function<Future<int>()> read = [&] {
        return async([&] {
                   uassert(ErrorCodes::BadValue, "", --tries == 0);
                   return tries;
               })
            .onError([&](Status) { return read(); });
    };
    ASSERT_EQ(read().then([](int x) { return x + 0.5; }).get(), 0.5);
}

class DummyInterruptable final : public Interruptible {
    StatusWith<stdx::cv_status> waitForConditionOrInterruptNoAssertUntil(
        stdx::condition_variable& cv,
        stdx::unique_lock<stdx::mutex>& m,
        Date_t deadline) noexcept override {
        return Status(ErrorCodes::Interrupted, "");
    }
    Date_t getDeadline() const override {
        MONGO_UNREACHABLE;
    }
    Status checkForInterruptNoAssert() noexcept override {
        MONGO_UNREACHABLE;
    }
    IgnoreInterruptsState pushIgnoreInterrupts() override {
        MONGO_UNREACHABLE;
    }
    void popIgnoreInterrupts(IgnoreInterruptsState iis) override {
        MONGO_UNREACHABLE;
    }
    DeadlineState pushArtificialDeadline(Date_t deadline, ErrorCodes::Error error) override {
        MONGO_UNREACHABLE;
    }
    void popArtificialDeadline(DeadlineState) override {
        MONGO_UNREACHABLE;
    }
    Date_t getExpirationDateForWaitForValue(Milliseconds waitFor) override {
        MONGO_UNREACHABLE;
    }
};

TEST(Future_EdgeCases, interrupted_wait_then_get) {
    DummyInterruptable dummyInterruptable;

    auto pf = makePromiseFuture<void>();
    ASSERT_EQ(pf.future.waitNoThrow(&dummyInterruptable), ErrorCodes::Interrupted);
    ASSERT_EQ(pf.future.getNoThrow(&dummyInterruptable), ErrorCodes::Interrupted);

    pf.promise.emplaceValue();
    pf.future.get();
}

TEST(Future_EdgeCases, interrupted_wait_then_get_with_bgthread) {
    DummyInterruptable dummyInterruptable;

    // Note, this is intentionally somewhat racy. async() is defined to sleep 100ms before running
    // the function so it will generally test blocking in the final get(). Under TSAN the sleep is
    // removed to allow it to find more interesting interleavings, and give it a better chance at
    // detecting data races.
    auto future = async([] {});

    auto res = future.waitNoThrow(&dummyInterruptable);
    if (!res.isOK())
        ASSERT_EQ(res, ErrorCodes::Interrupted);

    res = future.getNoThrow(&dummyInterruptable);
    if (!res.isOK())
        ASSERT_EQ(res, ErrorCodes::Interrupted);

    future.get();
}

TEST(Future_EdgeCases, interrupted_wait_then_then) {
    DummyInterruptable dummyInterruptable;

    auto pf = makePromiseFuture<void>();
    ASSERT_EQ(pf.future.waitNoThrow(&dummyInterruptable), ErrorCodes::Interrupted);
    auto fut2 = std::move(pf.future).then([] {});

    pf.promise.emplaceValue();
    fut2.get();
}

TEST(Future_EdgeCases, interrupted_wait_then_then_with_bgthread) {
    DummyInterruptable dummyInterruptable;

    // Note, this is intentionally somewhat racy. async() is defined to sleep 100ms before running
    // the function so it will generally test blocking in the final get(). Under TSAN the sleep is
    // removed to allow it to find more interesting interleavings, and give it a better chance at
    // detecting data races.
    auto future = async([] {});

    auto res = future.waitNoThrow(&dummyInterruptable);
    if (!res.isOK())
        ASSERT_EQ(res, ErrorCodes::Interrupted);

    res = future.getNoThrow(&dummyInterruptable);
    if (!res.isOK())
        ASSERT_EQ(res, ErrorCodes::Interrupted);

    std::move(future).then([] {}).get();
}

TEST(Future_EdgeCases, Racing_SharePromise_getFuture_and_emplaceValue) {
    SharedPromise<void> sp;
    std::vector<Future<void>> futs;
    futs.reserve(30);

    // Note, this is intentionally somewhat racy. async() is defined to sleep 100ms before running
    // the function so the first batch of futures will generally block before getting the value is
    // emplaced, and the second batch will happen around the same time. In all cases the final batch
    // happen after the emplaceValue(), but roughly at the same time. Under TSAN the sleep is
    // removed to allow it to find more interesting interleavings, and give it a better chance at
    // detecting data races.

    for (int i = 0; i < 10; i++) {
        futs.push_back(async([&] { sp.getFuture().get(); }));
    }

    sleepUnlessInTsan();

    for (int i = 0; i < 10; i++) {
        futs.push_back(async([&] { sp.getFuture().get(); }));
    }

    sleepUnlessInTsan();

    sp.emplaceValue();

    for (int i = 0; i < 10; i++) {
        futs.push_back(async([&] { sp.getFuture().get(); }));
    }

    for (auto& fut : futs) {
        fut.get();
    }
}

TEST(Future_EdgeCases, Racing_SharePromise_getFuture_and_setError) {
    SharedPromise<void> sp;
    std::vector<Future<void>> futs;
    futs.reserve(30);

    // Note, this is intentionally somewhat racy. async() is defined to sleep 100ms before running
    // the function so the first batch of futures will generally block before getting the value is
    // emplaced, and the second batch will happen around the same time. In all cases the final batch
    // happen after the emplaceValue(), but roughly at the same time. Under TSAN the sleep is
    // removed to allow it to find more interesting interleavings, and give it a better chance at
    // detecting data races.

    for (int i = 0; i < 10; i++) {
        futs.push_back(async([&] { sp.getFuture().get(); }));
    }

    sleepUnlessInTsan();

    for (int i = 0; i < 10; i++) {
        futs.push_back(async([&] { sp.getFuture().get(); }));
    }

    sleepUnlessInTsan();

    sp.setError(failStatus());

    for (int i = 0; i < 10; i++) {
        futs.push_back(async([&] { sp.getFuture().get(); }));
    }

    for (auto& fut : futs) {
        ASSERT_EQ(fut.getNoThrow(), failStatus());
    }
}

// Make sure we actually die if someone throws from the getAsync callback.
//
// With gcc 5.8 we terminate, but print "terminate() called. No exception is active". This works in
// clang and gcc 7, so hopefully we can change the death-test search string to "die die die!!!" when
// we upgrade the toolchain.
DEATH_TEST(Future_EdgeCases, Success_getAsync_throw, "terminate() called") {
    Future<void>::makeReady().getAsync(
        [](Status) { uasserted(ErrorCodes::BadValue, "die die die!!!"); });
}

}  // namespace
}  // namespace mongo
