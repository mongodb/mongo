/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/executor/async_multicaster.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/retry_strategy.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/future.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace {

using namespace mongo;
using namespace mongo::executor;

class AsyncMulticasterTest : public ServiceContextTest {
public:
    void setUp() override;

    // Common response factories
    RemoteCommandResponse makeSuccessResponse(const std::string& result = "success");
    RemoteCommandResponse makeRetryableErrorResponse();
    RemoteCommandResponse makeSystemOverloadedErrorResponse();

    std::vector<AsyncMulticaster::Reply> runMulticast(OperationContext* opCtx,
                                                      const std::vector<HostAndPort>& hosts,
                                                      Milliseconds timeout = Milliseconds(5000),
                                                      AsyncMulticaster::Options options = {});

    std::vector<AsyncMulticaster::Reply> runMulticastWithResponses(
        OperationContext* opCtx,
        const std::vector<HostAndPort>& hosts,
        const std::vector<RemoteCommandResponse>& responses,
        Milliseconds timeout = Milliseconds(5000),
        AsyncMulticaster::Options options = {});

    void assertCountAndAllSuccessful(const std::vector<AsyncMulticaster::Reply>& results,
                                     const std::vector<HostAndPort>& hosts,
                                     const std::vector<RemoteCommandResponse>& responses);

    void assertContainsError(const std::vector<AsyncMulticaster::Reply>& results,
                             ErrorCodes::Error expectedCode);

    executor::NetworkInterfaceMock* getNet() {
        return _net;
    }

protected:
    executor::NetworkInterfaceMock* _net = nullptr;
    std::shared_ptr<executor::ThreadPoolTaskExecutor> _threadPoolExecutor;

    std::vector<HostAndPort> makeHostList(size_t count);

    void processAllNetworkRequests(const std::vector<RemoteCommandResponse>& responses);
    void processNetworkRequest(const RemoteCommandResponse& response);

    template <typename Duration>
    void advanceTime(Duration d) {
        executor::NetworkInterfaceMock::InNetworkGuard guard(_net);
        _net->advanceTime(_net->now() + d);
    }

    bool networkHasReadyRequests() {
        executor::NetworkInterfaceMock::InNetworkGuard guard(_net);
        return _net->hasReadyRequests();
    }

    Milliseconds advanceUntilReadyRequest() const {
        using namespace std::literals;
        stdx::this_thread::sleep_for(1ms);
        auto totalWaited = Milliseconds{0};
        auto _ = executor::NetworkInterfaceMock::InNetworkGuard{_net};
        while (!_net->hasReadyRequests()) {
            auto advance = Milliseconds{10};
            _net->advanceTime(_net->now() + advance);
            totalWaited += advance;
            stdx::this_thread::sleep_for(100us);
        }
        return totalWaited;
    }

    void checkRequestReadyAfterDelay(const int testBaseBackoffMillis) {
        // Verify the request doesn't get immediately retried because 'SystemOverloadedError' label
        // implies entering in an backoff delay.
        auto waited = advanceUntilReadyRequest();
        ASSERT_GTE(waited, Milliseconds{testBaseBackoffMillis});
        ASSERT_TRUE(networkHasReadyRequests());
    }

private:
    BSONObj _defaultCmd;
    DatabaseName _defaultDbName;

    std::shared_ptr<executor::ThreadPoolTaskExecutor> getExecutor() {
        return _threadPoolExecutor;
    }

    executor::ThreadPoolMock::Options makeThreadPoolMockOptions() const {
        executor::ThreadPoolMock::Options options;
        options.onCreateThread = []() {
            Client::initThread("OplogFetcherTest", getGlobalServiceContext()->getService());
        };
        return options;
    };
};

void AsyncMulticasterTest::setUp() {
    ServiceContextTest::setUp();

    auto network = std::make_unique<executor::NetworkInterfaceMock>();
    _net = network.get();
    _threadPoolExecutor =
        makeThreadPoolTestExecutor(std::move(network), makeThreadPoolMockOptions());
    _threadPoolExecutor->startup();

    _defaultCmd = BSON("ping" << 1);
    _defaultDbName = DatabaseName::createDatabaseName_forTest(boost::none, "testdb");
}

std::vector<HostAndPort> AsyncMulticasterTest::makeHostList(size_t count) {
    std::vector<HostAndPort> hosts;
    for (size_t i = 0; i < count; ++i) {
        hosts.emplace_back("host" + std::to_string(i), 27017 + i);
    }
    return hosts;
}

void AsyncMulticasterTest::processNetworkRequest(const RemoteCommandResponse& response) {
    auto net = getNet();
    executor::NetworkInterfaceMock::InNetworkGuard guard(net);
    ASSERT_TRUE(net->hasReadyRequests());
    auto noi = net->getNextReadyRequest();
    net->scheduleResponse(noi, net->now(), response);
    net->runReadyNetworkOperations();
}

void AsyncMulticasterTest::processAllNetworkRequests(
    const std::vector<RemoteCommandResponse>& responses) {
    using namespace std::literals;
    for (const auto& response : responses) {
        while (!networkHasReadyRequests()) {
            stdx::this_thread::sleep_for(100us);
        }
        processNetworkRequest(response);
    }
}

RemoteCommandResponse AsyncMulticasterTest::makeSuccessResponse(const std::string& result) {
    return RemoteCommandResponse::make_forTest(BSON("ok" << 1 << "result" << result),
                                               Milliseconds(0));
}

RemoteCommandResponse AsyncMulticasterTest::makeRetryableErrorResponse() {
    return RemoteCommandResponse::make_forTest(
        BSON("errorLabels" << BSON_ARRAY(ErrorLabel::kRetryableError)), Milliseconds(0));
}

RemoteCommandResponse AsyncMulticasterTest::makeSystemOverloadedErrorResponse() {
    return RemoteCommandResponse::make_forTest(
        BSON("errorLabels" << BSON_ARRAY(ErrorLabel::kRetryableError
                                         << ErrorLabel::kSystemOverloadedError)),
        Milliseconds(0));
}

std::vector<AsyncMulticaster::Reply> AsyncMulticasterTest::runMulticast(
    OperationContext* opCtx,
    const std::vector<HostAndPort>& hosts,
    Milliseconds timeout,
    AsyncMulticaster::Options options) {

    AsyncMulticaster multicaster(_threadPoolExecutor, options);

    return multicaster.multicast(hosts, _defaultDbName, _defaultCmd, opCtx, timeout);
}

std::vector<AsyncMulticaster::Reply> AsyncMulticasterTest::runMulticastWithResponses(
    OperationContext* opCtx,
    const std::vector<HostAndPort>& hosts,
    const std::vector<RemoteCommandResponse>& responses,
    Milliseconds timeout,
    AsyncMulticaster::Options options) {

    auto future = stdx::async(stdx::launch::async,
                              [&]() { return runMulticast(opCtx, hosts, timeout, options); });

    processAllNetworkRequests(responses);

    return future.get();
}

void AsyncMulticasterTest::assertCountAndAllSuccessful(
    const std::vector<AsyncMulticaster::Reply>& results,
    const std::vector<HostAndPort>& hosts,
    const std::vector<RemoteCommandResponse>& responses) {
    ASSERT_EQUALS(responses.size(), results.size());
    for (size_t i = 0; i < results.size(); i++) {
        ASSERT_OK(std::get<1>(results[i]).status);
        ASSERT_BSONOBJ_EQ(responses[i].data, std::get<1>(results[i]).data);
    }

    // Results might not be in order, so create a map for verification
    std::map<HostAndPort, RemoteCommandResponse> resultMap;
    for (const auto& [host, response] : results) {
        resultMap.emplace(host, response);
    }
    for (const auto& host : hosts) {
        ASSERT_TRUE(resultMap.contains(host));
        ASSERT_OK(resultMap.at(host).status);
    }
}

void AsyncMulticasterTest::assertContainsError(const std::vector<AsyncMulticaster::Reply>& results,
                                               ErrorCodes::Error expectedCode) {
    bool foundError = std::ranges::any_of(results, [&](const AsyncMulticaster::Reply& result) {
        const auto& [_, response] = result;
        return response.status == expectedCode;
    });
    ASSERT_TRUE(foundError);
}

TEST_F(AsyncMulticasterTest, MulticastToSingleHostSuccessfulResponse) {
    auto opCtx = makeOperationContext();
    auto hosts = makeHostList(1);

    std::vector<RemoteCommandResponse> responses{makeSuccessResponse()};
    auto results = runMulticastWithResponses(opCtx.get(), hosts, responses);
    assertCountAndAllSuccessful(results, hosts, responses);
}

TEST_F(AsyncMulticasterTest, MulticastToMultipleHostsAllSuccessful) {
    auto opCtx = makeOperationContext();
    auto hosts = makeHostList(3);

    std::vector<RemoteCommandResponse> responses(3, makeSuccessResponse());
    auto results = runMulticastWithResponses(opCtx.get(), hosts, responses);
    assertCountAndAllSuccessful(results, hosts, responses);
}

TEST_F(AsyncMulticasterTest, MulticastWithSomeFailedResponses) {
    auto opCtx = makeOperationContext();
    auto hosts = makeHostList(3);

    std::vector<RemoteCommandResponse> responses = {
        RemoteCommandResponse::make_forTest(BSON("ok" << 1 << "result" << "host0"),
                                            Milliseconds(100)),
        RemoteCommandResponse::make_forTest(Status(ErrorCodes::HostUnreachable, "host unreachable"),
                                            Milliseconds(0)),
        RemoteCommandResponse::make_forTest(BSON("ok" << 1 << "result" << "host2"),
                                            Milliseconds(200))};

    auto results = runMulticastWithResponses(opCtx.get(), hosts, responses);
    ASSERT_EQUALS(3U, results.size());
    assertContainsError(results, ErrorCodes::HostUnreachable);
}

TEST_F(AsyncMulticasterTest, MulticastWithLimitedConcurrency) {
    AsyncMulticaster::Options options;
    options.maxConcurrency = 2;  // Limit to 2 concurrent operations

    auto opCtx = makeOperationContext();
    auto hosts = makeHostList(5);

    auto future =
        stdx::async(stdx::launch::async, [&]() { return runMulticast(opCtx.get(), hosts); });

    std::vector<RemoteCommandResponse> responses(
        5, RemoteCommandResponse::make_forTest(BSON("ok" << 1), Milliseconds(100)));

    // Process requests in batches due to concurrency limit
    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // First batch - should have at most 2 requests
        size_t processed = 0;
        while (net->hasReadyRequests() && processed < 2) {
            auto noi = net->getNextReadyRequest();
            net->scheduleResponse(noi, net->now(), responses[processed]);
            processed++;
        }
        net->runReadyNetworkOperations();

        // More requests should become available as the first batch completes
        while (processed < 5) {
            while (net->hasReadyRequests() && processed < 5) {
                auto noi = net->getNextReadyRequest();
                net->scheduleResponse(noi, net->now(), responses[processed]);
                processed++;
            }
            net->runReadyNetworkOperations();
        }
    }

    assertCountAndAllSuccessful(future.get(), hosts, responses);
}

