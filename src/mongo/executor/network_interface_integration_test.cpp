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
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

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
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/hedge_options_util.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/network_interface_integration_fixture.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace executor {
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

    auto fut = runCommand(makeCallbackHandle(), request);
    ASSERT_FALSE(fut.isReady());
    net().startup();
    ASSERT(fut.get().isOK());
}

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


// Test that we time out a command if the connection hook hangs.
TEST_F(NetworkInterfaceIntegrationFixture, HookHangs) {
    startNet(std::make_unique<HangingHook>());

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

using ResponseStatus = TaskExecutor::ResponseStatus;

BSONObj objConcat(std::initializer_list<BSONObj> objs) {
    BSONObjBuilder bob;

    for (const auto& obj : objs) {
        bob.appendElements(obj);
    }

    return bob.obj();
}

class NetworkInterfaceTest : public NetworkInterfaceIntegrationFixture {
public:
    constexpr static Milliseconds kNoTimeout = RemoteCommandRequest::kNoTimeout;
    constexpr static Milliseconds kMaxWait = Milliseconds(Minutes(1));

    void assertNumOps(uint64_t canceled, uint64_t timedOut, uint64_t failed, uint64_t succeeded) {
        auto counters = net().getCounters();
        ASSERT_EQ(canceled, counters.canceled);
        ASSERT_EQ(timedOut, counters.timedOut);
        ASSERT_EQ(failed, counters.failed);
        ASSERT_EQ(succeeded, counters.succeeded);
    }

    void setUp() override {
        startNet(std::make_unique<WaitForHelloHook>(this));
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
                                         RemoteCommandRequest::Options options = {}) {
        auto cs = fixture();
        return RemoteCommandRequest(cs.getServers().front(),
                                    DatabaseName::kAdmin,
                                    std::move(cmd),
                                    BSONObj(),
                                    opCtx,
                                    timeout,
                                    std::move(options));
    }

    BSONObj makeEchoCmdObj() {
        return BSON("echo" << 1 << "foo"
                           << "bar");
    }

    BSONObj makeSleepCmdObj() {
        return BSON("sleep" << 1 << "lock"
                            << "none"
                            << "secs" << 1000000000);
    }

    RemoteCommandResponse runCurrentOpForCommand(HostAndPort target, const std::string command) {
        const auto cmdObj =
            BSON("aggregate" << 1 << "pipeline"
                             << BSON_ARRAY(BSON("$currentOp" << BSON("localOps" << true))
                                           << BSON("$match" << BSON(("command." + command)
                                                                    << BSON("$exists" << true))))
                             << "cursor" << BSONObj() << "$readPreference"
                             << BSON("mode"
                                     << "nearest"));
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

    struct HelloData {
        BSONObj request;
        RemoteCommandResponse response;
    };
    HelloData waitForHello() {
        stdx::unique_lock<Latch> lk(_mutex);
        _helloCondVar.wait(lk, [this] { return _helloResult != boost::none; });

        return std::move(*_helloResult);
    }

    bool hasHelloResult() {
        stdx::lock_guard<Latch> lk(_mutex);
        return _helloResult != boost::none;
    }

private:
    class WaitForHelloHook : public NetworkConnectionHook {
    public:
        explicit WaitForHelloHook(NetworkInterfaceTest* parent) : _parent(parent) {}

        Status validateHost(const HostAndPort& host,
                            const BSONObj& request,
                            const RemoteCommandResponse& helloReply) override {
            stdx::lock_guard<Latch> lk(_parent->_mutex);
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
        NetworkInterfaceTest* _parent;
    };

    Mutex _mutex = MONGO_MAKE_LATCH("NetworkInterfaceTest::_mutex");
    stdx::condition_variable _helloCondVar;
    boost::optional<HelloData> _helloResult;
};

class NetworkInterfaceInternalClientTest : public NetworkInterfaceTest {
public:
    void setUp() override {
        resetIsInternalClient(true);
        NetworkInterfaceTest::setUp();
    }

    void tearDown() override {
        NetworkInterfaceTest::tearDown();
        resetIsInternalClient(false);
    }
};

TEST_F(NetworkInterfaceTest, CancelMissingOperation) {
    // This is just a sanity check, this action should have no effect.
    net().cancelCommand(makeCallbackHandle());
    assertNumOps(0u, 0u, 0u, 0u);
}

TEST_F(NetworkInterfaceTest, CancelLocally) {
    auto cbh = makeCallbackHandle();

    auto deferred = [&] {
        // Kick off our operation
        FailPointEnableBlock fpb("networkInterfaceHangCommandsAfterAcquireConn");

        auto deferred = runCommand(cbh, makeTestCommand(kMaxWait, makeEchoCmdObj()));

        waitForHello();

        fpb->waitForTimesEntered(fpb.initialTimesEntered() + 1);

        net().cancelCommand(cbh);

        return deferred;
    }();

    // Wait for op to complete, assert that it was canceled.
    auto result = deferred.get();
    ASSERT_EQ(ErrorCodes::CallbackCanceled, result.status);
    ASSERT(result.elapsed);

    assertNumOps(1u, 0u, 0u, 0u);
}

TEST_F(NetworkInterfaceTest, CancelRemotely) {
    // Enable blockConnection for "echo".
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
                        BSON("configureFailPoint"
                             << "failCommand"
                             << "mode"
                             << "off"),
                        kNoTimeout);
    });

    int numCurrentOpRan = 0;

    auto cbh = makeCallbackHandle();
    auto deferred = [&] {
        RemoteCommandRequest::Options options;
        options.hedgeOptions.isHedgeEnabled = true;
        // Kick off an "echo" operation, which should block until cancelCommand causes
        // the operation to be killed.
        auto deferred = runCommand(
            cbh, makeTestCommand(kNoTimeout, makeEchoCmdObj(), nullptr /* opCtx */, options));

        // Wait for the "echo" operation to start.
        numCurrentOpRan += waitForCommandToStart("echo", kMaxWait);

        // Run cancelCommand to kill the above operation.
        net().cancelCommand(cbh);

        return deferred;
    }();

    // Wait for the command to return, assert that it was canceled.
    auto result = deferred.get();
    ASSERT_EQ(ErrorCodes::CallbackCanceled, result.status);
    ASSERT(result.elapsed);

    // Wait for the operation to be killed on the remote host.
    numCurrentOpRan += waitForCommandToStop("echo", kMaxWait);

    // We have one canceled operation (echo), and two other succeeded operations
    // on top of the currentOp operations (configureFailPoint and _killOperations).
    assertNumOps(1u, 0u, 0u, 2u + numCurrentOpRan);
}

