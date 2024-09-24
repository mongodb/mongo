/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <absl/container/node_hash_map.h>
#include <array>
#include <boost/none.hpp>
#include <boost/smart_ptr.hpp>
#include <cstdint>
#include <fmt/format.h>
#include <memory>
#include <string>
#include <type_traits>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/executor/async_rpc_util.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/client_cursor/cursor_response_gen.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/repl/task_executor_mock.h"
#include "mongo/executor/async_rpc.h"
#include "mongo/executor/async_rpc_error_info.h"
#include "mongo/executor/async_rpc_retry_policy.h"
#include "mongo/executor/async_rpc_targeter.h"
#include "mongo/executor/async_rpc_test_fixture.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/executor_test_util.h"
#include "mongo/util/future.h"

namespace mongo {
namespace async_rpc {
namespace {

/**
 * Tests the getAllResponsesOrFirstErrorWithCancellation function returns the responses
 * of each sendCommand.
 */
TEST_F(AsyncRPCTestFixture, GetAllResponsesUtil) {
    const size_t total_targets = 3;
    auto opCtxHolder = makeOperationContext();
    DatabaseName testDbName = DatabaseName::createDatabaseName_forTest(boost::none, "testdb");
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(testDbName);
    FindCommandRequest findCmd(nss);

    CancellationSource source;
    std::vector<ExecutorFuture<async_rpc::AsyncRPCResponse<FindCommandRequest::Reply>>> futures;
    for (size_t i = 0; i < total_targets; ++i) {
        std::unique_ptr<Targeter> targeter = std::make_unique<LocalHostTargeter>();
        auto options = std::make_shared<AsyncRPCOptions<FindCommandRequest>>(
            getExecutorPtr(), source.token(), findCmd);
        futures.push_back(async_rpc::sendCommand<FindCommandRequest>(
            options, opCtxHolder.get(), std::move(targeter)));
    }

    auto responsesFut = async_rpc::getAllResponsesOrFirstErrorWithCancellation<
        size_t,
        async_rpc::AsyncRPCResponse<FindCommandRequest::Reply>>(
        std::move(futures),
        source,
        [](async_rpc::AsyncRPCResponse<FindCommandRequest::Reply> reply, size_t index) -> size_t {
            return index;
        });

    NetworkTestEnv::OnCommandFunction returnSuccess = [&](const auto& request) {
        return CursorResponse(nss, 0LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    };
    onCommands(std::vector(total_targets, returnSuccess));

    auto responses = responsesFut.get();
    std::sort(responses.begin(), responses.end());

    for (size_t i = 0; i < total_targets; ++i) {
        ASSERT_EQ(responses[i], i);
    }
}

/**
 * Tests that when the getAllResponsesOrFirstErrorWithCancellation function cancels early due
 * to an error, the rest of the sendCommand functions are able to run to completion.
 */
TEST_F(AsyncRPCTestFixture, GetAllResponsesUtilCancellationFinishesResolvingPendingFutures) {
    auto opCtxHolder = makeOperationContext();
    DatabaseName testDbName = DatabaseName::createDatabaseName_forTest(boost::none, "testdb");
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(testDbName);
    FindCommandRequest findCmd(nss);

    const auto timeoutStatus = Status(ErrorCodes::NetworkTimeout, "dummy status");

    CancellationSource source;
    std::vector<ExecutorFuture<async_rpc::AsyncRPCResponse<FindCommandRequest::Reply>>> futures;

    // Make an immediately ready future filled with an error status, intended to cancel other
    // futures.
    futures.push_back(ExecutorFuture<AsyncRPCResponse<FindCommandRequest::Reply>>(
        getExecutorPtr(), Status{AsyncRPCErrorInfo(timeoutStatus), "mock"}));

    std::unique_ptr<Targeter> targeter = std::make_unique<LocalHostTargeter>();
    auto options = std::make_shared<AsyncRPCOptions<FindCommandRequest>>(
        getExecutorPtr(), source.token(), findCmd);
    futures.push_back(async_rpc::sendCommand<FindCommandRequest>(
        options, opCtxHolder.get(), std::move(targeter)));

    // runReadyNetworkOperations blocks until waitForWork is called, therefore ensuring
    // sendCommand schedules the request.
    auto network = getNetworkInterfaceMock();
    network->enterNetwork();
    network->exitNetwork();

    // A ready future with an error status already exists, so the util function will
    // be able to cancel the token before the network steps below.
    auto responsesFut = async_rpc::getAllResponsesOrFirstErrorWithCancellation<
        async_rpc::AsyncRPCResponse<FindCommandRequest::Reply>,
        async_rpc::AsyncRPCResponse<FindCommandRequest::Reply>>(
        std::move(futures),
        source,
        [](async_rpc::AsyncRPCResponse<FindCommandRequest::Reply> reply, size_t index)
            -> async_rpc::AsyncRPCResponse<FindCommandRequest::Reply> { return reply; });

    network->enterNetwork();
    ASSERT_TRUE(source.token().isCanceled());

    // Make sure that despite cancellation, responseFut is not ready because there is still
    // a future that has not finished.
    ASSERT_FALSE(responsesFut.isReady());

    // Process the cancellation, which should result in the second future returning
    // CallbackCancelled.
    network->runReadyNetworkOperations();
    network->exitNetwork();

    auto counters = network->getCounters();
    ASSERT_EQ(counters.canceled, 1);

    auto error = responsesFut.getNoThrow().getStatus();
    ASSERT_EQ(error.code(), ErrorCodes::RemoteCommandExecutionError);
    auto extraInfo = error.extraInfo<AsyncRPCErrorInfo>();
    ASSERT(extraInfo);
    ASSERT(extraInfo->isLocal());
    ASSERT_EQ(extraInfo->asLocal(), timeoutStatus);
}

/**
 * Test that responses from getAllResponsesOrFirstErrorWithCancellation are still processed, and
 * the appropriate cancellation actions are still taken, if the executor rejects the work of
 * processing the response. */
TEST_F(AsyncRPCTestFixture, ExecutorShutdownErrorProcessed) {
    auto opCtxHolder = makeOperationContext();
    DatabaseName testDbName = DatabaseName::createDatabaseName_forTest(boost::none, "testdb");
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(testDbName);

    // Set up a find command to schedule.
    FindCommandRequest findCmd(nss);

    using Reply = AsyncRPCResponse<FindCommandRequest::Reply>;
    CancellationSource source;
    std::vector<ExecutorFuture<Reply>> responseFutureContainer;

    // Set up an executor that rejects work that is scheduled.
    auto rejectingExecutor = std::make_shared<repl::TaskExecutorMock>(getExecutorPtr().get());
    rejectingExecutor->shouldFailScheduleWorkRequest = []() {
        return true;
    };
    rejectingExecutor->failureCode = ErrorCodes::ShutdownInProgress;

    // Try and send a find command and store the result future.
    std::unique_ptr<Targeter> targeter = std::make_unique<LocalHostTargeter>();
    auto options = std::make_shared<AsyncRPCOptions<FindCommandRequest>>(
        rejectingExecutor, source.token(), findCmd);
    responseFutureContainer.push_back(sendCommand(options, opCtxHolder.get(), std::move(targeter)));

    auto response = getAllResponsesOrFirstErrorWithCancellation<size_t, Reply>(
                        std::move(responseFutureContainer),
                        source,
                        [](Reply reply, size_t index) -> size_t { return index; })
                        .getNoThrow();

    // We expect to the promise to get resolved even if the executor is rejecteding work.
    auto error = response.getStatus();
    ASSERT_EQ(error.code(), ErrorCodes::RemoteCommandExecutionError);
    auto extraInfo = error.extraInfo<AsyncRPCErrorInfo>();
    ASSERT(extraInfo->isLocal());
    ASSERT_EQ(extraInfo->asLocal().code(), ErrorCodes::ShutdownInProgress);
    // We also expect that the helper utility has canceled all other work, now that the executor
    // has rejected work.
    ASSERT(source.token().isCanceled());
}

}  // namespace
}  // namespace async_rpc
}  // namespace mongo
