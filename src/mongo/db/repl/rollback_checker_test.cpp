
/**
 *    Copyright 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/client.h"
#include "mongo/db/repl/replication_executor_test_fixture.h"
#include "mongo/db/repl/rollback_checker.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/util/log.h"

#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;
using namespace mongo::repl;
using executor::NetworkInterfaceMock;
using executor::RemoteCommandResponse;

using LockGuard = stdx::lock_guard<stdx::mutex>;

class RollbackCheckerTest : public ReplicationExecutorTest {
public:
    RollbackChecker* getRollbackChecker() const;

protected:
    void setUp() override;

    std::unique_ptr<RollbackChecker> _rollbackChecker;
    RollbackChecker::Result _hasRolledBackResult = {ErrorCodes::NotYetInitialized, ""};
    bool _hasCalledCallback;
    mutable stdx::mutex _mutex;
};

void RollbackCheckerTest::setUp() {
    ReplicationExecutorTest::setUp();
    launchExecutorThread();
    _rollbackChecker = stdx::make_unique<RollbackChecker>(&getReplExecutor(), HostAndPort());
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _hasRolledBackResult = {ErrorCodes::NotYetInitialized, ""};
    _hasCalledCallback = false;
}

RollbackChecker* RollbackCheckerTest::getRollbackChecker() const {
    return _rollbackChecker.get();
}

TEST_F(RollbackCheckerTest, InvalidConstruction) {
    HostAndPort syncSource;

    // Null executor.
    ASSERT_THROWS_CODE(RollbackChecker(nullptr, syncSource), UserException, ErrorCodes::BadValue);
}

TEST_F(RollbackCheckerTest, ShutdownBeforeStart) {
    auto callback = [](const RollbackChecker::Result&) {};
    getReplExecutor().shutdown();
    ASSERT_NOT_OK(getRollbackChecker()->reset(callback).getStatus());
    ASSERT_NOT_OK(getRollbackChecker()->checkForRollback(callback).getStatus());
}

TEST_F(RollbackCheckerTest, ShutdownBeforeHasHadRollback) {
    getReplExecutor().shutdown();
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, getRollbackChecker()->hasHadRollback());
}

TEST_F(RollbackCheckerTest, ShutdownBeforeResetSync) {
    getReplExecutor().shutdown();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, getRollbackChecker()->reset_sync());
}

TEST_F(RollbackCheckerTest, reset) {
    auto callback = [](const RollbackChecker::Result&) {};
    auto cbh = unittest::assertGet(getRollbackChecker()->reset(callback));
    ASSERT(cbh);

    auto commandResponse = BSON("ok" << 1 << "rbid" << 3);
    getNet()->scheduleSuccessfulResponse(commandResponse);
    getNet()->runReadyNetworkOperations();
    getNet()->exitNetwork();

    getReplExecutor().wait(cbh);
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
    getNet()->scheduleSuccessfulResponse(commandResponse);
    getNet()->runReadyNetworkOperations();
    getReplExecutor().wait(refreshCBH);
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
    getNet()->scheduleSuccessfulResponse(commandResponse);
    getNet()->runReadyNetworkOperations();
    getNet()->exitNetwork();

    getReplExecutor().wait(rbCBH);
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
    getNet()->scheduleSuccessfulResponse(commandResponse);
    getNet()->runReadyNetworkOperations();
    getReplExecutor().wait(refreshCBH);
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
    getNet()->scheduleSuccessfulResponse(commandResponse);
    getNet()->runReadyNetworkOperations();
    getNet()->exitNetwork();

    getReplExecutor().wait(rbCBH);
    ASSERT_EQUALS(getRollbackChecker()->getLastRBID_forTest(), 3);
    ASSERT_EQUALS(getRollbackChecker()->getBaseRBID(), 3);
    LockGuard lk(_mutex);
    ASSERT_TRUE(_hasCalledCallback);
    ASSERT_FALSE(unittest::assertGet(_hasRolledBackResult));
}
}  // namespace