TEST_F(NetworkInterfaceTest, CancelRemotelyTimedOut) {
    // Enable blockConnection for "echo" and "_killOperations".
    assertCommandOK(DatabaseName::kAdmin,
                    BSON("configureFailPoint"
                         << "failCommand"
                         << "mode"
                         << "alwaysOn"
                         << "data"
                         << BSON("blockConnection" << true << "blockTimeMS" << 5000
                                                   << "failCommands"
                                                   << BSON_ARRAY("echo"
                                                                 << "_killOperations"))),
                    kNoTimeout);

    ON_BLOCK_EXIT([&] {
        // Disable blockConnection.
        assertCommandOK(DatabaseName::kAdmin,
                        BSON("configureFailPoint"
                             << "failCommand"
                             << "mode"
                             << "off"),
                        kNoTimeout);
    });

    int numCurrentOpRan = 0;

    auto cbh = makeCallbackHandle();
    auto deferred = [&] {
        RemoteCommandRequest::Options options;
        options.hedgeOptions.isHedgeEnabled = true;
        // Kick off a blocking "echo" operation.
        auto deferred = runCommand(
            cbh, makeTestCommand(kNoTimeout, makeEchoCmdObj(), nullptr /* opCtx */, options));

        // Wait for the "echo" operation to start.
        numCurrentOpRan += waitForCommandToStart("echo", kMaxWait);

        // Run cancelCommand to kill the above operation. _killOperations is expected to block and
        // time out, and to be canceled by the command timer.
        FailPointEnableBlock cmdFailedFpb("networkInterfaceCommandsFailedWithErrorCode",
                                          BSON("cmdNames"
                                               << BSON_ARRAY("_killOperations") << "errorCode"
                                               << ErrorCodes::NetworkInterfaceExceededTimeLimit));

        net().cancelCommand(cbh);

        // Wait for _killOperations for 'echo' to time out.
        cmdFailedFpb->waitForTimesEntered(cmdFailedFpb.initialTimesEntered() + 1);

        return deferred;
    }();

    // Wait for the command to return, assert that it was canceled.
    auto result = deferred.get();
    ASSERT_EQ(ErrorCodes::CallbackCanceled, result.status);
    ASSERT(result.elapsed);

    // We have one canceled operation (echo), one timedout operation (_killOperations),
    // and one succeeded operation on top of the currentOp operations (configureFailPoint).
    assertNumOps(1u, 1u, 0u, 1u + numCurrentOpRan);
}