TEST_F(AsyncMulticasterTest, MulticastWithEmptyHostList) {
    auto opCtx = makeOperationContext();
    auto results = runMulticastWithResponses(opCtx.get(), {}, {});
    assertCountAndAllSuccessful(results, {}, {});
}

TEST_F(AsyncMulticasterTest, MulticastWithTimeout) {
    auto opCtx = makeOperationContext();
    auto future = stdx::async(stdx::launch::async, [&]() {
        return runMulticast(opCtx.get(), makeHostList(2), Milliseconds(100));  // Short timeout
    });

    // Don't respond to requests - let them timeout
    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        while (net->getNumReadyRequests() < 2) {
            stdx::this_thread::sleep_for(stdx::chrono::milliseconds(1));
        }

        // Advance time beyond the timeout
        net->advanceTime(net->now() + Milliseconds(200));
        net->runReadyNetworkOperations();
    }

    auto results = future.get();
    ASSERT_EQUALS(2U, results.size());
    assertContainsError(results, ErrorCodes::NetworkInterfaceExceededTimeLimit);
}

TEST_F(AsyncMulticasterTest, MulticastWithCancelledOperation) {
    auto opCtx = makeOperationContext();
    opCtx->markKilled(ErrorCodes::Interrupted);
    ASSERT_THROWS_CODE(runMulticast(opCtx.get(), makeHostList(2)), DBException, 11601);
}

