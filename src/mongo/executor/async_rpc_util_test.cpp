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
#include "mongo/unittest/unittest.h"
#include "mongo/util/executor_test_util.h"
#include "mongo/util/future.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <fmt/format.h>

namespace mongo {
namespace async_rpc {
namespace {

class AsyncRPCUtilTest : public AsyncRPCTestFixture {
public:
    NamespaceString kNss{NamespaceString::createNamespaceString_forTest(
        DatabaseName::createDatabaseName_forTest(boost::none, "testdb"))};

    /**
     * Sends `totalTargets` find commands and schedules success responses for those commands.
     * Returns the futures from scheduling the commands.
     */
    std::vector<ExecutorFuture<async_rpc::AsyncRPCResponse<FindCommandRequest::Reply>>>
    setupSuccessTest(OperationContext* opCtx,
                     const size_t totalTargets,
                     CancellationSource source) {
        FindCommandRequest findCmd(kNss);

        std::vector<ExecutorFuture<async_rpc::AsyncRPCResponse<FindCommandRequest::Reply>>> futures;
        for (size_t i = 0; i < totalTargets; ++i) {
            std::unique_ptr<Targeter> targeter = std::make_unique<LocalHostTargeter>();
            auto options = std::make_shared<AsyncRPCOptions<FindCommandRequest>>(
                getExecutorPtr(), source.token(), findCmd);
            futures.push_back(
                async_rpc::sendCommand<FindCommandRequest>(options, opCtx, std::move(targeter)));
        }

        NetworkTestEnv::OnCommandFunction returnSuccess = [&](const auto& request) {
            return CursorResponse(kNss, 0LL, {BSON("x" << 1)})
                .toBSON(CursorResponse::ResponseType::InitialResponse);
        };
        onCommands(std::vector(totalTargets, returnSuccess));

        return futures;
    }

    /**
     * Creates one future with an error status and schedules one find command. Returns a vector of
     * futures containing the ready one with an error status matching the passed in status and the
     * pending one from the find request.
     */
    std::vector<ExecutorFuture<async_rpc::AsyncRPCResponse<FindCommandRequest::Reply>>>
    setupErrorTest(OperationContext* opCtx, CancellationSource source, const Status& errorStatus) {
        FindCommandRequest findCmd(kNss);

        std::vector<ExecutorFuture<async_rpc::AsyncRPCResponse<FindCommandRequest::Reply>>> futures;

        // Make an immediately ready future filled with an error status.
        futures.push_back(ExecutorFuture<AsyncRPCResponse<FindCommandRequest::Reply>>(
            getExecutorPtr(), Status{AsyncRPCErrorInfo(errorStatus), "mock"}));

        std::unique_ptr<Targeter> targeter = std::make_unique<LocalHostTargeter>();
        auto options = std::make_shared<AsyncRPCOptions<FindCommandRequest>>(
            getExecutorPtr(), source.token(), findCmd);
        futures.push_back(
            async_rpc::sendCommand<FindCommandRequest>(options, opCtx, std::move(targeter)));

        // runReadyNetworkOperations blocks until waitForWork is called, therefore ensuring
        // sendCommand schedules the request.
        auto network = getNetworkInterfaceMock();
        network->enterNetwork();
        network->exitNetwork();

        return futures;
    }