TEST_F(NetworkInterfaceTest, ImmediateCancel) {
    auto cbh = makeCallbackHandle();

    auto deferred = [&] {
        // Kick off our operation
        FailPointEnableBlock fpb("networkInterfaceDiscardCommandsBeforeAcquireConn");

        auto deferred = runCommand(cbh, makeTestCommand(kMaxWait, makeEchoCmdObj()));

        fpb->waitForTimesEntered(fpb.initialTimesEntered() + 1);

        net().cancelCommand(cbh);

        return deferred;
    }();

    ASSERT_EQ(net().getCounters().sent, 0);

    // Wait for op to complete, assert that it was canceled.
    auto result = deferred.get();
    ASSERT_EQ(ErrorCodes::CallbackCanceled, result.status);
    ASSERT(result.elapsed);
    assertNumOps(1u, 0u, 0u, 0u);
}

TEST_F(NetworkInterfaceTest, LateCancel) {
    auto cbh = makeCallbackHandle();

    auto deferred = runCommand(cbh, makeTestCommand(kMaxWait, makeEchoCmdObj()));

    // Wait for op to complete, assert that it was canceled.
    auto result = deferred.get();
    net().cancelCommand(cbh);

    ASSERT_OK(result.status);
    ASSERT(result.elapsed);
    assertNumOps(0u, 0u, 0u, 1u);
}

TEST_F(NetworkInterfaceTest, ConnectionErrorDropsSingleConnection) {
    FailPoint* failPoint =
        globalFailPointRegistry().find("asioTransportLayerAsyncConnectReturnsConnectionError");
    auto timesEntered = failPoint->setMode(FailPoint::nTimes, 1);

    auto cbh = makeCallbackHandle();
    auto deferred = runCommand(cbh, makeTestCommand(kMaxWait, makeEchoCmdObj()));
    // Wait for one of the connection attempts to fail with a `ConnectionError`.
    failPoint->waitForTimesEntered(timesEntered + 1);
    auto result = deferred.get();

    ASSERT_OK(result.status);
    ConnectionPoolStats stats;
    net().appendConnectionStats(&stats);

    ASSERT_EQ(stats.totalCreated, 2);
    ASSERT_EQ(stats.totalInUse + stats.totalAvailable + stats.totalRefreshing, 1);
    // Connection dropped during finishRefresh, so the dropped connection still
    // counts toward the refreshed counter.
    ASSERT_EQ(stats.totalRefreshed, 2);
}

TEST_F(NetworkInterfaceTest, AsyncOpTimeout) {
    // Kick off operation
    auto cb = makeCallbackHandle();
    auto request = makeTestCommand(Milliseconds{1000}, makeSleepCmdObj());
    auto deferred = runCommandOnAny(cb, request);

    waitForHello();

    auto result = deferred.get();

    // mongos doesn't implement the ping command, so ignore the response there, otherwise
    // check that we've timed out.
    if (!pingCommandMissing(result)) {
        ASSERT_EQ(ErrorCodes::NetworkInterfaceExceededTimeLimit, result.status);
        ASSERT(result.elapsed);
        ASSERT_EQ(result.target, fixture().getServers().front());
        assertNumOps(0u, 1u, 0u, 0u);
    }
}

TEST_F(NetworkInterfaceTest, AsyncOpTimeoutWithOpCtxDeadlineSooner) {
    // Kick off operation
    auto cb = makeCallbackHandle();

    constexpr auto opCtxDeadline = Milliseconds{600};
    constexpr auto requestTimeout = Milliseconds{1000};

    auto serviceContext = ServiceContext::make();
    auto client = serviceContext->makeClient("NetworkClient");
    auto opCtx = client->makeOperationContext();

    auto stopWatch = serviceContext->getPreciseClockSource()->makeStopWatch();
    opCtx->setDeadlineByDate(stopWatch.start() + opCtxDeadline, ErrorCodes::ExceededTimeLimit);

    auto request = makeTestCommand(requestTimeout, makeSleepCmdObj(), opCtx.get());

    auto deferred = runCommandOnAny(cb, request);
    // The time returned in result.elapsed is measured from when the command started, which happens
    // in runCommand. The delay between setting the deadline on opCtx and starting the command can
    // be long enough that the assertion about opCtxDeadline fails.
    auto networkStartCommandDelay = stopWatch.elapsed();

    waitForHello();

    auto result = deferred.get();

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
    assertNumOps(0u, 1u, 0u, 0u);
}