TEST_F(AsyncMulticasterTest, MulticastToSingleHostSuccessfulResponseWithRetry) {
    auto opCtx = makeOperationContext();
    auto hosts = makeHostList(1);
    auto future =
        stdx::async(stdx::launch::async, [&]() { return runMulticast(opCtx.get(), hosts); });

    auto successResponse = {makeSuccessResponse()};

    // First request fails with retryable error
    processAllNetworkRequests({makeRetryableErrorResponse()});
    // Retry succeeds
    processAllNetworkRequests(successResponse);
    assertCountAndAllSuccessful(future.get(), hosts, successResponse);
}

TEST_F(AsyncMulticasterTest, MulticastToMultipleHostSuccessfulResponseWithRetry) {
    auto opCtx = makeOperationContext();
    auto hosts = makeHostList(3);
    auto future =
        stdx::async(stdx::launch::async, [&]() { return runMulticast(opCtx.get(), hosts); });

    auto successResponses = std::vector<RemoteCommandResponse>(3, makeSuccessResponse());

    // First requests fails with retryable error
    processAllNetworkRequests(std::vector<RemoteCommandResponse>(3, makeRetryableErrorResponse()));
    // Retries succeeds
    processAllNetworkRequests(successResponses);
    assertCountAndAllSuccessful(future.get(), hosts, successResponses);
}

