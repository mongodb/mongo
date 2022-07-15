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

#include "mongo/executor/remote_command_retry_policy.h"
#include "mongo/executor/remote_command_runner.h"
#include "mongo/executor/remote_command_targeter.h"

#include "mongo/bson/oid.h"
#include "mongo/db/repl/hello_gen.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_test_fixture.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include <memory>

namespace mongo {
namespace executor {
namespace remote_command_runner {
namespace {

using executor::NetworkTestEnv;

class RemoteCommandRunnerTestFixture : public ThreadPoolExecutorTest {
public:
    void setUp() {
        TaskExecutorTest::setUp();
        launchExecutorThread();

        _networkTestEnv = std::make_shared<NetworkTestEnv>(getExecutorPtr().get(), getNet());
    }

    void tearDown() {
        TaskExecutorTest::tearDown();
        _networkTestEnv.reset();
    }

    void onCommand(NetworkTestEnv::OnCommandFunction func) {
        _networkTestEnv->onCommand(func);
    }

    /**
     * Initialize an IDL command with the necessary fields (dbName) to avoid an invariant failure.
     */
    template <typename CommandType>
    void initializeCommand(CommandType& c) {
        c.setDbName("testdb");
    }

    /**
     * Mocks an error response from a remote with the given 'status'.
     */
    BSONObj createErrorResponse(Status status) {
        invariant(!status.isOK());
        BSONObjBuilder result;
        status.serializeErrorToBSON(&result);
        result.appendBool("ok", false);
        return result.obj();
    }

protected:
    CancellationToken _cancellationToken{CancellationToken::uncancelable()};

private:
    std::shared_ptr<NetworkTestEnv> _networkTestEnv;
};

/*
 * Mock a successful network response to hello command.
 */
TEST_F(RemoteCommandRunnerTestFixture, SuccessfulHello) {
    HelloCommandReply helloReply = HelloCommandReply(TopologyVersion(OID::gen(), 0));
    HelloCommand helloCmd;
    initializeCommand(helloCmd);

    SemiFuture<HelloCommandReply> resultFuture =
        doRequest(helloCmd, nullptr, getExecutorPtr(), _cancellationToken);

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["hello"]);
        return helloReply.toBSON();
    });

    HelloCommandReply res = resultFuture.get();

    ASSERT_BSONOBJ_EQ(res.toBSON(), helloReply.toBSON());
}

/*
 * Mock error on local host side.
 */
TEST_F(RemoteCommandRunnerTestFixture, LocalError) {
    HelloCommand helloCmd;
    initializeCommand(helloCmd);

    SemiFuture<HelloCommandReply> resultFuture =
        doRequest(helloCmd, nullptr, getExecutorPtr(), _cancellationToken);

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["hello"]);
        return Status(ErrorCodes::NetworkTimeout, "mock");
    });

    ASSERT_THROWS_CODE(resultFuture.get(), DBException, ErrorCodes::NetworkTimeout);
}

/*
 * Mock error on remote host.
 */
TEST_F(RemoteCommandRunnerTestFixture, RemoteError) {
    HelloCommand helloCmd;
    initializeCommand(helloCmd);

    SemiFuture<HelloCommandReply> resultFuture =
        doRequest(helloCmd, nullptr, getExecutorPtr(), _cancellationToken);

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["hello"]);
        return createErrorResponse(Status(ErrorCodes::BadValue, "mock"));
    });

    ASSERT_THROWS_CODE(resultFuture.get(), DBException, ErrorCodes::BadValue);
}

/*
 * Mock write concern error on remote host.
 */
TEST_F(RemoteCommandRunnerTestFixture, WriteConcernError) {
    HelloCommand helloCmd;
    initializeCommand(helloCmd);

    const BSONObj writeConcernError = BSON("code" << ErrorCodes::WriteConcernFailed << "errmsg"
                                                  << "mock");
    const BSONObj resWithWriteConcernError =
        BSON("ok" << 1 << "writeConcernError" << writeConcernError);

    SemiFuture<HelloCommandReply> resultFuture =
        doRequest(helloCmd, nullptr, getExecutorPtr(), _cancellationToken);

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["hello"]);
        return resWithWriteConcernError;
    });

    ASSERT_THROWS_CODE(resultFuture.get(), DBException, ErrorCodes::WriteConcernFailed);
}

/*
 * Mock write error on remote host.
 */
TEST_F(RemoteCommandRunnerTestFixture, WriteError) {
    HelloCommand helloCmd;
    initializeCommand(helloCmd);

    const BSONObj writeErrorExtraInfo = BSON("failingDocumentId" << OID::gen());
    const BSONObj writeError = BSON("code" << ErrorCodes::DocumentValidationFailure << "errInfo"
                                           << writeErrorExtraInfo << "errmsg"
                                           << "Document failed validation");
    const BSONObj resWithWriteError = BSON("ok" << 1 << "writeErrors" << BSON_ARRAY(writeError));
    SemiFuture<HelloCommandReply> resultFuture =
        doRequest(helloCmd, nullptr, getExecutorPtr(), _cancellationToken);

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["hello"]);
        return resWithWriteError;
    });

    ASSERT_THROWS_CODE(resultFuture.get(), DBException, ErrorCodes::DocumentValidationFailure);
}

/*
 * Basic Targeter that returns the host that invoked it.
 */
TEST_F(RemoteCommandRunnerTestFixture, LocalTargeter) {
    RemoteCommandLocalHostTargeter t;
    auto targetFuture = t.resolve(_cancellationToken);
    auto target = targetFuture.get();

    ASSERT_EQ(target.size(), 1);
    ASSERT_EQ(HostAndPort("localhost", serverGlobalParams.port), target[0]);
}

/*
 * Basic RetryPolicy that never retries.
 */
TEST_F(RemoteCommandRunnerTestFixture, NoRetry) {
    RemoteCommandNoRetryPolicy p;

    ASSERT_FALSE(p.shouldRetry(Status(ErrorCodes::BadValue, "mock")));
    ASSERT_EQUALS(p.getNextRetryDelay(), Milliseconds::zero());
}

}  // namespace
}  // namespace remote_command_runner
}  // namespace executor
}  // namespace mongo