TEST_F(NetworkInterfaceTest, AsyncOpTimeoutWithOpCtxDeadlineLater) {
    // Kick off operation
    auto cb = makeCallbackHandle();

    constexpr auto opCtxDeadline = Milliseconds{1000};
    constexpr auto requestTimeout = Milliseconds{600};

    auto serviceContext = ServiceContext::make();
    auto client = serviceContext->makeClient("NetworkClient");
    auto opCtx = client->makeOperationContext();

    auto stopWatch = serviceContext->getPreciseClockSource()->makeStopWatch();
    opCtx->setDeadlineByDate(stopWatch.start() + opCtxDeadline, ErrorCodes::ExceededTimeLimit);

    auto request = makeTestCommand(requestTimeout, makeSleepCmdObj(), opCtx.get());

    auto deferred = runCommandOnAny(cb, request);
    // The time returned in result.elapsed is measured from when the command started, which happens
    // in runCommand. The delay between setting the deadline on opCtx and starting the command can
    // be long enough that the assertion about opCtxDeadline fails.
    auto networkStartCommandDelay = stopWatch.elapsed();

    waitForHello();

    auto result = deferred.get();

    // mongos doesn't implement the ping command, so ignore the response there, otherwise
    // check that we've timed out.
    if (pingCommandMissing(result)) {
        return;
    }

    ASSERT_EQ(ErrorCodes::NetworkInterfaceExceededTimeLimit, result.status);
    ASSERT(result.elapsed);

    // check that the request timeout uses the smaller of the operation context deadline and
    // the timeout specified in the request constructor.
    ASSERT_GTE(duration_cast<Milliseconds>(result.elapsed.value()), requestTimeout);
    ASSERT_LT(duration_cast<Milliseconds>(result.elapsed.value() + networkStartCommandDelay),
              opCtxDeadline);

    assertNumOps(0u, 1u, 0u, 0u);
}

TEST_F(NetworkInterfaceTest, StartCommand) {
    RemoteCommandRequest::Options options;
    options.hedgeOptions.isHedgeEnabled = true;
    auto request = makeTestCommand(kNoTimeout, makeEchoCmdObj(), nullptr /* opCtx */, options);

    auto deferred = runCommand(makeCallbackHandle(), std::move(request));

    auto res = deferred.get();

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
    assertNumOps(0u, 0u, 0u, 1u);
}

TEST_F(NetworkInterfaceTest, FireAndForget) {
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
                        BSON("configureFailPoint"
                             << "failCommand"
                             << "mode"
                             << "off"));
    });

    // Run fireAndForget commands and verify that we get status OK responses.
    const int numFireAndForgetRequests = 3;
    std::vector<Future<RemoteCommandResponse>> futures;

    RemoteCommandRequest::Options options;
    options.fireAndForget = true;
    for (int i = 0; i < numFireAndForgetRequests; i++) {
        auto cbh = makeCallbackHandle();
        auto fireAndForgetRequest =
            makeTestCommand(kNoTimeout, makeEchoCmdObj(), nullptr /* opCtx */, options);
        futures.push_back(runCommand(cbh, fireAndForgetRequest));
    }

    for (auto& future : futures) {
        auto result = future.get();
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
    assertNumOps(0u, 0u, 0u, 5u);
}

TEST_F(NetworkInterfaceInternalClientTest, StartCommandOnAny) {
    // The echo command below uses hedging so after a response is returned, we will issue
    // a _killOperations command to kill the pending operation. As a result, the number of
    // successful commands can sometimes be 2 (echo and _killOperations) instead 1 when the
    // num ops assertion below runs.
    FailPointEnableBlock fpb("networkInterfaceShouldNotKillPendingRequests");

    auto commandRequest = makeEchoCmdObj();
    auto request = [&] {
        auto cs = fixture();
        RemoteCommandRequestBase::Options options;
        options.hedgeOptions.isHedgeEnabled = true;
        options.hedgeOptions.hedgeCount = 1;

        return RemoteCommandRequestOnAny({cs.getServers()},
                                         DatabaseName::kAdmin,
                                         std::move(commandRequest),
                                         BSONObj(),
                                         nullptr,
                                         RemoteCommandRequest::kNoTimeout,
                                         options);
    }();

    auto deferred = runCommandOnAny(makeCallbackHandle(), std::move(request));
    auto res = deferred.get();

    uassertStatusOK(res.status);
    auto cmdObj = res.data.getObjectField("echo");
    ASSERT_EQ(1, cmdObj.getIntField("echo"));
    ASSERT_EQ("bar"_sd, cmdObj.getStringField("foo"));
    ASSERT_EQ("admin"_sd, cmdObj.getStringField("$db"));
    ASSERT_FALSE(cmdObj["clientOperationKey"].eoo());
    ASSERT_EQ(1, res.data.getIntField("ok"));
    assertNumOps(0u, 0u, 0u, 1u);
}

