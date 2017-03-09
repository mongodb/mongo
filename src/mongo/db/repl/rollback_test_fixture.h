/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#pragma once

#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"

namespace mongo {
namespace repl {

/**
 * Test fixture for both 3.4 and 3.6 rollback unit tests.
 * The fixture makes available to tests:
 * - an "ephemeralForTest" storage engine for checking results of the rollback algorithm at the
 *   storage layer. The storage engine is initialized as part of the ServiceContextForMongoD test
 *   fixture.
 * - a task executor for simulating remote command responses from the sync source. The
 *   ThreadPoolExecutorTest is used to initialize the task executor.
 */
class RollbackTest : public unittest::Test {
public:
    /**
     * Initializes executor::ThreadPoolExecutorTest so that each thread is initialized with a
     * Client.
     */
    RollbackTest();

    /**
     * Initializes the service context and task executor.
     */
    void setUp() override;

    /**
     * Destroys the service context and task executor.
     *
     * Note on overriding tearDown() in tests:
     * Tests should explicitly shut down and join the task executor (by invoking
     * TaskExecutorTest::shutdownExecutorThread() and TaskExecutorTest::joinExecutorThread()
     * respectively) before calling RollbackTest::tearDown().
     * This cancels outstanding tasks and remote command requests scheduled using the task
     * executor.
     */
    void tearDown() override;

protected:
    // Test fixture used to manage the service context and global storage engine.
    ServiceContextMongoDTest _serviceContextMongoDTest;

    // Test fixture used to manage the task executor.
    executor::ThreadPoolExecutorTest _threadPoolExecutorTest;

    // OperationContext provided to test cases for storage layer operations.
    ServiceContext::UniqueOperationContext _opCtx;

    // ReplicationCoordinator mock implementation for rollback tests.
    // Owned by service context.
    class ReplicationCoordinatorRollbackMock;
    ReplicationCoordinatorRollbackMock* _coordinator = nullptr;

    // StorageInterface used to access minValid.
    StorageInterfaceMock _storageInterface;
};

/**
 * ReplicationCoordinator mock implementation for rollback tests.
 */
class RollbackTest::ReplicationCoordinatorRollbackMock : public ReplicationCoordinatorMock {
public:
    ReplicationCoordinatorRollbackMock(ServiceContext* service);

    /**
     * Base class implementation triggers an invariant. This function is overridden to be a no-op
     * for rollback tests.
     */
    void resetLastOpTimesFromOplog(OperationContext* opCtx) override;

    /**
     * Returns false (does not forward call to ReplicationCoordinatorMock::setFollowerMode())
     * if new state requested is '_failSetFollowerModeOnThisMemberState'.
     * Otherwise, calls ReplicationCoordinatorMock::setFollowerMode().
     */
    bool setFollowerMode(const MemberState& newState) override;

    // Override this to make setFollowerMode() fail when called with this state.
    MemberState _failSetFollowerModeOnThisMemberState = MemberState::RS_UNKNOWN;
};

}  // namespace repl
}  // namespace mongo
