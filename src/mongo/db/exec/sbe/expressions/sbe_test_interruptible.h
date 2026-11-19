// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/interruptible.h"

namespace mongo::sbe {

// Counts VM interrupt polls and optionally interrupts on a specified poll.
class CountingInterruptible final : public Interruptible {
public:
    explicit CountingInterruptible(int interruptOnCheck = 0)
        : _interruptOnCheck(interruptOnCheck) {}

    int checks() const {
        return _checks;
    }

private:
    Status checkForInterruptNoAssert() noexcept override {
        ++_checks;
        return _checks == _interruptOnCheck ? Status(ErrorCodes::Interrupted, "test interrupt")
                                            : Status::OK();
    }

    StatusWith<stdx::cv_status> waitForConditionOrInterruptNoAssertUntil(stdx::condition_variable&,
                                                                         BasicLockableAdapter,
                                                                         Date_t) noexcept override {
        MONGO_UNREACHABLE;
    }

    Date_t getDeadline() const override {
        return Date_t::max();
    }

    Status checkForDeadlineExpiredNoAssert(Date_t) noexcept override {
        return Status::OK();
    }

    DeadlineState pushArtificialDeadline(Date_t, ErrorCodes::Error) override {
        MONGO_UNREACHABLE;
    }

    void popArtificialDeadline(DeadlineState) override {
        MONGO_UNREACHABLE;
    }

    Date_t getExpirationDateForWaitForValue(Milliseconds waitFor) override {
        return Date_t::now() + waitFor;
    }

    int _checks = 0;
    int _interruptOnCheck;
};

}  // namespace mongo::sbe
