// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/rollback_checker.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/repl/optime.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor_test_fixture.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <mutex>

#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace {

using namespace mongo;
using namespace mongo::repl;

using LockGuard = std::lock_guard<std::mutex>;

class RollbackCheckerTest : public executor::ThreadPoolExecutorTest {
public:
    RollbackChecker* getRollbackChecker() const;

protected:
    void setUp() override;

    std::unique_ptr<RollbackChecker> _rollbackChecker;
    RollbackChecker::Result _hasRolledBackResult = {ErrorCodes::NotYetInitialized, ""};
    bool _hasCalledCallback;
    mutable std::mutex _mutex;
};

void RollbackCheckerTest::setUp() {
    executor::ThreadPoolExecutorTest::setUp();
    launchExecutorThread();
    _rollbackChecker = std::make_unique<RollbackChecker>(&getExecutor(), HostAndPort());
    std::lock_guard<std::mutex> lk(_mutex);
    _hasRolledBackResult = {ErrorCodes::NotYetInitialized, ""};
    _hasCalledCallback = false;
}

RollbackChecker* RollbackCheckerTest::getRollbackChecker() const {
    return _rollbackChecker.get();
}

TEST_F(RollbackCheckerTest, InvalidConstruction) {
    HostAndPort syncSource;

    // Null executor.
    ASSERT_THROWS_CODE(
        RollbackChecker(nullptr, syncSource), AssertionException, ErrorCodes::BadValue);
}

TEST_F(RollbackCheckerTest, ShutdownBeforeStart) {
    auto callback = [](const RollbackChecker::Result&) {
    };
    shutdownExecutorThread();
    joinExecutorThread();
    ASSERT_NOT_OK(getRollbackChecker()->reset(callback).getStatus());
    ASSERT_NOT_OK(getRollbackChecker()->checkForRollback(callback).getStatus());
}

TEST_F(RollbackCheckerTest, ShutdownBeforeHasHadRollback) {
    shutdownExecutorThread();
    joinExecutorThread();
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, getRollbackChecker()->hasHadRollback());
}

TEST_F(RollbackCheckerTest, ShutdownBeforeResetSync) {
    shutdownExecutorThread();
    joinExecutorThread();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, getRollbackChecker()->reset_sync());
}

TEST_F(RollbackCheckerTest, reset) {
    auto callback = [](const RollbackChecker::Result&) {
    };
    auto cbh = unittest::assertGet(getRollbackChecker()->reset(callback));
    ASSERT(cbh);

    auto commandResponse = BSON("ok" << 1 << "rbid" << 3);

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        getNet()->scheduleSuccessfulResponse(commandResponse);
        getNet()->runReadyNetworkOperations();
    }

    getExecutor().wait(cbh);
    ASSERT_EQUALS(getRollbackChecker()->getBaseRBID(), 3);
}

TEST_F(RollbackCheckerTest, RollbackRBID) {
    auto callback = [this](const RollbackChecker::Result& result) {
        LockGuard lk(_mutex);
        _hasRolledBackResult = result;
        _hasCalledCallback = true;
    };
    // First set the RBID to 3.
    auto refreshCBH = unittest::assertGet(getRollbackChecker()->reset(callback));
    ASSERT(refreshCBH);
    auto commandResponse = BSON("ok" << 1 << "rbid" << 3);
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        getNet()->scheduleSuccessfulResponse(commandResponse);
        getNet()->runReadyNetworkOperations();
    }
    getExecutor().wait(refreshCBH);
    ASSERT_EQUALS(getRollbackChecker()->getBaseRBID(), 3);
    {
        LockGuard lk(_mutex);
        ASSERT_TRUE(_hasCalledCallback);
        ASSERT_TRUE(unittest::assertGet(_hasRolledBackResult));
        _hasCalledCallback = false;
    }

    // Check for rollback
    auto rbCBH = unittest::assertGet(getRollbackChecker()->checkForRollback(callback));
    ASSERT(rbCBH);

    commandResponse = BSON("ok" << 1 << "rbid" << 4);
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        getNet()->scheduleSuccessfulResponse(commandResponse);
        getNet()->runReadyNetworkOperations();
    }

    getExecutor().wait(rbCBH);
    ASSERT_EQUALS(getRollbackChecker()->getLastRBID_forTest(), 4);
    ASSERT_EQUALS(getRollbackChecker()->getBaseRBID(), 3);
    LockGuard lk(_mutex);
    ASSERT_TRUE(_hasCalledCallback);
    ASSERT_TRUE(unittest::assertGet(_hasRolledBackResult));
}

TEST_F(RollbackCheckerTest, NoRollbackRBID) {
    auto callback = [this](const RollbackChecker::Result& result) {
        LockGuard lk(_mutex);
        _hasRolledBackResult = result;
        _hasCalledCallback = true;
    };
    // First set the RBID to 3.
    auto refreshCBH = unittest::assertGet(getRollbackChecker()->reset(callback));
    ASSERT(refreshCBH);
    auto commandResponse = BSON("ok" << 1 << "rbid" << 3);
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        getNet()->scheduleSuccessfulResponse(commandResponse);
        getNet()->runReadyNetworkOperations();
    }
    getExecutor().wait(refreshCBH);
    ASSERT_EQUALS(getRollbackChecker()->getBaseRBID(), 3);
    {
        LockGuard lk(_mutex);
        ASSERT_TRUE(_hasCalledCallback);
        ASSERT_TRUE(unittest::assertGet(_hasRolledBackResult));
        _hasCalledCallback = false;
    }

    // Check for rollback
    auto rbCBH = unittest::assertGet(getRollbackChecker()->checkForRollback(callback));
    ASSERT(rbCBH);

    commandResponse = BSON("ok" << 1 << "rbid" << 3);
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        getNet()->scheduleSuccessfulResponse(commandResponse);
        getNet()->runReadyNetworkOperations();
    }

    getExecutor().wait(rbCBH);
    ASSERT_EQUALS(getRollbackChecker()->getLastRBID_forTest(), 3);
    ASSERT_EQUALS(getRollbackChecker()->getBaseRBID(), 3);
    LockGuard lk(_mutex);
    ASSERT_TRUE(_hasCalledCallback);
    ASSERT_FALSE(unittest::assertGet(_hasRolledBackResult));
}
}  // namespace
