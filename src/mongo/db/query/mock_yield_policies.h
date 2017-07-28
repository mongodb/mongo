/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/db/query/plan_yield_policy.h"

namespace mongo {

/**
 * A custom yield policy that always reports the plan should yield, and always returns
 * ErrorCodes::ExceededTimeLimit from yield().
 */
class AlwaysTimeOutYieldPolicy : public PlanYieldPolicy {
public:
    AlwaysTimeOutYieldPolicy(PlanExecutor* exec)
        : PlanYieldPolicy(exec, PlanExecutor::YieldPolicy::ALWAYS_TIME_OUT) {}

    AlwaysTimeOutYieldPolicy(ClockSource* cs)
        : PlanYieldPolicy(PlanExecutor::YieldPolicy::ALWAYS_TIME_OUT, cs) {}

    bool shouldYield() override {
        return true;
    }

    Status yield(RecordFetcher* recordFetcher) override {
        return {ErrorCodes::ExceededTimeLimit, "Using AlwaysTimeOutYieldPolicy"};
    }

    Status yield(stdx::function<void()> beforeYieldingFn,
                 stdx::function<void()> whileYieldingFn) override {
        return {ErrorCodes::ExceededTimeLimit, "Using AlwaysTimeOutYieldPolicy"};
    }
};

/**
 * A custom yield policy that always reports the plan should yield, and always returns
 * ErrorCodes::QueryPlanKilled from yield().
 */
class AlwaysPlanKilledYieldPolicy : public PlanYieldPolicy {
public:
    AlwaysPlanKilledYieldPolicy(PlanExecutor* exec)
        : PlanYieldPolicy(exec, PlanExecutor::YieldPolicy::ALWAYS_MARK_KILLED) {}

    AlwaysPlanKilledYieldPolicy(ClockSource* cs)
        : PlanYieldPolicy(PlanExecutor::YieldPolicy::ALWAYS_MARK_KILLED, cs) {}

    bool shouldYield() override {
        return true;
    }

    Status yield(RecordFetcher* recordFetcher) override {
        return {ErrorCodes::QueryPlanKilled, "Using AlwaysPlanKilledYieldPolicy"};
    }

    Status yield(stdx::function<void()> beforeYieldingFn,
                 stdx::function<void()> whileYieldingFn) override {
        return {ErrorCodes::QueryPlanKilled, "Using AlwaysPlanKilledYieldPolicy"};
    }
};

}  // namespace mongo
