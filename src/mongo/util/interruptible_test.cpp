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
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

class DummyInterruptible final : public Interruptible {
    Date_t getDeadline() const override {
        MONGO_UNREACHABLE;
    }
    Status checkForInterruptNoAssert() noexcept override {
        // Must be implemented because it's called by Interruptible::waitForConditionOrInterrupt.
        return Status::OK();
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
        return Date_t::now() + waitFor;
    }
    StatusWith<stdx::cv_status> waitForConditionOrInterruptNoAssertUntil(
        stdx::condition_variable& cv, BasicLockableAdapter m, Date_t deadline) noexcept override {
        mongo::sleepFor(deadline - Date_t::now());
        return stdx::cv_status::timeout;
    }

public:
    // To accommodate for OS scheduling and timing inaccuracies.
    static constexpr auto kPrecision = Milliseconds(5);
};

TEST(Interruptible, WaitUntilDeadline) {
    auto interruptible = std::make_unique<DummyInterruptible>();
    const auto sleepFor = Milliseconds(500);

    auto mutex = MONGO_MAKE_LATCH();
    stdx::condition_variable cv;
    stdx::unique_lock<Latch> lk(mutex);

    const auto start = Date_t::now();
    Date_t deadline = start + sleepFor;
    interruptible->waitForConditionOrInterruptUntil(cv, lk, deadline, []() { return false; });
    const auto end = Date_t::now();

    ASSERT_GTE(end, deadline - DummyInterruptible::kPrecision);
    ASSERT_LTE(end, deadline + DummyInterruptible::kPrecision);
    ASSERT_FALSE(interruptible->isWaitingForConditionOrInterrupt());
}

}  // namespace
}  // namespace mongo