TEST_F(NetworkInterfaceTest, SetAlarm) {
    // set a first alarm, to execute after "expiration"
    Date_t expiration = net().now() + Milliseconds(100);
    auto makeTimerFuture = [&] {
        auto pf = makePromiseFuture<Date_t>();
        return std::make_pair(
            [this, promise = std::move(pf.promise)](Status status) mutable {
                if (status.isOK()) {
                    promise.emplaceValue(net().now());
                } else {
                    promise.setError(status);
                }
            },
            std::move(pf.future));
    };

    auto futurePair = makeTimerFuture();
    ASSERT_OK(net().setAlarm(makeCallbackHandle(), expiration, std::move(futurePair.first)));

    // assert that it executed after "expiration"
    auto& result = futurePair.second.get();
    ASSERT(result >= expiration);

    expiration = net().now() + Milliseconds(99999999);
    auto futurePair2 = makeTimerFuture();
    ASSERT_OK(net().setAlarm(makeCallbackHandle(), expiration, std::move(futurePair2.first)));

    net().shutdown();
    auto swResult = futurePair2.second.getNoThrow();
    ASSERT_FALSE(swResult.isOK());
}

TEST_F(NetworkInterfaceTest, UseOperationKeyWhenProvided) {
    const auto opKey = UUID::gen();
    assertCommandOK(DatabaseName::kAdmin,
                    BSON("configureFailPoint"
                         << "failIfOperationKeyMismatch"
                         << "mode"
                         << "alwaysOn"
                         << "data" << BSON("clientOperationKey" << opKey)),
                    kNoTimeout);

    ON_BLOCK_EXIT([&] {
        assertCommandOK(DatabaseName::kAdmin,
                        BSON("configureFailPoint"
                             << "failIfOperationKeyMismatch"
                             << "mode"
                             << "off"),
                        kNoTimeout);
    });

    RemoteCommandRequest::Options rcrOptions;
    rcrOptions.hedgeOptions.isHedgeEnabled = true;
    rcrOptions.hedgeOptions.hedgeCount = fixture().getServers().size();
    RemoteCommandRequestOnAny rcr(fixture().getServers(),
                                  DatabaseName::kAdmin,
                                  makeEchoCmdObj(),
                                  BSONObj(),
                                  nullptr,
                                  kNoTimeout,
                                  std::move(rcrOptions),
                                  opKey);
    // Only internal clients can run hedged operations.
    resetIsInternalClient(true);
    ON_BLOCK_EXIT([&] { resetIsInternalClient(false); });
    auto cbh = makeCallbackHandle();
    auto fut = runCommand(cbh, std::move(rcr));
    fut.get();
}

class HedgeCancellationTest : public NetworkInterfaceTest {
public:
    enum class CancellationMode { kAfterCompletion, kAfterScheduling };

    void runTest(CancellationMode mode) {
        if (fixture().type() != ConnectionString::ConnectionType::kReplicaSet) {
            LOGV2(7176401, "Skipped: this test may only run against a replica-set");
            return;
        }

        auto cbh = makeCallbackHandle();
        auto future = [&] {
            _blockCommandsOnAllServers(BSON_ARRAY("echo"
                                                  << "_killOperations"));
            ON_BLOCK_EXIT([&] { _unblockCommandsOnAllServers({"echo", "_killOperations"}); });

            auto future = _scheduleHedgedEcho(cbh);

            _waitForServersToStartRunningEcho();

            if (mode == CancellationMode::kAfterScheduling) {
                net().cancelCommand(cbh);
            } else {
                // Let the first node in the list of servers proceed with running the command by
                // killing the blocked `echo` operation. This results in the completion of the
                // operation and cancels all pending hedged operations.
                _killRemoteOps(fixture().getServers().front(), "echo");
            }

            _waitForServersToStartRunningKillOperations(mode);

            return future;
        }();

        LOGV2(7176402, "Wait for the remote command to finish");
        std::move(future).ignoreValue().get();
    }

private:
    void _runCommand(const HostAndPort& server, const DatabaseName& db, BSONObj cmd) {
        RemoteCommandRequest request{server, db, cmd, BSONObj(), nullptr, kNoTimeout};
        request.sslMode = transport::kGlobalSSLMode;
        auto res = runCommandSync(request);
        ASSERT_OK(res.status);
        ASSERT_OK(getStatusFromCommandResult(res.data));
        ASSERT(!res.data["writeErrors"]);
    }

    void _configureFailPoint(const HostAndPort& server,
                             std::string fpName,
                             bool enable,
                             BSONObj data = BSONObj()) {
        BSONObjBuilder bob;
        bob.append("configureFailPoint", fpName);
        bob.append("mode", enable ? "alwaysOn" : "off");
        if (!data.isEmpty())
            bob.append("data", std::move(data));
        _runCommand(server, DatabaseName::kAdmin, bob.obj());
    }

    void _blockCommandsOnAllServers(BSONArray cmds) {
        auto servers = fixture().getServers();
        for (const auto& server : servers) {
            _configureFailPoint(server,
                                "failCommand",
                                true,
                                BSON("blockConnection" << true << "blockTimeMS" << 1000000
                                                       << "failCommands" << cmds));
        }
    }