TEST_F(AsyncMulticasterTest, MulticastToSingleHostSuccessfulResponseWithMaxRetry) {
    auto opCtx = makeOperationContext();
    auto hosts = makeHostList(1);
    auto future =
        stdx::async(stdx::launch::async, [&]() { return runMulticast(opCtx.get(), hosts); });
    for (int i = 0; i < 4; ++i) {
        processAllNetworkRequests({makeRetryableErrorResponse()});
    }
    assertCountAndAllSuccessful(future.get(), hosts, {makeRetryableErrorResponse()});
}

TEST_F(AsyncMulticasterTest, MulticastToSingleHostSuccessfulResponseWithDelay) {
    auto opCtx = makeOperationContext();
    auto hosts = makeHostList(1);

    const int testBaseBackoffMillis = 200;
    FailPointEnableBlock fp{"setBackoffDelayForTesting",
                            BSON("backoffDelayMs" << testBaseBackoffMillis)};

    auto future =
        stdx::async(stdx::launch::async, [&]() { return runMulticast(opCtx.get(), hosts); });

    // First request fails
    processAllNetworkRequests({makeSystemOverloadedErrorResponse()});
    auto successResponses = {makeSuccessResponse()};
    checkRequestReadyAfterDelay(testBaseBackoffMillis);

    // Second request also fails
    processAllNetworkRequests({makeSystemOverloadedErrorResponse()});
    checkRequestReadyAfterDelay(testBaseBackoffMillis);

    // Retry after delay succeeds
    processAllNetworkRequests(successResponses);
    assertCountAndAllSuccessful(future.get(), hosts, successResponses);
}

TEST_F(AsyncMulticasterTest, MulticastToMultipleHostSuccessfulResponseWithDelay) {
    auto opCtx = makeOperationContext();
    auto hosts = makeHostList(3);

    const int testBaseBackoffMillis = 200;
    FailPointEnableBlock fp{"setBackoffDelayForTesting",
                            BSON("backoffDelayMs" << testBaseBackoffMillis)};

    auto future =
        stdx::async(stdx::launch::async, [&]() { return runMulticast(opCtx.get(), hosts); });

    auto successResponses = std::vector<RemoteCommandResponse>(3, makeSuccessResponse());
    auto errorResponses =
        std::vector<RemoteCommandResponse>(3, makeSystemOverloadedErrorResponse());

    // First request fails
    processAllNetworkRequests(errorResponses);
    checkRequestReadyAfterDelay(testBaseBackoffMillis);

    // Second request also fails
    processAllNetworkRequests(errorResponses);
    checkRequestReadyAfterDelay(testBaseBackoffMillis);

    // Retry after delay succeeds
    processAllNetworkRequests(successResponses);
    assertCountAndAllSuccessful(future.get(), hosts, successResponses);
}

TEST_F(AsyncMulticasterTest, MulticastToMultipleHostSuccessfulResponseWithDelayLimitedConcurrency) {
    AsyncMulticaster::Options options;
    options.maxConcurrency = 2;

    auto opCtx = makeOperationContext();
    auto hosts = makeHostList(3);

    const int testBaseBackoffMillis = 200;
    FailPointEnableBlock fp{"setBackoffDelayForTesting",
                            BSON("backoffDelayMs" << testBaseBackoffMillis)};

    auto future = stdx::async(stdx::launch::async, [&]() {
        return runMulticast(opCtx.get(), hosts, Milliseconds(5000), options);
    });

    auto successResponses = std::vector<RemoteCommandResponse>(3, makeSuccessResponse());

    // Process in batches of 2 due to concurrency limit

    // Batch 1
    processAllNetworkRequests(
        {makeSystemOverloadedErrorResponse(), makeSystemOverloadedErrorResponse()});
    checkRequestReadyAfterDelay(testBaseBackoffMillis);

    processAllNetworkRequests({successResponses[0], successResponses[1]});

    // Batch 2
    processAllNetworkRequests({makeSystemOverloadedErrorResponse()});
    checkRequestReadyAfterDelay(testBaseBackoffMillis);

    processAllNetworkRequests({successResponses[2]});
    assertCountAndAllSuccessful(future.get(), hosts, successResponses);
}

}  // namespace