    /**
     * Creates an executor which is rejecting work due to shutdown. Schedules a find command with
     * this excutor and returns the future from that find command.
     */
    std::vector<ExecutorFuture<async_rpc::AsyncRPCResponse<FindCommandRequest::Reply>>>
    setupExecutorShutdownTest(OperationContext* opCtx, CancellationSource source) {
        // Set up a find command to schedule.
        FindCommandRequest findCmd(kNss);

        std::vector<ExecutorFuture<AsyncRPCResponse<FindCommandRequest::Reply>>> futures;

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
        futures.push_back(sendCommand(options, opCtx, std::move(targeter)));

        return futures;
    }
};

/**
 * Tests the getAllResponsesOrFirstErrorWithCancellation function returns the responses
 * of each sendCommand.
 */
TEST_F(AsyncRPCUtilTest, GetAllResponsesOrFirstErrorUtil) {
    const size_t total_targets = 3;
    auto opCtxHolder = makeOperationContext();
    CancellationSource source;

    auto futures = setupSuccessTest(opCtxHolder.get(), total_targets, source);

    auto responsesFut = async_rpc::getAllResponsesOrFirstErrorWithCancellation<
        size_t,
        async_rpc::AsyncRPCResponse<FindCommandRequest::Reply>>(
        std::move(futures),
        source,
        [](async_rpc::AsyncRPCResponse<FindCommandRequest::Reply> reply, size_t index) -> size_t {
            return index;
        });

    auto responses = responsesFut.get();
    std::sort(responses.begin(), responses.end());

    for (size_t i = 0; i < total_targets; ++i) {
        ASSERT_EQ(responses[i], i);
    }
}

/**
 * Tests the getAllResponses function returns the responses of each sendCommand.
 */
TEST_F(AsyncRPCUtilTest, GetAllResponsesUtil) {
    const size_t total_targets = 3;
    auto opCtxHolder = makeOperationContext();
    CancellationSource source;

    auto futures = setupSuccessTest(opCtxHolder.get(), total_targets, source);

    auto responsesFut =
        async_rpc::getAllResponses<StatusWith<size_t>,
                                   async_rpc::AsyncRPCResponse<FindCommandRequest::Reply>>(
            std::move(futures),
            [](StatusWith<async_rpc::AsyncRPCResponse<FindCommandRequest::Reply>> swReply,
               size_t index) -> StatusWith<size_t> {
                if (!swReply.isOK())
                    return swReply.getStatus();
                else
                    return index;
            });

    auto responses = responsesFut.get();

    size_t responseSum = 0;
    size_t expectedSum = 0;
    for (size_t i = 0; i < total_targets; ++i) {
        ASSERT_OK(responses[i]);
        responseSum += responses[i].getValue();
        expectedSum += i;
    }
    ASSERT_EQ(responseSum, expectedSum);
}

/**
 * Test that when the user supplied lambda throws an exception, the top level future returned will
 * contain that error.
 */
TEST_F(AsyncRPCUtilTest, GetAllResponsesUtilExceptionCancelsPendingRequests) {
    const size_t total_targets = 3;
    auto opCtxHolder = makeOperationContext();
    CancellationSource source;

    auto futures = setupSuccessTest(opCtxHolder.get(), total_targets, source);

    auto responsesFut =
        async_rpc::getAllResponses<StatusWith<size_t>,
                                   async_rpc::AsyncRPCResponse<FindCommandRequest::Reply>>(
            std::move(futures),
            [](StatusWith<async_rpc::AsyncRPCResponse<FindCommandRequest::Reply>> swReply,
               size_t index) -> StatusWith<size_t> {
                uassert(
                    ErrorCodes::InvalidOptions, "Dummy error thrown on client side", index != 0);
                if (!swReply.isOK())
                    return swReply.getStatus();
                else
                    return index;
            });

    auto response = responsesFut.getNoThrow();

    ASSERT_NOT_OK(response);
    ASSERT_EQ(response.getStatus().code(), ErrorCodes::InvalidOptions);
}

/**
 * Tests that when the getAllResponsesOrFirstErrorWithCancellation function cancels early due
 * to an error, the rest of the sendCommand functions are able to run to completion.
 */
TEST_F(AsyncRPCUtilTest,
       GetAllResponsesOrFirstErrorUtilCancellationFinishesResolvingPendingFutures) {
    auto opCtxHolder = makeOperationContext();
    CancellationSource source;
    const auto timeoutStatus = Status(ErrorCodes::NetworkTimeout, "dummy status");

    auto futures = setupErrorTest(opCtxHolder.get(), source, timeoutStatus);

    // A ready future with an error status already exists, so the util function will
    // be able to cancel the token before the network steps below.
    auto responsesFut = async_rpc::getAllResponsesOrFirstErrorWithCancellation<
        async_rpc::AsyncRPCResponse<FindCommandRequest::Reply>,
        async_rpc::AsyncRPCResponse<FindCommandRequest::Reply>>(
        std::move(futures),
        source,
        [](async_rpc::AsyncRPCResponse<FindCommandRequest::Reply> reply, size_t index)
            -> async_rpc::AsyncRPCResponse<FindCommandRequest::Reply> { return reply; });

    auto network = getNetworkInterfaceMock();
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
 * Tests that when the getAllResponses function finds an error, it completes all
 * the other tasks and then returns all of their results.
 */
TEST_F(AsyncRPCUtilTest, GetAllResponsesUtilErrorsDoNotAffectOtherCommands) {
    auto opCtxHolder = makeOperationContext();
    CancellationSource source;
    const auto timeoutStatus = Status(ErrorCodes::NetworkTimeout, "dummy status");

    auto futures = setupErrorTest(opCtxHolder.get(), source, timeoutStatus);

    auto responsesFut =
        async_rpc::getAllResponses<StatusWith<size_t>,
                                   async_rpc::AsyncRPCResponse<FindCommandRequest::Reply>>(
            std::move(futures),
            [](StatusWith<async_rpc::AsyncRPCResponse<FindCommandRequest::Reply>> swReply,
               size_t index) -> StatusWith<size_t> {
                if (!swReply.isOK())
                    return swReply.getStatus();
                else
                    return index;
            });

    auto network = getNetworkInterfaceMock();
    network->enterNetwork();
    ASSERT_FALSE(source.token().isCanceled());

    // Make sure that responseFut is not ready because there is still a future that has not
    // finished.
    ASSERT_FALSE(responsesFut.isReady());

    // Process the failure, which should not affect anything.
    network->runReadyNetworkOperations();
    network->exitNetwork();

    auto counters = network->getCounters();
    ASSERT_EQ(counters.canceled, 0);

    // Issue a success response for the other response.
    NetworkTestEnv::OnCommandFunction returnSuccess = [&](const auto& request) {
        return CursorResponse(kNss, 0LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    };
    onCommands({returnSuccess});

    auto responses = responsesFut.get();
    ASSERT_EQ(responses.size(), 2);

    auto checkErrorResponse = [&](const StatusWith<size_t> response) {
        ASSERT_NOT_OK(response);
        auto error = response.getStatus();
        ASSERT_EQ(error.code(), ErrorCodes::RemoteCommandExecutionError);
        auto extraInfo = error.extraInfo<AsyncRPCErrorInfo>();
        ASSERT(extraInfo);
        ASSERT(extraInfo->isLocal());
        ASSERT_EQ(extraInfo->asLocal(), timeoutStatus);
    };

    if (responses[0].isOK()) {
        checkErrorResponse(responses[1]);
    } else {
        ASSERT_OK(responses[1]);
        checkErrorResponse(responses[0]);
    }
}

/**
 * Test that responses from getAllResponsesOrFirstErrorWithCancellation are still processed, and
 * the appropriate cancellation actions are still taken, if the executor rejects the work of
 * processing the response. */
TEST_F(AsyncRPCUtilTest, GetAllResponsesOrFirstErrorUtilExecutorShutdownErrorProcessed) {
    auto opCtxHolder = makeOperationContext();
    CancellationSource source;
    using Reply = AsyncRPCResponse<FindCommandRequest::Reply>;

    auto futures = setupExecutorShutdownTest(opCtxHolder.get(), source);

    auto response =
        getAllResponsesOrFirstErrorWithCancellation<size_t, Reply>(
            std::move(futures), source, [](Reply reply, size_t index) -> size_t { return index; })
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

/**
 * Test that responses from getAllResponses are still processed, if the executor rejects the work of
 * processing the response.
 */
TEST_F(AsyncRPCUtilTest, GetAllResponsesUtilExecutorShutdownErrorProcessed) {
    auto opCtxHolder = makeOperationContext();
    CancellationSource source;

    auto futures = setupExecutorShutdownTest(opCtxHolder.get(), source);

    auto responses =
        getAllResponses<StatusWith<size_t>, AsyncRPCResponse<FindCommandRequest::Reply>>(
            std::move(futures),
            [](StatusWith<AsyncRPCResponse<FindCommandRequest::Reply>> swReply,
               size_t index) -> StatusWith<size_t> {
                if (!swReply.isOK())
                    return swReply.getStatus();
                else
                    return index;
            })
            .get();

    // We expect to the promise to get resolved even if the executor is rejecteding work.
    ASSERT_EQ(responses.size(), 1);
    ASSERT_NOT_OK(responses[0]);
    auto error = responses[0].getStatus();
    ASSERT_EQ(error.code(), ErrorCodes::RemoteCommandExecutionError);
    auto extraInfo = error.extraInfo<AsyncRPCErrorInfo>();
    ASSERT(extraInfo->isLocal());
    ASSERT_EQ(extraInfo->asLocal().code(), ErrorCodes::ShutdownInProgress);
}
}  // namespace
}  // namespace async_rpc
}  // namespace mongo
