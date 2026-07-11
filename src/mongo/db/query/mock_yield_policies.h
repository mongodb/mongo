// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * A base class from which all mocked yield policy implementations used for testing should derive.
 */
class MockYieldPolicy : public PlanYieldPolicy {
public:
    MockYieldPolicy(OperationContext* opCtx,
                    ClockSource* clockSource,
                    PlanYieldPolicy::YieldPolicy policy)
        : PlanYieldPolicy(opCtx, policy, clockSource, 0, Milliseconds{0}, nullptr) {}

private:
    void saveState(OperationContext* opCtx) final {
        MONGO_UNREACHABLE;
    }

    void restoreState(OperationContext* opCtx,
                      const Yieldable* yieldable,
                      RestoreContext::RestoreType restoreType) final {
        MONGO_UNREACHABLE;
    }
};

/**
 * A custom yield policy that always reports the plan should yield, and always returns
 * ErrorCodes::ExceededTimeLimit from yield().
 */
class AlwaysTimeOutYieldPolicy final : public MockYieldPolicy {
public:
    AlwaysTimeOutYieldPolicy(OperationContext* opCtx, ClockSource* cs)
        : MockYieldPolicy(opCtx, cs, PlanYieldPolicy::YieldPolicy::ALWAYS_TIME_OUT) {}

    bool doShouldYieldOrInterrupt(OperationContext*) override {
        return true;
    }

    Status yieldOrInterrupt(
        OperationContext*,
        const std::function<void()>& whileYieldingFn,
        RestoreContext::RestoreType restoreType,
        const std::function<void()>& afterSnapshotAbandonFn = nullptr) override {

        return {ErrorCodes::ExceededTimeLimit, "Using AlwaysTimeOutYieldPolicy"};
    }
};

/**
 * A custom yield policy that always reports the plan should yield, and always returns
 * ErrorCodes::QueryPlanKilled from yield().
 */
class AlwaysPlanKilledYieldPolicy final : public MockYieldPolicy {
public:
    AlwaysPlanKilledYieldPolicy(OperationContext* opCtx, ClockSource* cs)
        : MockYieldPolicy(opCtx, cs, PlanYieldPolicy::YieldPolicy::ALWAYS_MARK_KILLED) {}

    bool doShouldYieldOrInterrupt(OperationContext*) override {
        return true;
    }

    Status yieldOrInterrupt(
        OperationContext*,
        const std::function<void()>& whileYieldingFn,
        RestoreContext::RestoreType restoreType,
        const std::function<void()>& afterSnapshotAbandonFn = nullptr) override {
        return {ErrorCodes::QueryPlanKilled, "Using AlwaysPlanKilledYieldPolicy"};
    }
};

/**
 * A yield policy for testing which never reports that the plan should yield, as
 * 'shouldYieldOrInterrupt()' always returns false.
 */
class NoopYieldPolicy final : public MockYieldPolicy {
public:
    NoopYieldPolicy(OperationContext* opCtx, ClockSource* clockSource)
        : MockYieldPolicy(opCtx, clockSource, PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY) {}

    bool doShouldYieldOrInterrupt(OperationContext*) override {
        return false;
    }

    Status yieldOrInterrupt(
        OperationContext*,
        const std::function<void()>& whileYieldingFn,
        RestoreContext::RestoreType restoreType,
        const std::function<void()>& afterSnapshotAbandonFn = nullptr) override {
        MONGO_UNREACHABLE;
    }
};

}  // namespace mongo
