/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include <memory>

#include "mongo/base/string_data.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace executor {

struct RemoteCommandRequest;
class TaskExecutor;
class NetworkInterface;
class NetworkInterfaceMock;

/**
 * Test fixture for tests that require a TaskExecutor backed by a NetworkInterfaceMock.
 */
class TaskExecutorTest : public unittest::Test {
public:
    /**
     * Creates an initial error status suitable for checking if
     * component has modified the 'status' field in test fixture.
     */
    static Status getDetectableErrorStatus();

    /**
     * Validates command name in remote command request.
     */
    static void assertRemoteCommandNameEquals(StringData cmdName,
                                              const RemoteCommandRequest& request);

protected:
    virtual ~TaskExecutorTest();

    executor::NetworkInterfaceMock* getNet() {
        return _net;
    }
    TaskExecutor& getExecutor() {
        return *_executor;
    }

    /**
     * Initializes both the NetworkInterfaceMock and TaskExecutor but does not start the executor.
     */
    void setUp() override;

    /**
     * Destroys the replication executor.
     *
     * Shuts down and joins the running executor.
     */
    void tearDown() override;

    void launchExecutorThread();
    void shutdownExecutorThread();
    void joinExecutorThread();

private:
    virtual std::unique_ptr<TaskExecutor> makeTaskExecutor(
        std::unique_ptr<NetworkInterfaceMock> net) = 0;

    virtual void postExecutorThreadLaunch();

    NetworkInterfaceMock* _net;
    std::unique_ptr<TaskExecutor> _executor;

    /**
     * kPreStart -> kRunning -> kJoinRequired -> kJoining -> kShutdownComplete
     */
    enum LifecycleState { kPreStart, kRunning, kJoinRequired, kJoining, kShutdownComplete };
    LifecycleState _executorState = LifecycleState::kPreStart;
};

}  // namespace executor
}  // namespace mongo
