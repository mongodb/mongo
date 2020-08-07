/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/util/out_of_line_executor.h"

#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/executor_test_util.h"

namespace mongo {
namespace {

TEST(ExecutorTest, RejectingExecutor) {
    // Verify that the executor rejects every time and keeps an accurate count.
    const auto exec = RejectingExecutor::make();

    static constexpr size_t kCount = 1000;
    for (size_t i = 0; i < kCount; ++i) {
        exec->schedule([&](Status error) {
            ASSERT_NOT_OK(error);
            ASSERT_EQ(exec->tasksRejected.load(), (i + 1));
        });
    }
}

TEST(ExecutorTest, InlineCountingExecutor) {
    // Verify that the executor accepts every time and keeps an accurate count.
    const auto execA = InlineCountingExecutor::make();
    const auto execB = InlineCountingExecutor::make();

    // Using prime numbers so there is no chance of multiple traps
    static constexpr size_t kCountA = 1013;
    static constexpr size_t kCountB = 1511;

    // Schedule kCountA tasks one at a time.
    for (size_t i = 0; i < kCountA; ++i) {
        execA->schedule([&](Status status) {
            ASSERT_OK(status);
            ASSERT_EQ(execA->tasksRun.load(), (i + 1));
        });
    }

    {
        // Schedule kCountB tasks recursively.
        size_t i = 0;
        std::function<void(Status)> recurseExec;
        bool inTask = false;

        recurseExec = [&](Status status) {
            ASSERT(!std::exchange(inTask, true));
            ASSERT_OK(status);

            auto tasksRun = execB->tasksRun.load();
            ASSERT_EQ(tasksRun, ++i);
            if (tasksRun < kCountB) {
                execB->schedule(recurseExec);
            }

            ASSERT(std::exchange(inTask, false));
        };

        execB->schedule(recurseExec);
    }

    // Verify that running executors together didn't change the expected counts.
    ASSERT_EQ(execA->tasksRun.load(), kCountA);
    ASSERT_EQ(execB->tasksRun.load(), kCountB);
}

DEATH_TEST(ExecutorTest,
           GuaranteedExecutor_MainInvalid_FallbackInvalid,
           GuaranteedExecutor::kNoExecutorStr) {
    // If no executor was provided, then we invariant.
    const auto gwarExec = makeGuaranteedExecutor({});
}

DEATH_TEST(ExecutorTest,
           GuaranteedExecutor_MainInvalid_FallbackRejects,
           GuaranteedExecutor::kRejectedWorkStr) {
    // If we have a fallback and it rejects work, then we invariant.
    const auto gwarExec = makeGuaranteedExecutor({}, RejectingExecutor::make());
    gwarExec->schedule([](Status) { FAIL("Nothing should run the actual callback"); });
}

TEST(ExecutorTest, GuaranteedExecutor_MainInvalid_FallbackAccepts) {
    // If we only have a fallback, then everything runs on it.
    const auto countExec = InlineCountingExecutor::make();
    const auto gwarExec = makeGuaranteedExecutor({}, countExec);

    static constexpr size_t kCount = 1000;
    for (size_t i = 0; i < kCount; ++i) {
        gwarExec->schedule([&](Status status) { ASSERT_OK(status); });
    }

    ASSERT_EQ(countExec->tasksRun.load(), kCount);
}

DEATH_TEST(ExecutorTest,
           GuaranteedExecutor_MainRejects_FallbackInvalid,
           GuaranteedExecutor::kRejectedWorkStr) {
    // If we only have a main executor and it rejects work, then we invariant.
    const auto gwarExec = makeGuaranteedExecutor(RejectingExecutor::make());
    gwarExec->schedule([](Status) { FAIL("Nothing should run the actual callback"); });
}

DEATH_TEST(ExecutorTest,
           GuaranteedExecutor_MainRejects_FallbackRejects,
           GuaranteedExecutor::kRejectedWorkStr) {
    // If we have a main and a fallback and both reject work, then we invariant.
    const auto gwarExec =
        makeGuaranteedExecutor(RejectingExecutor::make(), RejectingExecutor::make());
    gwarExec->schedule([](Status) { FAIL("Nothing should run the actual callback"); });
}

TEST(ExecutorTest, GuaranteedExecutor_MainRejects_FallbackAccepts) {
    // If the main rejects and the fallback accepts, then run on the fallback.
    const auto rejectExec = RejectingExecutor::make();
    const auto countExec = InlineCountingExecutor::make();
    const auto gwarExec = makeGuaranteedExecutor(rejectExec, countExec);

    static constexpr size_t kCount = 1000;
    for (size_t i = 0; i < kCount; ++i) {
        gwarExec->schedule([&](Status status) { ASSERT_OK(status); });
    }
    ASSERT_EQ(rejectExec->tasksRejected.load(), kCount);
    ASSERT_EQ(countExec->tasksRun.load(), kCount);
}

TEST(ExecutorTest, GuaranteedExecutor_MainAccepts_FallbackInvalid) {
    // If the main accepts and we don't have a fallback, then run on the main.
    const auto countExec = InlineCountingExecutor::make();
    const auto gwarExec = makeGuaranteedExecutor(countExec, {});

    static constexpr size_t kCount = 1000;
    for (size_t i = 0; i < kCount; ++i) {
        gwarExec->schedule([&](Status status) { ASSERT_OK(status); });
    }

    ASSERT_EQ(countExec->tasksRun.load(), kCount);
}

TEST(ExecutorTest, GuaranteedExecutor_MainAccepts_FallbackRejects) {
    // If the main accepts and the fallback would reject, then run on the main.
    const auto countExec = InlineCountingExecutor::make();
    const auto rejectExec = RejectingExecutor::make();
    const auto gwarExec = makeGuaranteedExecutor(countExec, rejectExec);

    static constexpr size_t kCount = 1000;
    for (size_t i = 0; i < kCount; ++i) {
        gwarExec->schedule([&](Status status) { ASSERT_OK(status); });
    }

    ASSERT_EQ(countExec->tasksRun.load(), kCount);
    ASSERT_EQ(rejectExec->tasksRejected.load(), 0);
}

TEST(ExecutorTest, GuaranteedExecutor_MainAccepts_FallbackAccepts) {
    // If both executor accepts, then run on the main.
    const auto countExecA = InlineCountingExecutor::make();
    const auto countExecB = InlineCountingExecutor::make();
    const auto gwarExec = makeGuaranteedExecutor(countExecA, countExecB);

    static constexpr size_t kCount = 1000;
    for (size_t i = 0; i < kCount; ++i) {
        gwarExec->schedule([&](Status status) { ASSERT_OK(status); });
    }

    ASSERT_EQ(countExecA->tasksRun.load(), kCount);
    ASSERT_EQ(countExecB->tasksRun.load(), 0);
}

}  // namespace
}  // namespace mongo