    void _killRemoteOps(HostAndPort server, std::string cmd) {
        auto res = runCurrentOpForCommand(server, cmd);
        for (auto& op : res.data["cursor"]["firstBatch"].Array()) {
            auto opid = op.Obj()["opid"];
            _runCommand(server, DatabaseName::kAdmin, BSON("killOp" << 1 << "op" << opid));
        }
    }

    void _unblockCommandsOnAllServers(std::vector<std::string> cmds) {
        LOGV2(7176403, "Disabling fail-points to unblock commands");
        auto servers = fixture().getServers();
        for (auto& server : servers) {
            // Must kill (and unblock) the remote operations blocked behind the `failCommand`
            // fail-point (if still running) before disabling it, otherwise it will hang forever.
            for (auto& cmd : cmds) {
                _killRemoteOps(server, cmd);
            }
            _configureFailPoint(server, "failCommand", false);
        }
    }

    Future<RemoteCommandResponse> _scheduleHedgedEcho(const TaskExecutor::CallbackHandle& cbh) {
        RemoteCommandRequest::Options rcrOptions;
        rcrOptions.hedgeOptions.isHedgeEnabled = true;
        rcrOptions.hedgeOptions.hedgeCount = fixture().getServers().size();
        RemoteCommandRequestOnAny rcr(fixture().getServers(),
                                      DatabaseName::kAdmin,
                                      makeEchoCmdObj(),
                                      BSONObj(),
                                      nullptr,
                                      kNoTimeout,
                                      std::move(rcrOptions));
        // Only internal clients can run hedged operations.
        resetIsInternalClient(true);
        ON_BLOCK_EXIT([&] { resetIsInternalClient(false); });
        LOGV2(7176404, "Scheduling the remote command");
        return runCommand(cbh, std::move(rcr));
    }

    void _waitForServerToRunCommand(const HostAndPort& server, std::string command) {
        ClockSource::StopWatch stopwatch;
        while (!isCommandRunning(command, server) && stopwatch.elapsed() < kMaxWait) {
            sleepmillis(100);
        }
    }

    void _waitForServersToStartRunningEcho() {
        LOGV2(7176405, "Waiting for all servers to start running the command");
        const auto cmd = "echo";
        auto servers = fixture().getServers();
        for (auto& server : servers) {
            _waitForServerToRunCommand(server, cmd);
            ASSERT_TRUE(isCommandRunning(cmd, server));
        }
    }

    void _waitForServersToStartRunningKillOperations(CancellationMode mode) {
        LOGV2(7176406, "Wait for servers to receive $_killOperations");
        const auto cmd = "_killOperations";
        auto servers = fixture().getServers();
        size_t idx = (mode == CancellationMode::kAfterCompletion) ? 1 : 0;
        for (; idx < servers.size(); idx++) {
            _waitForServerToRunCommand(servers[idx], cmd);
            ASSERT_TRUE(isCommandRunning(cmd, servers[idx]));
        }
    }
};

TEST_F(HedgeCancellationTest, CancelAfterScheduling) {
    // Cancel the hedged operation after it is scheduled on all targets and before completion.
    // We should send `_killOperations` to all targets that have already acquired a connection
    // and might have started/completed running the operation.
    runTest(CancellationMode::kAfterScheduling);
}

TEST_F(HedgeCancellationTest, CancelAfterCompletion) {
    // Waits until the hedged operation is scheduled on all targets, then cancels all pending
    // operations after the first scheduled operation completes. We should send
    // `_killOperations` to all targets except for the one used to fulfill the final promise
    // (i.e. complete the operation).
    runTest(CancellationMode::kAfterCompletion);
}

TEST_F(NetworkInterfaceInternalClientTest,
       HelloRequestContainsOutgoingWireVersionInternalClientInfo) {
    auto deferred = runCommand(makeCallbackHandle(), makeTestCommand(kNoTimeout, makeEchoCmdObj()));
    auto helloHandshake = waitForHello();

    // Verify that the "hello" reply has the expected internalClient data.
    auto wireSpec = WireSpec::getWireSpec(getGlobalServiceContext()).get();
    auto internalClientElem = helloHandshake.request["internalClient"];
    ASSERT_EQ(internalClientElem.type(), BSONType::Object);
    auto minWireVersionElem = internalClientElem.Obj()["minWireVersion"];
    auto maxWireVersionElem = internalClientElem.Obj()["maxWireVersion"];
    ASSERT_EQ(minWireVersionElem.type(), BSONType::NumberInt);
    ASSERT_EQ(maxWireVersionElem.type(), BSONType::NumberInt);
    ASSERT_EQ(minWireVersionElem.numberInt(), wireSpec->outgoing.minWireVersion);
    ASSERT_EQ(maxWireVersionElem.numberInt(), wireSpec->outgoing.maxWireVersion);

    // Verify that the ping op is counted as a success.
    auto res = deferred.get();
    ASSERT(res.elapsed);
    assertNumOps(0u, 0u, 0u, 1u);
}

