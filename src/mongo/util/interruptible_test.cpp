// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/interruptible.h"

#include "mongo/stdx/condition_variable.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

#include <mutex>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

class InterruptibleTest : public unittest::Test {};

TEST_F(InterruptibleTest, NotInterruptibleWaitForConditionFailsWithOverflowError) {
    auto notInterruptible = Interruptible::notInterruptible();

    std::mutex mutex;
    stdx::condition_variable cv;
    std::unique_lock<std::mutex> lk(mutex);

    auto overflowDeadline = Date_t::max() - Milliseconds(1);

    ASSERT_THROWS_CODE(notInterruptible->waitForConditionOrInterruptUntil(
                           cv, lk, overflowDeadline, [] { return false; }),
                       DBException,
                       ErrorCodes::DurationOverflow);
}

}  // namespace

}  // namespace mongo
