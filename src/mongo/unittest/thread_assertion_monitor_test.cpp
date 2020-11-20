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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/unittest/thread_assertion_monitor.h"

#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"

namespace mongo::unittest {
namespace {

TEST(ThreadAssertionMonitor, Trivial) {
    ThreadAssertionMonitor monitor;
    monitor.notifyDone();  // Somebody always has to call notifyDone or dtor will wait forever.
}

TEST(ThreadAssertionMonitor, ControllerInStdxThread) {
    ThreadAssertionMonitor monitor;
    stdx::thread{[&] { monitor.notifyDone(); }}.join();
}

TEST(ThreadAssertionMonitor, OnlyControllerInSpawn) {
    ThreadAssertionMonitor monitor;
    monitor.spawn([&] { monitor.notifyDone(); }).join();
}

TEST(ThreadAssertionMonitor, OnlyControllerInSpawnController) {
    ThreadAssertionMonitor monitor;
    monitor.spawnController([] {}).join();
}

TEST(ThreadAssertionMonitor, WorkerExecOk) {
    ThreadAssertionMonitor monitor;
    monitor.spawnController([&] { monitor.spawn([&] { ASSERT_EQ(1, 1) << "Worker ok"; }).join(); })
        .join();
}

TEST(ThreadAssertionMonitor, WorkerExecFail) {
    ThreadAssertionMonitor monitor;
    monitor.spawnController([&] { monitor.spawn([&] { ASSERT_EQ(1, 2) << "Oops"; }).join(); })
        .join();
    try {
        monitor.wait();
        FAIL("Expected monitor.wait() to throw");
    } catch (const TestAssertionFailureException& ex) {
        ASSERT_STRING_SEARCH_REGEX(ex.what(), "Oops");
    }
    LOGV2_INFO(5182100, "monitor.wait finished");
}

TEST(ThreadAssertionMonitor, ThreadAssertionMonitoredTestTrivial) {
    threadAssertionMonitoredTest([](auto&) {});
}

TEST(ThreadAssertionMonitor, ThreadAssertionMonitoredTestPassing) {
    threadAssertionMonitoredTest([](auto& monitor) { monitor.spawn([&] {}).join(); });
}

TEST(ThreadAssertionMonitor, ThreadAssertionMonitoredTestFailing) {
    try {
        threadAssertionMonitoredTest(
            [](auto& monitor) { monitor.spawn([&] { ASSERT_EQ(1, 2) << "Oops"; }).join(); });
        FAIL("Expected threadAssertionMonitoredTest to throw");
    } catch (const TestAssertionFailureException& ex) {
        ASSERT_STRING_SEARCH_REGEX(ex.what(), "Oops");
    }
}


}  // namespace
}  // namespace mongo::unittest