TEST_F(NetworkInterfaceTest, HelloRequestMissingInternalClientInfoWhenNotInternalClient) {
    resetIsInternalClient(false);

    auto deferred = runCommand(makeCallbackHandle(), makeTestCommand(kNoTimeout, makeEchoCmdObj()));
    auto helloHandshake = waitForHello();

    // Verify that the "hello" reply has the expected internalClient data.
    ASSERT_FALSE(helloHandshake.request["internalClient"]);
    // Verify that the ping op is counted as a success.
    auto res = deferred.get();
    ASSERT(res.elapsed);
    assertNumOps(0u, 0u, 0u, 1u);
}

class ExhaustRequestHandlerUtil {
public:
    struct responseOutcomeCount {
        int _success = 0;
        int _failed = 0;
    };

    std::function<void(const RemoteCommandResponse&)>&& getExhaustRequestCallbackFn() {
        return std::move(_callbackFn);
    }

    ExhaustRequestHandlerUtil::responseOutcomeCount getCountersWhenReady() {
        stdx::unique_lock<Latch> lk(_mutex);
        _cv.wait(_mutex, [&] { return _replyUpdated; });
        _replyUpdated = false;
        return _responseOutcomeCount;
    }

private:
    // set to true once '_responseOutcomeCount' has been updated. Used to indicate that a new
    // response has been sent.
    bool _replyUpdated = false;

    // counter of how many successful and failed responses were received.
    responseOutcomeCount _responseOutcomeCount;

    Mutex _mutex = MONGO_MAKE_LATCH("ExhaustRequestHandlerUtil::_mutex");
    stdx::condition_variable _cv;

    // called when a server sends a new isMaster exhaust response. Updates _responseOutcomeCount
    // and _replyUpdated.
    std::function<void(const RemoteCommandResponse&)> _callbackFn =
        [&](const executor::RemoteCommandResponse& response) {
            {
                stdx::unique_lock<Latch> lk(_mutex);
                if (response.status.isOK()) {
                    _responseOutcomeCount._success++;
                } else {
                    _responseOutcomeCount._failed++;
                }
                _replyUpdated = true;
            }

            _cv.notify_all();
        };
};

TEST_F(NetworkInterfaceTest, StartExhaustCommandShouldReceiveMultipleResponses) {
    auto isMasterCmd = BSON("isMaster" << 1 << "maxAwaitTimeMS" << 1000 << "topologyVersion"
                                       << TopologyVersion(OID::max(), 0).toBSON());

    auto request = makeTestCommand(kNoTimeout, isMasterCmd);
    auto cbh = makeCallbackHandle();
    ExhaustRequestHandlerUtil exhaustRequestHandler;

    auto exhaustFuture = startExhaustCommand(
        cbh, std::move(request), exhaustRequestHandler.getExhaustRequestCallbackFn());

    {
        // The server sends a response either when a topology change occurs or when it has not sent
        // a response in 'maxAwaitTimeMS'. In this case we expect a response every 'maxAwaitTimeMS'
        // = 1000 (set in the isMaster cmd above)
        auto counters = exhaustRequestHandler.getCountersWhenReady();
        ASSERT(!exhaustFuture.isReady());

        // The first response should be successful
        ASSERT_EQ(counters._success, 1);
        ASSERT_EQ(counters._failed, 0);
    }

    {
        auto counters = exhaustRequestHandler.getCountersWhenReady();
        ASSERT(!exhaustFuture.isReady());

        // The second response should also be successful
        ASSERT_EQ(counters._success, 2);
        ASSERT_EQ(counters._failed, 0);
    }

    net().cancelCommand(cbh);
    auto error = exhaustFuture.getNoThrow();
    ASSERT((error == ErrorCodes::CallbackCanceled) || (error == ErrorCodes::HostUnreachable));

    auto counters = exhaustRequestHandler.getCountersWhenReady();

    // The command was cancelled so the 'fail' counter should be incremented
    ASSERT_EQ(counters._success, 2);
    ASSERT_EQ(counters._failed, 1);
}

