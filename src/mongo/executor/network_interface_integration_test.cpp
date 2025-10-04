/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


// IWYU pragma: no_include "cxxabi.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/client/async_client.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/client.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/executor_integration_test_connection_stats.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_integration_fixture.h"
#include "mongo/executor/network_interface_tl.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/idl/generic_argument_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/grpc_connection_stats_gen.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <ostream>
#include <sstream>  // TODO?
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace executor {

// for assertion error messages
// "{.sent=1, .canceled=0, ...}"
std::ostream& operator<<(std::ostream& out, const NetworkInterface::Counters& counters) {
#define PRINT(FIELD) "." #FIELD "=" << counters.FIELD
    return out << "NetworkInterface::Counters{" << PRINT(sent) << ", " << PRINT(canceled) << ", "
               << PRINT(timedOut) << ", " << PRINT(failed) << ", " << PRINT(failedRemotely)
               << ", " PRINT(succeeded) << "}";
#undef PRINT
}

namespace {

bool pingCommandMissing(const RemoteCommandResponse& result) {
    if (result.isOK()) {
        // On mongos, there is no sleep command, so just check that the command failed with
        // a "Command not found" error code
        ASSERT_EQ(result.data["ok"].Double(), 0.0);
        ASSERT_EQ(result.data["code"].Int(), 59);
        return true;
    }

    return false;
}

TEST_F(NetworkInterfaceIntegrationFixture, Ping) {
    startNet();
    assertCommandOK(DatabaseName::kAdmin, BSON("ping" << 1));
}

TEST_F(NetworkInterfaceIntegrationFixture, PingWithoutStartup) {
    createNet();

    RemoteCommandRequest request{fixture().getServers()[0],
                                 DatabaseName::kAdmin,
                                 BSON("ping" << 1),
                                 BSONObj(),
                                 nullptr,
                                 Minutes(5)};

    ASSERT_THROWS_CODE(
        runCommand(makeCallbackHandle(), request), DBException, ErrorCodes::NotYetInitialized);
    net().startup();
    auto fut = runCommand(makeCallbackHandle(), request);
    ASSERT(fut.get(interruptible()).isOK());
}

struct ExpectedCounters {
    boost::optional<uint64_t> sent;
    boost::optional<uint64_t> canceled;
    boost::optional<uint64_t> timedOut;
    boost::optional<uint64_t> failed;
    boost::optional<uint64_t> failedRemotely;
    boost::optional<uint64_t> succeeded;
};

// Compares the non-null fields of `expected` with the corresponding fields of `actual`, for
// assertions.
bool operator==(const NetworkInterface::Counters& actual, const ExpectedCounters& expected) {
#define MATCH(FIELD) (!expected.FIELD.has_value() || actual.FIELD == *expected.FIELD)
    return MATCH(sent) && MATCH(canceled) && MATCH(timedOut) && MATCH(failed) &&
        MATCH(failedRemotely) && MATCH(succeeded);
#undef MATCH
}

// for assertion error messages:
// "{.sent=1, .canceled=0, ...}"
std::ostream& operator<<(std::ostream& out, const ExpectedCounters& expected) {
    bool first = true;
#define PRINT(FIELD)                              \
    if (expected.FIELD.has_value()) {             \
        if (first) {                              \
            first = false;                        \
        } else {                                  \
            out << ", ";                          \
        }                                         \
        out << "." #FIELD "=" << *expected.FIELD; \
    }
    out << "ExpectedCounters{";
    PRINT(sent)
    PRINT(canceled)
    PRINT(timedOut)
    PRINT(failed)
    PRINT(failedRemotely)
    PRINT(succeeded)
    out << "}";
    return out;
#undef PRINT
}

class NetworkInterfaceTest : public NetworkInterfaceIntegrationFixture {
public:
    constexpr static Milliseconds kNoTimeout = RemoteCommandRequest::kNoTimeout;
    constexpr static Milliseconds kMaxWait = Minutes(1);

    void assertNumOps(const ExpectedCounters& expected) {
        ASSERT_EQ(net().getCounters(), expected);
    }

    void setUp() override {
        startNet();
    }

    // NetworkInterfaceIntegrationFixture::tearDown() shuts down the NetworkInterface. We always
    // need to do it even if we have additional tearDown tasks.
    void tearDown() override {
        NetworkInterfaceIntegrationFixture::tearDown();
        ASSERT_EQ(getInProgress(), 0);
    }

    RemoteCommandRequest makeTestCommand(Milliseconds timeout,
                                         BSONObj cmd,
                                         OperationContext* opCtx = nullptr,
                                         bool fireAndForget = false,
                                         boost::optional<ErrorCodes::Error> timeoutCode = {},
                                         boost::optional<UUID> operationKey = {}) {

        auto cs = fixture();
        if (operationKey) {
            cmd = cmd.addField(BSON(GenericArguments::kClientOperationKeyFieldName << *operationKey)
                                   .firstElement());
        }

        RemoteCommandRequest request(cs.getServers().front(),
                                     DatabaseName::kAdmin,
                                     std::move(cmd),
                                     BSONObj(),
                                     opCtx,
                                     timeout,
                                     fireAndForget,
                                     operationKey);
        // Don't override possible opCtx error code.
        if (timeoutCode) {
            request.timeoutCode = timeoutCode;
        }
        return request;
    }

    BSONObj makeEchoCmdObj() {
        return BSON("echo" << 1 << "foo"
                           << "bar");
    }

    BSONObj makeFindCmdObj() {
        return BSON("find" << "test"
                           << "filter" << BSONObj());
    }

    BSONObj makeSleepCmdObj() {
        return BSON("sleep" << 1 << "lock"
                            << "none"
                            << "secs" << 1000000000);
    }

    RemoteCommandResponse runCurrentOpForCommand(HostAndPort target, const std::string command) {
        const auto cmdObj = BSON(
            "aggregate" << 1 << "pipeline"
                        << BSON_ARRAY(BSON("$currentOp" << BSON("localOps" << true))
                                      << BSON("$match" << BSON(("command." + command)
                                                               << BSON("$exists" << true))))
                        << "cursor" << BSONObj() << "$readPreference" << BSON("mode" << "nearest"));
        RemoteCommandRequest request{
            target, DatabaseName::kAdmin, cmdObj, BSONObj(), nullptr, kNoTimeout};
        auto res = runCommandSync(request);
        ASSERT_OK(res.status);
        ASSERT_OK(getStatusFromCommandResult(res.data));
        return res;
    }

    /**
     * Returns true if the given command is still running.
     */
    bool isCommandRunning(const std::string command,
                          boost::optional<HostAndPort> target = boost::none) {
        auto res = runCurrentOpForCommand(target.value_or(fixture().getServers().front()), command);
        return !res.data["cursor"]["firstBatch"].Array().empty();
    }

    /**
     * Repeatedly runs currentOp to check if the given command is running, and blocks until
     * the command starts running or the wait timeout is reached. Asserts that the command
     * is running after the wait and returns the number times of currentOp is run.
     */
    uint64_t waitForCommandToStart(const std::string command, Milliseconds timeout) {
        ClockSource::StopWatch stopwatch;
        uint64_t numCurrentOpRan = 0;
        do {
            sleepmillis(100);
            numCurrentOpRan++;
        } while (!isCommandRunning(command) && stopwatch.elapsed() < timeout);

        ASSERT_TRUE(isCommandRunning(command));
        return ++numCurrentOpRan;
    }

    /**
     * Repeatedly runs currentOp to check if the given command is running, and blocks until
     * the command finishes running or the wait timeout is reached. Asserts that the command
     * is no longer running after the wait and returns the number times of currentOp is run.
     */
    uint64_t waitForCommandToStop(const std::string command, Milliseconds timeout) {
        ClockSource::StopWatch stopwatch;
        uint64_t numCurrentOpRan = 0;
        do {
            sleepmillis(100);
            numCurrentOpRan++;
        } while (isCommandRunning(command) && stopwatch.elapsed() < timeout);

        ASSERT_FALSE(isCommandRunning(command));
        return ++numCurrentOpRan;
    }

    const AsyncClientFactory& getFactory() {
        return checked_cast<NetworkInterfaceTL&>(net()).getClientFactory_forTest();
    }

    void runAcquireConnectionTimeoutTest(boost::optional<ErrorCodes::Error> customCode);

    // Test case definitions.
    void testCancelMissingOperation();
    void testCancelLocally();
    void testCancelRemotely();
    void testCancelRemotelyTimedOut();
    void testCancelBeforeConnection();
    void testLateCancel();
    void testConnectionErrorDropsSingleConnection();
    void testTimeoutDuringConnectionHandshake();
    void testTimeoutWaitingToAcquireConnection();
    void testTimeoutGeneralNetworkInterface();
    void testCustomCodeRequestTimeoutHit();
    void testCustomCodeTimeoutWaitingToAcquireConnection();
    void testNoCustomCodeRequestTimeoutHit();
    void testAsyncOpTimeout();
    void testAsyncOpTimeoutWithOpCtxDeadlineSooner();
    void testAsyncOpTimeoutWithOpCtxDeadlineLater();
    void testStartCommand();
    void testFireAndForget();
    void testUseOperationKeyWhenProvided();
    void testHelloRequestMissingInternalClientInfoWhenNotInternalClient();
    void testTearDownWaitsForInProgress();
    void testRunCommandOnLeasedStream();
    void testConnectionErrorAssociatedWithRemote();
    void testShutdownBeforeSendRequest();
    void testShutdownAfterSendRequest();
};

class NetworkInterfaceTestWithBaton : public NetworkInterfaceTest {
public:
    void setUp() override {
        NetworkInterfaceTest::setUp();
        _serviceContext = ServiceContext::make();
        _client = _serviceContext->getService()->makeClient("BatonClient");
        _opCtx = _client->makeOperationContext();
        _baton = _opCtx->getBaton();
    }

    BatonHandle baton() override {
        return _baton;
    }

    Interruptible* interruptible() override {
        return _opCtx.get();
    }

private:
    ServiceContext::UniqueServiceContext _serviceContext;
    ServiceContext::UniqueClient _client;
    ServiceContext::UniqueOperationContext _opCtx;
    BatonHandle _baton;
};

using NetworkInterfaceTestWithoutBaton = NetworkInterfaceTest;

#define SKIP_ON_GRPC(reason)                                                             \
    if (unittest::shouldUseGRPCEgress()) {                                               \
        LOGV2(9936111, "Skipping test not supported with gRPC", "reason"_attr = reason); \
        return;                                                                          \
    }

#define TEST_WITH_AND_WITHOUT_BATON_F(suite, name) \
    TEST_F(suite##WithBaton, name) {               \
        test##name();                              \
    }                                              \
    TEST_F(suite##WithoutBaton, name) {            \
        test##name();                              \
    }                                              \
    void suite::test##name()


TEST_WITH_AND_WITHOUT_BATON_F(NetworkInterfaceTest, CancelMissingOperation) {
    // This is just a sanity check, this action should have no effect.
    cancelCommand(makeCallbackHandle());
    assertNumOps({.canceled = 0u, .timedOut = 0u, .failed = 0u, .succeeded = 0u});
}

TEST_WITH_AND_WITHOUT_BATON_F(NetworkInterfaceTest, CancelLocally) {
    stdx::thread runCommandThread;
    ON_BLOCK_EXIT([&] { runCommandThread.join(); });
    auto cbh = makeCallbackHandle();
    CancellationSource cancellationSource;

    auto deferred = [&] {
        // Kick off our operation
        FailPointEnableBlock fpb("networkInterfaceHangCommandsAfterAcquireConn");

        // Must create a request without an operation key so that a killOperations command is not
        // sent in the NITL as a result of a failed attempt to send the request. Sending a
        // killOperations may fail some assertions in assertNumOps and is only expected to be sent
        // in the CancelRemotely test below.
        RemoteCommandRequest request(
            fixture().getServers().front(), DatabaseName::kAdmin, makeEchoCmdObj(), nullptr);

        auto [promise, future] = makePromiseFuture<RemoteCommandResponse>();
        runCommandThread = stdx::thread(
            [this, cbh, request, promise = std::move(promise), cancellationSource]() mutable {
                promise.setFrom(runCommand(cbh, request, cancellationSource.token())
                                    .getNoThrow(interruptible()));
            });

        fpb->waitForTimesEntered(fpb.initialTimesEntered() + 1);

        cancellationSource.cancel();

        return std::move(future);
    }();

    // Wait for op to complete, assert that it was canceled.
    auto result = deferred.get();
    ASSERT_EQ(ErrorCodes::CallbackCanceled, result.status);
    ASSERT(result.elapsed);

    assertNumOps({.canceled = 1u, .timedOut = 0u, .failed = 0u, .succeeded = 0u});
}

TEST_WITH_AND_WITHOUT_BATON_F(NetworkInterfaceTest, CancelRemotely) {
    // Enable blockConnection for "echo".
    FailPointEnableBlock fpb("increaseTimeoutOnKillOp");
    assertCommandOK(DatabaseName::kAdmin,
                    BSON("configureFailPoint"
                         << "failCommand"
                         << "mode"
                         << "alwaysOn"
                         << "data"
                         << BSON("blockConnection" << true << "blockTimeMS" << 1000000000
                                                   << "failCommands" << BSON_ARRAY("echo"))),
                    kNoTimeout);

    ON_BLOCK_EXIT([&] {
        // Disable blockConnection.
        assertCommandOK(DatabaseName::kAdmin,
                        BSON("configureFailPoint" << "failCommand"
                                                  << "mode"
                                                  << "off"),
                        kNoTimeout);
    });

    int numCurrentOpRan = 0;

    auto cbh = makeCallbackHandle();
    CancellationSource cancellationSource;
    auto deferred = [&] {
        // Kick off an "echo" operation, which should block until cancelCommand causes
        // the operation to be killed.
        auto deferred = runCommand(cbh,
                                   makeTestCommand(kNoTimeout,
                                                   makeEchoCmdObj(),
                                                   /*opCtx=*/nullptr,
                                                   /*fireAndForget=*/false,
                                                   /*timeoutCode=*/{},
                                                   /*operationKey=*/UUID::gen()),
                                   cancellationSource.token());

        // Wait for the "echo" operation to start.
        numCurrentOpRan += waitForCommandToStart("echo", kMaxWait);

        // Kill the above operation.
        cancellationSource.cancel();

        return deferred;
    }();

    // Wait for the command to return, assert that it was canceled.
    auto result = deferred.get(interruptible());
    ASSERT_EQ(ErrorCodes::CallbackCanceled, result.status);
    ASSERT(result.elapsed);

    // Wait for the operation to be killed on the remote host.
    numCurrentOpRan += waitForCommandToStop("echo", kMaxWait);

    // We have one canceled operation (echo), and two other succeeded operations
    // on top of the currentOp operations (configureFailPoint and _killOperations).
    assertNumOps({.canceled = 1u, .timedOut = 0u, .failed = 0u, .succeeded = 2u + numCurrentOpRan});
}

TEST_WITH_AND_WITHOUT_BATON_F(NetworkInterfaceTest, CancelRemotelyTimedOut) {
    // Enable blockConnection for "echo" and "_killOperations".
    assertCommandOK(DatabaseName::kAdmin,
                    BSON("configureFailPoint"
                         << "failCommand"
                         << "mode"
                         << "alwaysOn"
                         << "data"
                         << BSON("blockConnection"
                                 << true << "blockTimeMS"
                                 << NetworkInterfaceTL::kCancelCommandTimeout.count() + 4000
                                 << "failCommands" << BSON_ARRAY("echo" << "_killOperations"))),
                    kNoTimeout);

    ON_BLOCK_EXIT([&] {
        // Disable blockConnection.
        assertCommandOK(DatabaseName::kAdmin,
                        BSON("configureFailPoint" << "failCommand"
                                                  << "mode"
                                                  << "off"),
                        kNoTimeout);
    });

    int numCurrentOpRan = 0;

    auto cbh = makeCallbackHandle();
    CancellationSource cancellationSource;
    auto deferred = [&] {
        // Kick off a blocking "echo" operation.
        auto deferred = runCommand(cbh,
                                   makeTestCommand(kNoTimeout,
                                                   makeEchoCmdObj(),
                                                   /*opCtx=*/nullptr,
                                                   /*fireAndForget=*/false,
                                                   /*timeoutCode=*/{},
                                                   /*operationKey=*/UUID::gen()),
                                   cancellationSource.token());

        // Wait for the "echo" operation to start.
        numCurrentOpRan += waitForCommandToStart("echo", kMaxWait);

        // Run cancelCommand to kill the above operation. _killOperations is expected to block and
        // time out, and to be canceled by the command timer.
        FailPointEnableBlock cmdFailedFpb("networkInterfaceCommandsFailedWithErrorCode",
                                          BSON("cmdNames"
                                               << BSON_ARRAY("_killOperations") << "errorCode"
                                               << ErrorCodes::NetworkInterfaceExceededTimeLimit));

        cancellationSource.cancel();

        // Wait for _killOperations for 'echo' to time out.
        cmdFailedFpb->waitForTimesEntered(interruptible(), cmdFailedFpb.initialTimesEntered() + 1);

        return deferred;
    }();

    // Wait for the command to return, assert that it was canceled.
    auto result = deferred.get(interruptible());
    ASSERT_EQ(ErrorCodes::CallbackCanceled, result.status);
    ASSERT(result.elapsed);

    // We have one canceled operation (echo), one timedout operation (_killOperations),
    // and one succeeded operation on top of the currentOp operations (configureFailPoint).
    assertNumOps({.canceled = 1u, .timedOut = 1u, .failed = 0u, .succeeded = 1u + numCurrentOpRan});
}

TEST_WITH_AND_WITHOUT_BATON_F(NetworkInterfaceTest, CancelBeforeConnection) {
    // Covered in MockGRPCAsyncClientFactoryTest::CancelStreamEstablishment.
    SKIP_ON_GRPC("ConnectionPool-specific failpoint");

    boost::optional<FailPointEnableBlock> fpb("connectionPoolDoesNotFulfillRequests");
    auto cbh = makeCallbackHandle();
    CancellationSource cancellationSource;

    auto pf = makePromiseFuture<RemoteCommandResponse>();
    auto cmdThread = stdx::thread([&] {
        pf.promise.setFrom(
            runCommand(cbh, makeTestCommand(kMaxWait, makeEchoCmdObj()), cancellationSource.token())
                .getNoThrow(interruptible()));
    });
    ON_BLOCK_EXIT([&] { cmdThread.join(); });

    fpb.get()->waitForTimesEntered(fpb->initialTimesEntered() + 1);
    cancellationSource.cancel();
    fpb.reset();

    ASSERT_EQ(net().getCounters().sent, 0);

    // Wait for op to complete, assert that it was canceled.
    auto result = pf.future.get();
    ASSERT_EQ(ErrorCodes::CallbackCanceled, result.status);
    ASSERT(result.elapsed);
    assertNumOps({.canceled = 1u, .timedOut = 0u, .failed = 0u, .succeeded = 0u});
}

TEST_WITH_AND_WITHOUT_BATON_F(NetworkInterfaceTest, LateCancel) {
    auto cbh = makeCallbackHandle();
    CancellationSource cancellationSource;

    auto deferred =
        runCommand(cbh, makeTestCommand(kMaxWait, makeEchoCmdObj()), cancellationSource.token());

    // Wait for op to complete, assert that it was not canceled.
    auto result = deferred.get(interruptible());
    cancellationSource.cancel();

    ASSERT_OK(result.status);
    ASSERT(result.elapsed);
    assertNumOps({.canceled = 0u, .timedOut = 0u, .failed = 0u, .succeeded = 1u});
}

TEST_WITH_AND_WITHOUT_BATON_F(NetworkInterfaceTest, ConnectionErrorDropsSingleConnection) {
    SKIP_ON_GRPC("gRPC doesn't use ConnectionPool");

    FailPoint* failPoint = globalFailPointRegistry().find("asyncConnectReturnsConnectionError");
    auto timesEntered = failPoint->setMode(FailPoint::nTimes, 1);

    auto cbh = makeCallbackHandle();
    auto deferred = runCommand(cbh, makeTestCommand(kMaxWait, makeEchoCmdObj()));
    // Wait for one of the connection attempts to fail with a `ConnectionError`.
    failPoint->waitForTimesEntered(interruptible(), timesEntered + 1);
    auto result = deferred.get(interruptible());

    ASSERT_OK(result.status);
    ConnectionPoolStats stats;
    net().appendConnectionStats(&stats);

    ASSERT_EQ(stats.totalCreated, 2);
    ASSERT_EQ(stats.totalInUse + stats.totalAvailable + stats.totalRefreshing, 1);
    // Connection dropped during finishRefresh, so the dropped connection still
    // counts toward the refreshed counter.
    ASSERT_EQ(stats.totalRefreshed, 2);
}

TEST_WITH_AND_WITHOUT_BATON_F(NetworkInterfaceTest, TimeoutDuringConnectionHandshake) {
    SKIP_ON_GRPC("gRPC skips the handshake");

    // If network timeout occurs during connection setup before handshake completes,
    // HostUnreachable should be returned.
    FailPointEnableBlock fpb1(
        "connectionPoolDropConnectionsBeforeGetConnection",
        BSON("instance" << "NetworkInterfaceTL-NetworkInterfaceIntegrationFixture"));
    FailPointEnableBlock fpb2(
        "triggerConnectionSetupHandshakeTimeout",
        BSON("instance" << "NetworkInterfaceTL-NetworkInterfaceIntegrationFixture"));
    auto cbh = makeCallbackHandle();
    auto deferred = runCommand(cbh, makeTestCommand(Milliseconds(100), makeEchoCmdObj()));

    auto result = deferred.get(interruptible());

    ASSERT_EQ(ErrorCodes::HostUnreachable, result.status);
    // No timeouts are counted as a result of HostUnreachable being returned.
    assertNumOps({.canceled = 0u, .timedOut = 0u, .failed = 1u, .succeeded = 0u});
}

void NetworkInterfaceTest::runAcquireConnectionTimeoutTest(
    boost::optional<ErrorCodes::Error> customCode) {
    // Covered in MockGRPCAsyncClientFactoryTest::runStreamEstablishmentTimeoutTest.
    SKIP_ON_GRPC("ConnectionPool-specific failpoint");

    // If timeout occurs during connection acquisition, PooledConnectionAcquisitionExceededTimeLimit
    // should be returned.
    FailPointEnableBlock fpb("connectionPoolDoesNotFulfillRequests");
    auto cbh = makeCallbackHandle();
    auto request = makeTestCommand(Milliseconds(100), makeFindCmdObj());
    request.timeoutCode = customCode;
    auto deferred = runCommand(cbh, request);

    auto result = deferred.get(interruptible());

    auto expectedCode =
        customCode.value_or(ErrorCodes::PooledConnectionAcquisitionExceededTimeLimit);
    ASSERT_EQ(expectedCode, result.status);
    assertNumOps({.canceled = 0u, .timedOut = 1u, .failed = 0u, .succeeded = 0u});
}

TEST_WITH_AND_WITHOUT_BATON_F(NetworkInterfaceTest, TimeoutWaitingToAcquireConnection) {
    runAcquireConnectionTimeoutTest({});
}

TEST_WITH_AND_WITHOUT_BATON_F(NetworkInterfaceTest, CustomCodeTimeoutWaitingToAcquireConnection) {
    runAcquireConnectionTimeoutTest(ErrorCodes::MaxTimeMSExpired);
}

TEST_WITH_AND_WITHOUT_BATON_F(NetworkInterfaceTest, TimeoutGeneralNetworkInterface) {
    // Run a command to populate the connection pool.
    assertCommandOK(DatabaseName::kAdmin, BSON("ping" << 1));

    auto fpGuard = configureFailCommand("ping", {}, Milliseconds(30000));

    auto cbh = makeCallbackHandle();
    auto deferred = runCommand(cbh, makeTestCommand(Milliseconds(100), BSON("ping" << 1)));

    auto result = deferred.get(interruptible());

    ASSERT_EQ(ErrorCodes::NetworkInterfaceExceededTimeLimit, result.status);
    assertNumOps({.canceled = 0u, .timedOut = 1u, .failed = 0u, .succeeded = 1u});
}

/**
 * Test that if a custom timeout code is passed into the request, then timeouts errors will
 * expect the request's error code.
 */
TEST_WITH_AND_WITHOUT_BATON_F(NetworkInterfaceTest, CustomCodeRequestTimeoutHit) {
    auto fpGuard = configureFailCommand("ping", {}, Milliseconds(30000));

    auto cb = makeCallbackHandle();
    // Force timeout by setting timeout to 0.
    auto request = makeTestCommand(
        Milliseconds(0), BSON("ping" << 1), nullptr, false, ErrorCodes::MaxTimeMSExpired);
    auto deferred = runCommand(cb, request);
    auto res = deferred.get(interruptible());

    ASSERT(!res.isOK());
    ASSERT_EQ(res.status.code(), ErrorCodes::MaxTimeMSExpired);
}

/**
 * Test that if no custom timeout code is passed into the request, then timeouts errors will
 * expect default error codes depending on location.
 */
TEST_WITH_AND_WITHOUT_BATON_F(NetworkInterfaceTest, NoCustomCodeRequestTimeoutHit) {
    // Covered in MockGRPCAsyncClientFactoryTest::TimeoutStreamEstablishment.
    SKIP_ON_GRPC("ConnectionPool-specific error code");

    auto cb = makeCallbackHandle();
    // Force timeout by setting timeout to 0.
    auto request = makeTestCommand(Milliseconds(0), makeFindCmdObj());
    auto deferred = runCommand(cb, request);
    auto res = deferred.get(interruptible());

    ASSERT(!res.isOK());
    ASSERT_EQ(res.status.code(), ErrorCodes::PooledConnectionAcquisitionExceededTimeLimit);
}

TEST_WITH_AND_WITHOUT_BATON_F(NetworkInterfaceTest, AsyncOpTimeout) {
    auto fpGuard = configureFailCommand("ping", {}, Milliseconds(30'000));

    // Kick off operation
    auto cb = makeCallbackHandle();
    auto request = makeTestCommand(
        Milliseconds{3'000}, BSON("ping" << 1), nullptr, false, ErrorCodes::MaxTimeMSExpired);
    auto deferred = runCommand(cb, request);

    auto result = deferred.get(interruptible());
    ASSERT_EQ(ErrorCodes::MaxTimeMSExpired, result.status);
    ASSERT(result.elapsed);
    ASSERT_EQ(result.target, fixture().getServers().front());
    // The "ping" will have timed out on the client side, and then because of that a
    // "_killOperations" command will be sent to the server, and that will succeed.
    assertNumOps({.canceled = 0u, .timedOut = 1u, .failed = 0u, .succeeded = 0u});
}

TEST_WITH_AND_WITHOUT_BATON_F(NetworkInterfaceTest, AsyncOpTimeoutWithOpCtxDeadlineSooner) {
    // Kick off operation
    auto cb = makeCallbackHandle();

    constexpr auto opCtxDeadline = Milliseconds{600};
    constexpr auto requestTimeout = Milliseconds{1000};

    auto serviceContext = ServiceContext::make();
    auto client = serviceContext->getService()->makeClient("NetworkClient");
    auto opCtx = client->makeOperationContext();

    auto stopWatch = serviceContext->getPreciseClockSource()->makeStopWatch();
    opCtx->setDeadlineByDate(stopWatch.start() + opCtxDeadline, ErrorCodes::ExceededTimeLimit);

    auto request = makeTestCommand(requestTimeout, makeSleepCmdObj(), opCtx.get());

    auto deferred = runCommand(cb, request);
    // The time returned in result.elapsed is measured from when the command started, which happens
    // in runCommand. The delay between setting the deadline on opCtx and starting the command can
    // be long enough that the assertion about opCtxDeadline fails.
    auto networkStartCommandDelay = stopWatch.elapsed();

    auto result = deferred.get(interruptible());

    // mongos doesn't implement the ping command, so ignore the response there, otherwise
    // check that we've timed out.
    if (pingCommandMissing(result)) {
        return;
    }

    ASSERT_EQ(ErrorCodes::ExceededTimeLimit, result.status);
    ASSERT(result.elapsed);

    // check that the request timeout uses the smaller of the operation context deadline and
    // the timeout specified in the request constructor.
    ASSERT_GTE(result.elapsed.value() + networkStartCommandDelay, opCtxDeadline);
    ASSERT_LT(result.elapsed.value(), requestTimeout);
    ASSERT_EQ(result.target, fixture().getServers().front());

    // Sleep has timed out but _killOperations may still be running. We can't use
    // waitForCommandToStop since there is no guarantee when _killOperations starts.
    assertNumOps({.canceled = 0u, .timedOut = 1u, .failed = 0u, .succeeded = 0u});
}

TEST_WITH_AND_WITHOUT_BATON_F(NetworkInterfaceTest, AsyncOpTimeoutWithOpCtxDeadlineLater) {
    // Kick off operation
    auto cb = makeCallbackHandle();

    constexpr auto opCtxDeadline = Milliseconds{1000};
    constexpr auto requestTimeout = Milliseconds{600};

    auto serviceContext = ServiceContext::make();
    auto client = serviceContext->getService()->makeClient("NetworkClient");
    auto opCtx = client->makeOperationContext();

    auto stopWatch = serviceContext->getPreciseClockSource()->makeStopWatch();
    opCtx->setDeadlineByDate(stopWatch.start() + opCtxDeadline, ErrorCodes::ExceededTimeLimit);

    auto request = makeTestCommand(
        requestTimeout, makeSleepCmdObj(), opCtx.get(), false, ErrorCodes::MaxTimeMSExpired);

    auto deferred = runCommand(cb, request);
    // The time returned in result.elapsed is measured from when the command started, which happens
    // in runCommand. The delay between setting the deadline on opCtx and starting the command can
    // be long enough that the assertion about opCtxDeadline fails.
    auto networkStartCommandDelay = stopWatch.elapsed();

    auto result = deferred.get(interruptible());

    // mongos doesn't implement the ping command, so ignore the response there, otherwise
    // check that we've timed out.
    if (pingCommandMissing(result)) {
        return;
    }

    ASSERT_EQ(ErrorCodes::MaxTimeMSExpired, result.status);
    ASSERT(result.elapsed);

    // check that the request timeout uses the smaller of the operation context deadline and
    // the timeout specified in the request constructor.
    ASSERT_GTE(duration_cast<Milliseconds>(result.elapsed.value()), requestTimeout);
    ASSERT_LT(duration_cast<Milliseconds>(result.elapsed.value() + networkStartCommandDelay),
              opCtxDeadline);

    // Sleep has timed out but _killOperations may still be running. We can't use
    // waitForCommandToStop since there is no guarantee when _killOperations starts.
    assertNumOps({.canceled = 0u, .timedOut = 1u, .failed = 0u, .succeeded = 0u});
}

TEST_WITH_AND_WITHOUT_BATON_F(NetworkInterfaceTest, StartCommand) {
    auto request = makeTestCommand(kNoTimeout,
                                   makeEchoCmdObj(),
                                   /*opCtx=*/nullptr,
                                   /*fireAndForget=*/false,
                                   /*timeoutCode=*/{},
                                   /*operationKey=*/UUID::gen());

    auto deferred = runCommand(makeCallbackHandle(), std::move(request));

    auto res = deferred.get(interruptible());

    ASSERT(res.elapsed);
    uassertStatusOK(res.status);

    // This opmsg request expect the following reply, which is generated below
    // { echo: { echo: 1, foo: "bar", clientOperationKey: uuid, $db: "admin" }, ok: 1.0 }
    auto cmdObj = res.data.getObjectField("echo");
    ASSERT_EQ(1, cmdObj.getIntField("echo"));
    ASSERT_EQ("bar"_sd, cmdObj.getStringField("foo"));
    ASSERT_EQ("admin"_sd, cmdObj.getStringField("$db"));
    ASSERT_FALSE(cmdObj["clientOperationKey"].eoo());
    ASSERT_EQ(1, res.data.getIntField("ok"));
    assertNumOps({.canceled = 0u, .timedOut = 0u, .failed = 0u, .succeeded = 1u});
}

TEST_WITH_AND_WITHOUT_BATON_F(NetworkInterfaceTest, FireAndForget) {
    assertCommandOK(DatabaseName::kAdmin,
                    BSON("configureFailPoint"
                         << "failCommand"
                         << "mode"
                         << "alwaysOn"
                         << "data"
                         << BSON("errorCode" << ErrorCodes::CommandFailed << "failCommands"
                                             << BSON_ARRAY("echo"))));

    ON_BLOCK_EXIT([&] {
        assertCommandOK(DatabaseName::kAdmin,
                        BSON("configureFailPoint" << "failCommand"
                                                  << "mode"
                                                  << "off"));
    });

    // Run fireAndForget commands and verify that we get status OK responses.
    const int numFireAndForgetRequests = 3;
    std::vector<Future<RemoteCommandResponse>> futures;

    for (int i = 0; i < numFireAndForgetRequests; i++) {
        auto cbh = makeCallbackHandle();
        auto fireAndForgetRequest = makeTestCommand(
            kNoTimeout, makeEchoCmdObj(), nullptr /* opCtx */, true /* fireAndForget */);
        futures.push_back(runCommand(cbh, fireAndForgetRequest));
    }

    for (auto& future : futures) {
        auto result = future.get(interruptible());
        ASSERT(result.elapsed);
        uassertStatusOK(result.status);
        ASSERT_EQ(1, result.data.getIntField("ok"));
    }

    // Run a non-fireAndForget command and verify that we get a CommandFailed response.
    auto nonFireAndForgetRequest = makeTestCommand(kNoTimeout, makeEchoCmdObj());
    auto result = runCommandSync(nonFireAndForgetRequest);
    ASSERT(result.elapsed);
    uassertStatusOK(result.status);
    ASSERT_EQ(0, result.data.getIntField("ok"));
    ASSERT_EQ(ErrorCodes::CommandFailed, result.data.getIntField("code"));
    assertNumOps({.canceled = 0u, .timedOut = 0u, .failed = 0u, .succeeded = 5u});
}

TEST_F(NetworkInterfaceTest, SetAlarm) {
    // Set alarm, wait for fulfillment.
    {
        Date_t expiration = net().now() + Milliseconds(100);
        net().setAlarm(expiration).get();
        ASSERT(net().now() >= expiration);
    }

    // Set alarm with passed deadline.
    {
        Date_t expiration = net().now() - Milliseconds(100);
        auto future = net().setAlarm(expiration);
        auto now = net().now();
        future.get();
        ASSERT(net().now() <= now + Milliseconds(10));
    }

    // Set alarm, shutdown, then check that future finishes as though it were cancelled.
    {
        Date_t expiration = net().now() + Milliseconds(99999999);
        auto future = net().setAlarm(expiration);
        ASSERT(!future.isReady());

        net().shutdown();
        ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::CallbackCanceled);
        startNet();
    }

    // Set alarm with cancelled token.
    {
        Date_t expiration = net().now() + Milliseconds(99999999);
        CancellationSource source;
        source.cancel();
        auto future = net().setAlarm(expiration, source.token());
        ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::CallbackCanceled);
        ASSERT(net().now() < expiration);
    }

    // Set alarm, then cancel.
    {
        Date_t expiration = net().now() + Milliseconds(9999999);
        CancellationSource source;
        auto future = net().setAlarm(expiration, source.token());
        ASSERT(!future.isReady());
        source.cancel();
        ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::CallbackCanceled);
        ASSERT(net().now() < expiration);
    }

    // Set alarm, then dismiss source, and check that deadline is respected.
    {
        Date_t expiration = net().now() + Milliseconds(100);
        CancellationSource source;
        auto future = net().setAlarm(expiration, source.token());
        ASSERT(!future.isReady());
        source = {};
        ASSERT(!future.isReady());
        ASSERT(net().now() < expiration);
        future.get();
        ASSERT(net().now() >= expiration);
    }

    // Set alarm, cancel after deadline.
    {
        Date_t expiration = net().now() + Milliseconds(100);
        CancellationSource source;
        net().setAlarm(expiration, source.token()).get();
        ASSERT(net().now() >= expiration);
        source.cancel();
    }

    // Set alarm, cancel after destruction.
    {
        Date_t expiration = net().now() + Milliseconds(100);
        CancellationSource source;
        net().setAlarm(expiration, source.token()).get();
        ASSERT(net().now() >= expiration);
        net().shutdown();
        startNet();
        source.cancel();
    }

    // Set alarm after shutdown.
    {
        Date_t expiration = net().now() + Seconds(60);
        net().shutdown();
        ASSERT_THROWS_CODE(
            net().setAlarm(expiration).get(), DBException, ErrorCodes::ShutdownInProgress);
    }
}

TEST_WITH_AND_WITHOUT_BATON_F(NetworkInterfaceTest, UseOperationKeyWhenProvided) {
    const auto opKey = UUID::gen();
    assertCommandOK(DatabaseName::kAdmin,
                    BSON("configureFailPoint" << "failIfOperationKeyMismatch"
                                              << "mode"
                                              << "alwaysOn"
                                              << "data" << BSON("clientOperationKey" << opKey)),
                    kNoTimeout);

    ON_BLOCK_EXIT([&] {
        assertCommandOK(DatabaseName::kAdmin,
                        BSON("configureFailPoint" << "failIfOperationKeyMismatch"
                                                  << "mode"
                                                  << "off"),
                        kNoTimeout);
    });

    RemoteCommandRequest rcr(fixture().getServers().front(),
                             DatabaseName::kAdmin,
                             makeEchoCmdObj(),
                             BSONObj(),
                             nullptr,
                             kNoTimeout,
                             false,
                             opKey);
    resetIsInternalClient(true);
    ON_BLOCK_EXIT([&] { resetIsInternalClient(false); });
    auto cbh = makeCallbackHandle();
    auto fut = runCommand(cbh, std::move(rcr));
    fut.get(interruptible());
}

TEST_F(NetworkInterfaceTest, StartExhaustCommandShouldReceiveMultipleResponses) {
    CancellationSource cancelSource;
    auto isMasterCmd = BSON("isMaster" << 1 << "maxAwaitTimeMS" << 1000 << "topologyVersion"
                                       << TopologyVersion(OID::max(), 0).toBSON());

    auto request = makeTestCommand(kNoTimeout, isMasterCmd);
    auto cbh = makeCallbackHandle();
    auto reader = net().startExhaustCommand(cbh, request, nullptr, cancelSource.token()).get();

    // The server sends a response either when a topology change occurs or when it has not sent
    // a response in 'maxAwaitTimeMS'. In this case we expect a response every 'maxAwaitTimeMS'
    // = 1000 (set in the isMaster cmd above)
    assertOK(reader->next().getNoThrow());
    assertOK(reader->next().getNoThrow());

    cancelSource.cancel();
    auto swResp = reader->next().getNoThrow();
    ASSERT_OK(swResp);
    ASSERT_EQ(swResp.getValue().status, ErrorCodes::CallbackCanceled);
}

TEST_F(NetworkInterfaceTest, StartExhaustCommandShouldStopOnFailure) {
    // Both assetCommandOK and makeTestCommand target the first host in the connection string, so we
    // are guaranteed that the failpoint is set on the same host that we run the exhaust command on.
    auto configureFailpointCmd =
        BSON("configureFailPoint" << "failCommand"
                                  << "mode"
                                  << "alwaysOn"
                                  << "data"
                                  << BSON("errorCode" << ErrorCodes::CommandFailed << "failCommands"
                                                      << BSON_ARRAY("isMaster")));
    assertCommandOK(DatabaseName::kAdmin, configureFailpointCmd);

    ON_BLOCK_EXIT([&] {
        auto stopFpRequest = BSON("configureFailPoint" << "failCommand"
                                                       << "mode"
                                                       << "off");
        assertCommandOK(DatabaseName::kAdmin, stopFpRequest);
    });

    auto isMasterCmd = BSON("isMaster" << 1 << "maxAwaitTimeMS" << 1000 << "topologyVersion"
                                       << TopologyVersion(OID::max(), 0).toBSON());

    auto request = makeTestCommand(kNoTimeout, isMasterCmd);
    auto cbh = makeCallbackHandle();

    auto reader = net().startExhaustCommand(cbh, request).get();
    auto failedResp = reader->next().get();
    ASSERT_EQ(getStatusFromCommandResult(failedResp.data), ErrorCodes::CommandFailed);
    ASSERT_FALSE(failedResp.moreToCome);

    ASSERT_EQ(reader->next().get().status, ErrorCodes::ExhaustCommandFinished);
}

TEST_WITH_AND_WITHOUT_BATON_F(NetworkInterfaceTest, TearDownWaitsForInProgress) {
    stdx::thread runCommandThread;
    stdx::thread tearDownThread;
    auto tearDownPF = makePromiseFuture<void>();

    auto deferred = [&] {
        // Enable failpoint to make sure tearDown is blocked
        FailPointEnableBlock fpb("networkInterfaceFixtureHangOnCompletion");

        auto [promise, future] = makePromiseFuture<RemoteCommandResponse>();
        runCommandThread = stdx::thread([this, promise = std::move(promise)]() mutable {
            promise.setFrom(
                runCommand(makeCallbackHandle(), makeTestCommand(kMaxWait, makeEchoCmdObj()))
                    .getNoThrow(interruptible()));
        });

        // Wait for the completion of the command
        fpb->waitForTimesEntered(fpb.initialTimesEntered() + 1);

        tearDownThread = stdx::thread([this, promise = std::move(tearDownPF.promise)]() mutable {
            tearDown();
            promise.setWith([] {});
        });

        // Arbitrary delay between spawning the tearDown thread and checking futures
        // to increase the chance of failures if tearDown doesn't wait for
        // in-progress commands.
        sleepFor(Milliseconds(50));

        ASSERT_EQ(getInProgress(), 1);
        ASSERT_FALSE(future.isReady()) << "Expected the command to be blocked";
        ASSERT_FALSE(tearDownPF.future.isReady())
            << "Expected tearDown to wait for blocked command";

        return std::move(future);
    }();

    tearDownThread.join();
    runCommandThread.join();

    ASSERT(deferred.isReady());
    ASSERT(tearDownPF.future.isReady());
    ASSERT_EQ(getInProgress(), 0);
}

TEST_WITH_AND_WITHOUT_BATON_F(NetworkInterfaceTest, RunCommandOnLeasedStream) {
    auto cs = fixture();
    auto target = cs.getServers().front();
    auto leasedStream = net().leaseStream(target, transport::kGlobalSSLMode, kNoTimeout).get();
    auto* client = leasedStream->getClient();

    auto request =
        RemoteCommandRequest(target, DatabaseName::kAdmin, makeEchoCmdObj(), nullptr, kNoTimeout);
    auto deferred = client->runCommandRequest(request);

    auto res = deferred.get(interruptible());

    ASSERT(res.elapsed);
    uassertStatusOK(res.status);
    leasedStream->indicateSuccess();
    leasedStream->indicateUsed();

    // This opmsg request expect the following reply, which is generated below
    // { echo: { echo: 1, foo: "bar", $db: "admin" }, ok: 1.0 }
    auto cmdObj = res.data.getObjectField("echo");
    ASSERT_EQ(1, cmdObj.getIntField("echo"));
    ASSERT_EQ("bar"_sd, cmdObj.getStringField("foo"));
    ASSERT_EQ("admin"_sd, cmdObj.getStringField("$db"));
    ASSERT_EQ(1, res.data.getIntField("ok"));
}

TEST_WITH_AND_WITHOUT_BATON_F(NetworkInterfaceTest, ConnectionErrorAssociatedWithRemote) {
    // Covered in MockGRPCAsyncClientFactoryTest::ConnectionErrorAssociatedWithRemote.
    SKIP_ON_GRPC("ConnectionPool-specific failpoint");

    FailPointEnableBlock fpb("connectionPoolReturnsErrorOnGet");

    auto cb = makeCallbackHandle();
    auto request = makeTestCommand(kNoTimeout, makeEchoCmdObj());
    auto deferred = runCommand(cb, request);

    auto result = deferred.get(interruptible());

    ASSERT_EQ(ErrorCodes::HostUnreachable, result.status);
    ASSERT_EQ(result.target, fixture().getServers().front());
    assertNumOps({.canceled = 0u, .timedOut = 0u, .failed = 1u, .succeeded = 0u});
}

TEST_WITH_AND_WITHOUT_BATON_F(NetworkInterfaceTest, ShutdownBeforeSendRequest) {
    SKIP_ON_GRPC("gRPC client factory shutdown blocks until all clients have been destroyed");

    auto operationKey = UUID::gen();

    // Block the remote handling of "echo" indefinitely. If the NI sends the command and is
    // (incorrectly) unable to cancel it, then this test will waiting for shutdown to complete.
    auto fpGuard = configureFailCommand("echo", boost::none, Milliseconds(60000));

    // Block the reactor thread after it acquires a connection but before it sends the request.
    boost::optional<FailPointEnableBlock> fpb("networkInterfaceHangCommandsAfterAcquireConn");
    boost::optional<FailPointEnableBlock> shutdownFp("hangBeforeDrainingCommandStates");

    auto pf = makePromiseFuture<RemoteCommandResponse>();
    auto commandThread = stdx::thread([&]() {
        auto cb = makeCallbackHandle();
        auto request =
            makeTestCommand(kNoTimeout, makeEchoCmdObj(), nullptr, false, {}, operationKey);
        pf.promise.setFrom(runCommand(cb, request).getNoThrow(interruptible()));
    });
    ON_BLOCK_EXIT([&] {
        // TODO SERVER-93077: remove this killOp once disabling the failpoint is sufficient.
        runSetupCommandSync(
            DatabaseName::kAdmin,
            BSON("_killOperations" << 1 << "operationKeys" << BSON_ARRAY(operationKey)));
        commandThread.join();
    });

    // Once the command thread has reached the failpoint, begin shutdown.
    fpb.get()->waitForTimesEntered(fpb->initialTimesEntered() + 1);

    Notification<void> shutdownComplete;
    auto shutdownThread = stdx::thread([&]() {
        net().shutdown();
        shutdownComplete.set();
    });
    ON_BLOCK_EXIT([&] { shutdownThread.join(); });

    // Wait for the shutdown thread to start draining operations in shutdown.
    shutdownFp.get()->waitForTimesEntered(shutdownFp->initialTimesEntered() + 1);

    // Disable the failpoint so the reactor can proceed and attempt to send the request.
    // Shutdown and draining should then complete despite the blocking failCommand, since the
    // request will either never be sent (if the cancellation performed by shutdown has already
    // occured) or the socket read/write will be interrupted if it has already begun.
    fpb.reset();
    shutdownFp.reset();

    auto client = getGlobalServiceContext()->getService()->makeClient(__FILE__);
    auto opCtx = client->makeOperationContext();
    ASSERT(shutdownComplete.waitFor(opCtx.get(), Seconds(30)));

    // Since shutdown has completed, this future should be ready very soon if not immediately.
    ASSERT_EQ(getWithTimeout(pf.future, *opCtx, Seconds(1)).status, ErrorCodes::ShutdownInProgress);

    assertNumOps({.canceled = 1u, .timedOut = 0u, .failed = 0u, .succeeded = 0u});

    ConnectionPoolStats stats;
    net().appendConnectionStats(&stats);
    ASSERT_EQ(stats.totalAvailable, 0);
    ASSERT_EQ(stats.totalInUse, 0);
}

TEST_WITH_AND_WITHOUT_BATON_F(NetworkInterfaceTest, ShutdownAfterSendRequest) {
    auto operationKey = UUID::gen();

    // Block the remote handling of "echo" indefinitely. If the NI sends the command and is
    // (incorrectly) unable to cancel it, then this test will waiting for shutdown to complete.
    auto fpGuard = configureFailCommand("echo", boost::none, Milliseconds(60000));

    auto pf = makePromiseFuture<RemoteCommandResponse>();
    auto commandThread = stdx::thread([&]() {
        auto cb = makeCallbackHandle();
        auto request =
            makeTestCommand(kNoTimeout, makeEchoCmdObj(), nullptr, false, {}, operationKey);
        pf.promise.setFrom(runCommand(cb, request).getNoThrow(interruptible()));
    });
    ON_BLOCK_EXIT([&] {
        // TODO SERVER-93077: remove this killOp once disabling the failpoint is sufficient.
        runSetupCommandSync(
            DatabaseName::kAdmin,
            BSON("_killOperations" << 1 << "operationKeys" << BSON_ARRAY(operationKey)));
        commandThread.join();
    });

    fpGuard.waitForAdditionalTimesEntered(1);

    // Once the command thread has started blocking remotely due to the failpoint, begin shutdown.
    // Shutdown and draining should complete despite the blocking failCommand, since the socket
    // read will be interrupted by connection pool shutdown.
    Notification<void> shutdownComplete;
    auto shutdownThread = stdx::thread([&]() {
        net().shutdown();
        shutdownComplete.set();
    });
    ON_BLOCK_EXIT([&] { shutdownThread.join(); });

    auto client = getGlobalServiceContext()->getService()->makeClient(__FILE__);
    auto opCtx = client->makeOperationContext();
    ASSERT(shutdownComplete.waitFor(opCtx.get(), Seconds(30)));

    // Since shutdown has completed, this future should be ready very soon if not immediately.
    ASSERT_EQ(getWithTimeout(pf.future, *opCtx, Seconds(1)).status, ErrorCodes::ShutdownInProgress);

    assertNumOps({.canceled = 1u, .timedOut = 0u, .failed = 0u, .succeeded = 0u});

    assertConnectionStats(
        getFactory(),
        getServer(),
        [](const ConnectionStatsPer& stats) { return stats.inUse == 0 && stats.available == 0; },
        [&](const GRPCConnectionStats& stats) {
            return stats.getTotalInUseStreams() == 0 && stats.getTotalOpenChannels() == 0;
        },
        "ShutdownAfterSendRequest failed assertion on inUse and available connection/channel "
        "count.");
}

class NetworkInterfaceTestWithConnectHook : public NetworkInterfaceTest {
public:
    void setConnectHook(std::unique_ptr<NetworkConnectionHook> hook) {
        _hook = std::move(hook);
    }

protected:
    std::unique_ptr<NetworkInterface> _makeNet(std::string instanceName,
                                               transport::TransportProtocol protocol) override {
        return makeNetworkInterface(
            instanceName, std::move(_hook), nullptr, makeDefaultConnectionPoolOptions(), protocol);
    }

private:
    std::unique_ptr<NetworkConnectionHook> _hook;
};

class NetworkInterfaceTestWithHelloHook : public NetworkInterfaceTestWithConnectHook {
public:
    void setUp() override {
        setConnectHook(std::make_unique<WaitForHelloHook>(this));
        NetworkInterfaceTestWithConnectHook::setUp();
    }

    struct HelloData {
        BSONObj request;
        RemoteCommandResponse response;
    };

    virtual HelloData waitForHello() {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        _helloCondVar.wait(lk, [this] { return _helloResult != boost::none; });

        return std::move(*_helloResult);
    }

    bool hasHelloResult() {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        return _helloResult != boost::none;
    }

private:
    class WaitForHelloHook : public NetworkConnectionHook {
    public:
        explicit WaitForHelloHook(NetworkInterfaceTestWithHelloHook* parent) : _parent(parent) {}

        Status validateHost(const HostAndPort& host,
                            const BSONObj& request,
                            const RemoteCommandResponse& helloReply) override {
            stdx::lock_guard<stdx::mutex> lk(_parent->_mutex);
            _parent->_helloResult = HelloData{request, helloReply};
            _parent->_helloCondVar.notify_all();
            return Status::OK();
        }

        StatusWith<boost::optional<RemoteCommandRequest>> makeRequest(const HostAndPort&) override {
            return {boost::none};
        }

        Status handleReply(const HostAndPort&, RemoteCommandResponse&&) override {
            return Status::OK();
        }

    private:
        NetworkInterfaceTestWithHelloHook* _parent;
    };

protected:
    stdx::mutex _mutex;
    stdx::condition_variable _helloCondVar;
    boost::optional<HelloData> _helloResult;
};

TEST_F(NetworkInterfaceTestWithHelloHook,
       HelloRequestMissingInternalClientInfoWhenNotInternalClient) {
    resetIsInternalClient(false);

    auto deferred = runCommand(makeCallbackHandle(), makeTestCommand(kNoTimeout, makeEchoCmdObj()));
    auto helloHandshake = waitForHello();

    // Verify that the "hello" reply has the expected internalClient data.
    ASSERT_FALSE(helloHandshake.request["internalClient"]);
    // Verify that the ping op is counted as a success.
    auto res = deferred.get(interruptible());
    ASSERT(res.elapsed);
    assertNumOps({.canceled = 0u, .timedOut = 0u, .failed = 0u, .succeeded = 1u});
}

class NetworkInterfaceInternalClientTest : public NetworkInterfaceTestWithHelloHook {
public:
    void setUp() override {
        resetIsInternalClient(true);
        NetworkInterfaceTestWithHelloHook::setUp();
    }

    void tearDown() override {
        NetworkInterfaceTestWithHelloHook::tearDown();
        resetIsInternalClient(false);
    }
};

TEST_F(NetworkInterfaceInternalClientTest,
       HelloRequestContainsOutgoingWireVersionInternalClientInfo) {
    auto deferred = runCommand(makeCallbackHandle(), makeTestCommand(kNoTimeout, makeEchoCmdObj()));
    auto helloHandshake = waitForHello();

    // Verify that the "hello" reply has the expected internalClient data.
    auto outgoing = WireSpec::getWireSpec(getGlobalServiceContext()).getOutgoing();
    auto internalClientElem = helloHandshake.request["internalClient"];
    ASSERT_EQ(internalClientElem.type(), BSONType::object);
    auto minWireVersionElem = internalClientElem.Obj()["minWireVersion"];
    auto maxWireVersionElem = internalClientElem.Obj()["maxWireVersion"];
    ASSERT_EQ(minWireVersionElem.type(), BSONType::numberInt);
    ASSERT_EQ(maxWireVersionElem.type(), BSONType::numberInt);
    ASSERT_EQ(minWireVersionElem.numberInt(), outgoing.minWireVersion);
    ASSERT_EQ(maxWireVersionElem.numberInt(), outgoing.maxWireVersion);

    // Verify that the ping op is counted as a success.
    auto res = deferred.get();
    ASSERT(res.elapsed);
    assertNumOps({.canceled = 0u, .timedOut = 0u, .failed = 0u, .succeeded = 1u});
}

class NetworkInterfaceTestWithHangingHook : public NetworkInterfaceTestWithConnectHook {
public:
    void setUp() override {
        setConnectHook(std::make_unique<HangingHook>());
        NetworkInterfaceTestWithConnectHook::setUp();
    }

private:
    // Hook that intentionally never finishes
    class HangingHook : public executor::NetworkConnectionHook {
        Status validateHost(const HostAndPort&,
                            const BSONObj& request,
                            const RemoteCommandResponse&) final {
            return Status::OK();
        }

        StatusWith<boost::optional<RemoteCommandRequest>> makeRequest(
            const HostAndPort& remoteHost) final {
            return {boost::make_optional(RemoteCommandRequest(remoteHost,
                                                              DatabaseName::kAdmin,
                                                              BSON("sleep" << 1 << "lock"
                                                                           << "none"
                                                                           << "secs" << 100000000),
                                                              BSONObj(),
                                                              nullptr))};
        }

        Status handleReply(const HostAndPort& remoteHost, RemoteCommandResponse&& response) final {
            if (!pingCommandMissing(response)) {
                ASSERT_EQ(ErrorCodes::CallbackCanceled, response.status);
                return response.status;
            }

            return {ErrorCodes::ExceededTimeLimit, "No ping command. Returning pseudo-timeout."};
        }
    };
};

// Test that we time out a command if the connection hook hangs.
TEST_F(NetworkInterfaceTestWithHangingHook, HookHangs) {
    /**
     *  Since mongos's have no ping command, we effectively skip this test by returning
     *  ExceededTimeLimit above. (That ErrorCode is used heavily in repl and sharding code.)
     *  If we return NetworkInterfaceExceededTimeLimit, it will make the ConnectionPool
     *  attempt to reform the connection, which can lead to an accepted but unfortunate
     *  race between TLConnection::setup and TLTypeFactory::shutdown.
     *  We assert here that the error code we get is in the error class of timeouts,
     *  which covers both NetworkInterfaceExceededTimeLimit and ExceededTimeLimit.
     */
    RemoteCommandRequest request{fixture().getServers()[0],
                                 DatabaseName::kAdmin,
                                 BSON("ping" << 1),
                                 BSONObj(),
                                 nullptr,
                                 Seconds(1)};
    auto res = runCommandSync(request);
    ASSERT(ErrorCodes::isExceededTimeLimitError(res.status.code()));
}

TEST_F(NetworkInterfaceIntegrationFixture, BasicRejectConnection) {
    SKIP_ON_GRPC("gRPC doesn't use ConnectionPool");

    FailPointEnableBlock fpb2("connectionPoolRejectsConnectionRequests");

    startNet();

    RemoteCommandRequest request{fixture().getServers()[0],
                                 DatabaseName::kAdmin,
                                 BSON("ping" << 1),
                                 BSONObj(),
                                 nullptr,
                                 Minutes(5)};

    auto fut2 = runCommand(makeCallbackHandle(), request);
    ASSERT_FALSE(fut2.get(interruptible()).isOK());

    auto result = fut2.get(interruptible());
    ASSERT_NOT_OK(result.status);
    ASSERT_EQ(ErrorCodes::PooledConnectionAcquisitionRejected, result.status);
}

TEST_F(NetworkInterfaceIntegrationFixture, RejectConnection) {
    SKIP_ON_GRPC("gRPC doesn't use ConnectionPool");

    FailPointEnableBlock fpb("connectionPoolDoesNotFulfillRequests");

    ConnectionPool::Options opts;
    opts.connectionRequestsMaxQueueDepth = 1;
    setConnectionPoolOptions(opts);

    startNet();

    RemoteCommandRequest request{fixture().getServers()[0],
                                 DatabaseName::kAdmin,
                                 BSON("ping" << 1),
                                 BSONObj(),
                                 nullptr,
                                 Minutes(5)};

    auto fut = runCommand(makeCallbackHandle(), request);

    auto fut2 = runCommand(makeCallbackHandle(), request);
    ASSERT_FALSE(fut2.get(interruptible()).isOK());

    auto result = fut2.get(interruptible());
    ASSERT_NOT_OK(result.status);
    ASSERT_EQ(ErrorCodes::PooledConnectionAcquisitionRejected, result.status);
}


}  // namespace
}  // namespace executor
}  // namespace mongo
