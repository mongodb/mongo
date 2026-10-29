// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/unittest/thread_assertion_monitor.h"

#include "mongo/logv2/log.h"
#include "mongo/unittest/framework.h"

#include <memory>
#include <ostream>
#include <string>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo::unittest {
namespace {

struct FakeAssertionFailedExceptionForTest : std::exception {
    explicit FakeAssertionFailedExceptionForTest(std::string what) : _what{std::move(what)} {}
    const char* what() const noexcept override {
        return _what.c_str();
    }
    std::string _what;
};

TEST(ThreadAssertionMonitor, Trivial) {
    ThreadAssertionMonitor monitor;
    monitor.notifyDone();  // Somebody always has to call notifyDone or dtor will wait forever.
}

TEST(ThreadAssertionMonitor, ControllerInStdxThread) {
    ThreadAssertionMonitor monitor;
    stdx::thread{[&] {
        monitor.notifyDone();
    }}.join();
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
    monitor
        .spawnController([&] {
            monitor.spawn([&] { throw FakeAssertionFailedExceptionForTest{"Oops"}; }).join();
        })
        .join();
    try {
        monitor.wait();
        FAIL("Expected monitor.wait() to throw");
    } catch (const FakeAssertionFailedExceptionForTest& ex) {
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
        threadAssertionMonitoredTest([](auto& monitor) {
            monitor.spawn([&] { throw FakeAssertionFailedExceptionForTest{"Oops"}; }).join();
        });
        FAIL("Expected threadAssertionMonitoredTest to throw");
    } catch (const FakeAssertionFailedExceptionForTest& ex) {
        ASSERT_STRING_SEARCH_REGEX(ex.what(), "Oops");
    }
}


}  // namespace
}  // namespace mongo::unittest