TEST_F(NetworkInterfaceTest, StartExhaustCommandShouldStopOnFailure) {
    // Both assetCommandOK and makeTestCommand target the first host in the connection string, so we
    // are guaranteed that the failpoint is set on the same host that we run the exhaust command on.
    auto configureFailpointCmd = BSON("configureFailPoint"
                                      << "failCommand"
                                      << "mode"
                                      << "alwaysOn"
                                      << "data"
                                      << BSON("errorCode" << ErrorCodes::CommandFailed
                                                          << "failCommands"
                                                          << BSON_ARRAY("isMaster")));
    assertCommandOK(DatabaseName::kAdmin, configureFailpointCmd);

    ON_BLOCK_EXIT([&] {
        auto stopFpRequest = BSON("configureFailPoint"
                                  << "failCommand"
                                  << "mode"
                                  << "off");
        assertCommandOK(DatabaseName::kAdmin, stopFpRequest);
    });

    auto isMasterCmd = BSON("isMaster" << 1 << "maxAwaitTimeMS" << 1000 << "topologyVersion"
                                       << TopologyVersion(OID::max(), 0).toBSON());

    auto request = makeTestCommand(kNoTimeout, isMasterCmd);
    auto cbh = makeCallbackHandle();
    ExhaustRequestHandlerUtil exhaustRequestHandler;

    auto exhaustFuture = startExhaustCommand(
        cbh, std::move(request), exhaustRequestHandler.getExhaustRequestCallbackFn());

    {
        auto counters = exhaustRequestHandler.getCountersWhenReady();

        auto error = exhaustFuture.getNoThrow();
        ASSERT_EQ(error, ErrorCodes::CommandFailed);

        // The response should be marked as failed
        ASSERT_EQ(counters._success, 0);
        ASSERT_EQ(counters._failed, 1);
    }
}

TEST_F(NetworkInterfaceTest, ExhaustCommandCancelRunsOutOfLine) {
    thread_local bool inCancellationContext = false;
    auto pf = makePromiseFuture<bool>();
    auto cbh = makeCallbackHandle();
    auto callback = [&](auto&&) mutable {
        pf.promise.emplaceValue(inCancellationContext);
    };

    auto deferred = [&] {
        FailPointEnableBlock fpb("networkInterfaceHangCommandsAfterAcquireConn");

        auto deferred = startExhaustCommand(
            cbh, makeTestCommand(kMaxWait, makeEchoCmdObj()), std::move(callback));

        waitForHello();

        fpb->waitForTimesEntered(fpb.initialTimesEntered() + 1);

        inCancellationContext = true;
        net().cancelCommand(cbh);
        inCancellationContext = false;
        return deferred;
    }();

    auto result = deferred.getNoThrow();
    ASSERT_EQ(ErrorCodes::CallbackCanceled, result);
    bool cancellationRanInline = pf.future.get();
    ASSERT_FALSE(cancellationRanInline);
}

TEST_F(NetworkInterfaceTest, TearDownWaitsForInProgress) {
    boost::optional<stdx::thread> tearDownThread;
    auto tearDownPF = makePromiseFuture<void>();

    auto deferred = [&] {
        // Enable failpoint to make sure tearDown is blocked
        FailPointEnableBlock fpb("networkInterfaceFixtureHangOnCompletion");

        auto future = runCommand(makeCallbackHandle(), makeTestCommand(kMaxWait, makeEchoCmdObj()));

        // Wait for the completion of the command
        fpb->waitForTimesEntered(fpb.initialTimesEntered() + 1);

        tearDownThread.emplace([this, promise = std::move(tearDownPF.promise)]() mutable {
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

        return future;
    }();

    tearDownThread->join();

    ASSERT(deferred.isReady());
    ASSERT(tearDownPF.future.isReady());
    ASSERT_EQ(getInProgress(), 0);
}

TEST_F(NetworkInterfaceTest, RunCommandOnLeasedStream) {
    auto cs = fixture();
    auto target = cs.getServers().front();
    auto leasedStream = net().leaseStream(target, transport::kGlobalSSLMode, kNoTimeout).get();
    auto* client = leasedStream->getClient();

    auto request =
        RemoteCommandRequest(target, DatabaseName::kAdmin, makeEchoCmdObj(), nullptr, kNoTimeout);
    auto deferred = client->runCommandRequest(request);

    auto res = deferred.get();

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

TEST_F(NetworkInterfaceTest, ConnectionErrorAssociatedWithRemote) {
    FailPointEnableBlock fpb("connectionPoolReturnsErrorOnGet");

    auto cb = makeCallbackHandle();
    auto request = makeTestCommand(kNoTimeout, makeEchoCmdObj());
    auto deferred = runCommandOnAny(cb, request);

    auto result = deferred.get();

    ASSERT_EQ(ErrorCodes::HostUnreachable, result.status);
    ASSERT_EQ(result.target, fixture().getServers().front());
    assertNumOps(0u, 0u, 1u, 0u);
}

}  // namespace
}  // namespace executor
}  // namespace mongo
