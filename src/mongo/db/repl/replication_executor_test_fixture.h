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

#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace executor {
    class NetworkInterfaceMock;
} // namespace executor

namespace repl {

    using std::unique_ptr;

    class ReplicationExecutor;
    class StorageInterfaceMock;

    /**
     * Test fixture for tests that require a ReplicationExecutor backed by
     * a NetworkInterfaceMock.
     */
    class ReplicationExecutorTest : public unittest::Test {
    public:

        /**
         * Creates an initial error status suitable for checking if
         * component has modified the 'status' field in test fixture.
         */
        static Status getDetectableErrorStatus();

    protected:
        executor::NetworkInterfaceMock* getNet() { return _net; }
        ReplicationExecutor& getExecutor() { return *_executor; }
        /**
         * Runs ReplicationExecutor in background.
         */
        void launchExecutorThread();

        /**
         * Anything that needs to be done after launchExecutorThread should go in here.
         */
        virtual void postExecutorThreadLaunch();

        /**
         * Waits for background ReplicationExecutor to stop running.
         *
         * The executor should be shutdown prior to calling this function
         * or the test may block indefinitely.
         */
        void joinExecutorThread();

        /**
         * Initializes both the NetworkInterfaceMock and ReplicationExecutor but
         * does not run the executor in the background.
         *
         * To run the executor in the background, tests should invoke launchExecutorThread() or
         * override this function() to achieve the same effect.
         */
        void setUp() override;

        /**
         * Destroys the replication executor.
         *
         * Shuts down running background executor.
         */
        void tearDown() override;


    private:
        executor::NetworkInterfaceMock* _net;
        StorageInterfaceMock* _storage;
        unique_ptr<ReplicationExecutor> _executor;
        unique_ptr<stdx::thread> _executorThread;
    };

}  // namespace repl
}  // namespace mongo
