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

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/db/query/plan_executor.h"

namespace mongo {

/**
 * A base class from which all mocked yield policy implementations used for testing should derive.
 */
class MockYieldPolicy : public PlanYieldPolicy {
public:
    MockYieldPolicy(OperationContext* opCtx,
                    ClockSource* clockSource,
                    PlanYieldPolicy::YieldPolicy policy)
        : PlanYieldPolicy(opCtx, policy, clockSource, 0, Milliseconds{0}, nullptr, nullptr) {}

private:
    void saveState(OperationContext* opCtx) override final {
        MONGO_UNREACHABLE;
    }

    void restoreState(OperationContext* opCtx, const Yieldable* yieldable) override final {
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

    bool shouldYieldOrInterrupt(OperationContext*) override {
        return true;
    }

    Status yieldOrInterrupt(OperationContext*, std::function<void()> whileYieldingFn) override {
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

    bool shouldYieldOrInterrupt(OperationContext*) override {
        return true;
    }

    Status yieldOrInterrupt(OperationContext*, std::function<void()> whileYieldingFn) override {
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
        : MockYieldPolicy(opCtx, clockSource, PlanYieldPolicy::YieldPolicy::NO_YIELD) {}

    bool shouldYieldOrInterrupt(OperationContext*) override {
        return false;
    }

    Status yieldOrInterrupt(OperationContext*, std::function<void()> whileYieldingFn) override {
        MONGO_UNREACHABLE;
    }
};

}  // namespace mongo
