/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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
#include <memory>

#include "mongo/bson/oid.h"
#include "mongo/db/concurrency/locker_noop_service_context_test_fixture.h"
#include "mongo/db/repl/hello_gen.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_retry_policy.h"
#include "mongo/executor/remote_command_runner.h"
#include "mongo/executor/remote_command_targeter.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_test_fixture.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"

namespace mongo::executor::remote_command_runner {
class RemoteCommandRunnerTestFixture : public LockerNoopServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        _net = std::make_shared<NetworkInterfaceMock>();
        ThreadPoolMock::Options opts{};
        _executor = std::make_shared<ThreadPoolTaskExecutor>(
            std::make_unique<ThreadPoolMock>(_net.get(), 1, std::move(opts)), _net);
        _executor->startup();

        _networkTestEnv = std::make_shared<NetworkTestEnv>(getExecutorPtr().get(), _net.get());
    }

    void tearDown() override {
        _networkTestEnv.reset();
        _executor.reset();
        _net.reset();
        ServiceContextTest::tearDown();
    }

    void onCommand(NetworkTestEnv::OnCommandFunction func) {
        _networkTestEnv->onCommand(func);
    }

    /**
     * Initialize an IDL command with the necessary fields (dbName) to avoid an invariant failure.
     */
    template <typename CommandType>
    void initializeCommand(CommandType& c) {
        c.setDbName(DatabaseName("testdb", boost::none));
    }

    /**
     * Generates an error BSONObj command response, using the given 'status' as
     * the error.
     */
    BSONObj createErrorResponse(Status status) {
        invariant(!status.isOK());
        BSONObjBuilder result;
        status.serializeErrorToBSON(&result);
        result.appendBool("ok", false);
        return result.obj();
    }

    TaskExecutor& getExecutor() const {
        return *_executor;
    }

    std::shared_ptr<TaskExecutor> getExecutorPtr() const {
        return _executor;
    }

    NetworkInterfaceMock* getNetworkInterfaceMock() const {
        return _net.get();
    }

protected:
    CancellationToken _cancellationToken{CancellationToken::uncancelable()};

private:
    std::shared_ptr<NetworkTestEnv> _networkTestEnv;
    std::shared_ptr<TaskExecutor> _executor;
    std::shared_ptr<NetworkInterfaceMock> _net;
};
}  // namespace mongo::executor::remote_command_runner
