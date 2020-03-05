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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include <algorithm>
#include <exception>
#include <memory>

#include "mongo/base/status_with.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_integration_fixture.h"
#include "mongo/executor/test_network_connection_hook.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/stdx/future.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

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
    assertCommandOK("admin", BSON("ping" << 1));
}

TEST_F(NetworkInterfaceIntegrationFixture, PingWithoutStartup) {
    createNet();

    RemoteCommandRequest request{
        fixture().getServers()[0], "admin", BSON("ping" << 1), BSONObj(), nullptr, Minutes(5)};

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
                                                          "admin",
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
    RemoteCommandRequest request{
        fixture().getServers()[0], "admin", BSON("ping" << 1), BSONObj(), nullptr, Seconds(1)};
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
        setTestCommandsEnabled(true);
        startNet(std::make_unique<WaitForIsMasterHook>(this));
    }

    RemoteCommandRequest makeTestCommand(
        Milliseconds timeout,
        BSONObj cmd,
        OperationContext* opCtx = nullptr,
        boost::optional<RemoteCommandRequest::HedgeOptions> hedgeOptions = boost::none,
        RemoteCommandRequest::FireAndForgetMode fireAndForgetMode =
            RemoteCommandRequest::FireAndForgetMode::kOff) {
        auto cs = fixture();
        return RemoteCommandRequest(cs.getServers().front(),
                                    "admin",
                                    std::move(cmd),
                                    BSONObj(),
                                    opCtx,
                                    timeout,
                                    hedgeOptions,
                                    fireAndForgetMode);
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

    /**
     * Returns true if the given command is still running.
     */
    bool isCommandRunning(const std::string command) {
        const auto cmdObj =
            BSON("aggregate" << 1 << "pipeline"
                             << BSON_ARRAY(BSON("$currentOp" << BSON("localOps" << true))
                                           << BSON("$match" << BSON(("command." + command)
                                                                    << BSON("$exists" << true))))
                             << "cursor" << BSONObj());
        auto cs = fixture();
        RemoteCommandRequest request{
            cs.getServers().front(), "admin", cmdObj, BSONObj(), nullptr, kNoTimeout};
        auto res = runCommandSync(request);

        ASSERT_OK(res.status);
        ASSERT_OK(getStatusFromCommandResult(res.data));
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

    struct IsMasterData {
        BSONObj request;
        RemoteCommandResponse response;
    };
    IsMasterData waitForIsMaster() {
        stdx::unique_lock<Latch> lk(_mutex);
        _isMasterCond.wait(lk, [this] { return _isMasterResult != boost::none; });

        return std::move(*_isMasterResult);
    }

    bool hasIsMaster() {
        stdx::lock_guard<Latch> lk(_mutex);
        return _isMasterResult != boost::none;
    }

private:
    class WaitForIsMasterHook : public NetworkConnectionHook {
    public:
        explicit WaitForIsMasterHook(NetworkInterfaceTest* parent) : _parent(parent) {}

        Status validateHost(const HostAndPort& host,
                            const BSONObj& request,
                            const RemoteCommandResponse& isMasterReply) override {
            stdx::lock_guard<Latch> lk(_parent->_mutex);
            _parent->_isMasterResult = IsMasterData{request, isMasterReply};
            _parent->_isMasterCond.notify_all();
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
    stdx::condition_variable _isMasterCond;
    boost::optional<IsMasterData> _isMasterResult;
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

        waitForIsMaster();

        fpb->waitForTimesEntered(fpb.initialTimesEntered() + 1);

        net().cancelCommand(cbh);

        return deferred;
    }();

    // Wait for op to complete, assert that it was canceled.
    auto result = deferred.get();
    ASSERT_EQ(ErrorCodes::CallbackCanceled, result.status);
    ASSERT(result.elapsedMillis);

    assertNumOps(1u, 0u, 0u, 0u);
}

TEST_F(NetworkInterfaceTest, CancelRemotely) {
    // Enable blockConnection for "echo".
    assertCommandOK("admin",
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
        assertCommandOK("admin",
                        BSON("configureFailPoint"
                             << "failCommand"
                             << "mode"
                             << "off"),
                        kNoTimeout);
    });

    int numCurrentOpRan = 0;

    auto cbh = makeCallbackHandle();
    auto deferred = [&] {
        // Kick off an "echo" operation, which should block until cancelCommand causes
        // the operation to be killed.
        auto deferred = runCommand(cbh,
                                   makeTestCommand(kNoTimeout,
                                                   makeEchoCmdObj(),
                                                   nullptr /* opCtx */,
                                                   RemoteCommandRequest::HedgeOptions()));

        // Wait for the "echo" operation to start.
        numCurrentOpRan += waitForCommandToStart("echo", kMaxWait);

        // Run cancelCommand to kill the above operation.
        net().cancelCommand(cbh);

        return deferred;
    }();

    // Wait for the command to return, assert that it was canceled.
    auto result = deferred.get();
    ASSERT_EQ(ErrorCodes::CallbackCanceled, result.status);
    ASSERT(result.elapsedMillis);

    // Wait for the operation to be killed on the remote host.
    numCurrentOpRan += waitForCommandToStop("echo", kMaxWait);

    // We have one canceled operation (echo), and two other succeeded operations
    // on top of the currentOp operations (configureFailPoint and _killOperations).
    assertNumOps(1u, 0u, 0u, 2u + numCurrentOpRan);
}

TEST_F(NetworkInterfaceTest, CancelRemotelyTimedOut) {
    // Enable blockConnection for "echo" and "_killOperations".
    assertCommandOK("admin",
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
        assertCommandOK("admin",
                        BSON("configureFailPoint"
                             << "failCommand"
                             << "mode"
                             << "off"),
                        kNoTimeout);
    });

    int numCurrentOpRan = 0;

    auto cbh = makeCallbackHandle();
    auto deferred = [&] {
        // Kick off a blocking "echo" operation.
        auto deferred = runCommand(cbh,
                                   makeTestCommand(kNoTimeout,
                                                   makeEchoCmdObj(),
                                                   nullptr /* opCtx */,
                                                   RemoteCommandRequest::HedgeOptions()));

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
    ASSERT(result.elapsedMillis);

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
    ASSERT(result.elapsedMillis);
    assertNumOps(1u, 0u, 0u, 0u);
}

TEST_F(NetworkInterfaceTest, LateCancel) {
    auto cbh = makeCallbackHandle();

    auto deferred = runCommand(cbh, makeTestCommand(kMaxWait, makeEchoCmdObj()));

    // Wait for op to complete, assert that it was canceled.
    auto result = deferred.get();
    net().cancelCommand(cbh);

    ASSERT_OK(result.status);
    ASSERT(result.elapsedMillis);
    assertNumOps(0u, 0u, 0u, 1u);
}

TEST_F(NetworkInterfaceTest, AsyncOpTimeout) {
    // Kick off operation
    auto cb = makeCallbackHandle();
    auto request = makeTestCommand(Milliseconds{1000}, makeSleepCmdObj());
    auto deferred = runCommand(cb, request);

    waitForIsMaster();

    auto result = deferred.get();

    // mongos doesn't implement the ping command, so ignore the response there, otherwise
    // check that we've timed out.
    if (!pingCommandMissing(result)) {
        ASSERT_EQ(ErrorCodes::NetworkInterfaceExceededTimeLimit, result.status);
        ASSERT(result.elapsedMillis);
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
    opCtx->setDeadlineAfterNowBy(opCtxDeadline, ErrorCodes::ExceededTimeLimit);

    auto request = makeTestCommand(requestTimeout, makeSleepCmdObj(), opCtx.get());

    auto deferred = runCommand(cb, request);

    waitForIsMaster();

    auto result = deferred.get();

    // mongos doesn't implement the ping command, so ignore the response there, otherwise
    // check that we've timed out.
    if (pingCommandMissing(result)) {
        return;
    }

    ASSERT_EQ(ErrorCodes::NetworkInterfaceExceededTimeLimit, result.status);
    ASSERT(result.elapsedMillis);
    // check that the request timeout uses the smaller of the operation context deadline and
    // the timeout specified in the request constructor.
    ASSERT_GTE(result.elapsedMillis.value(), opCtxDeadline);
    ASSERT_LT(result.elapsedMillis.value(), requestTimeout);
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
    opCtx->setDeadlineAfterNowBy(opCtxDeadline, ErrorCodes::ExceededTimeLimit);
    auto request = makeTestCommand(requestTimeout, makeSleepCmdObj(), opCtx.get());

    auto deferred = runCommand(cb, request);

    waitForIsMaster();

    auto result = deferred.get();

    // mongos doesn't implement the ping command, so ignore the response there, otherwise
    // check that we've timed out.
    if (pingCommandMissing(result)) {
        return;
    }

    ASSERT_EQ(ErrorCodes::NetworkInterfaceExceededTimeLimit, result.status);
    ASSERT(result.elapsedMillis);
    // check that the request timeout uses the smaller of the operation context deadline and
    // the timeout specified in the request constructor.
    ASSERT_GTE(result.elapsedMillis.value(), requestTimeout);
    ASSERT_LT(result.elapsedMillis.value(), opCtxDeadline);
    assertNumOps(0u, 1u, 0u, 0u);
}

TEST_F(NetworkInterfaceTest, StartCommand) {
    auto request = makeTestCommand(
        kNoTimeout, makeEchoCmdObj(), nullptr /* opCtx */, RemoteCommandRequest::HedgeOptions());

    auto deferred = runCommand(makeCallbackHandle(), std::move(request));

    auto res = deferred.get();

    ASSERT(res.elapsedMillis);
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
    assertCommandOK("admin",
                    BSON("configureFailPoint"
                         << "failCommand"
                         << "mode"
                         << "alwaysOn"
                         << "data"
                         << BSON("errorCode" << ErrorCodes::CommandFailed << "failCommands"
                                             << BSON_ARRAY("echo"))));

    ON_BLOCK_EXIT([&] {
        assertCommandOK("admin",
                        BSON("configureFailPoint"
                             << "failCommand"
                             << "mode"
                             << "off"));
    });

    // Run fireAndForget commands and verify that we get status OK responses.
    const int numFireAndForgetRequests = 3;
    std::vector<Future<RemoteCommandResponse>> futures;

    for (int i = 0; i < numFireAndForgetRequests; i++) {
        auto cbh = makeCallbackHandle();
        auto fireAndForgetRequest = makeTestCommand(kNoTimeout,
                                                    makeEchoCmdObj(),
                                                    nullptr /* opCtx */,
                                                    boost::none /* hedgeOptions */,
                                                    RemoteCommandRequest::FireAndForgetMode::kOn);
        futures.push_back(runCommand(cbh, fireAndForgetRequest));
    }

    for (auto& future : futures) {
        auto result = future.get();
        ASSERT(result.elapsedMillis);
        uassertStatusOK(result.status);
        ASSERT_EQ(1, result.data.getIntField("ok"));
    }

    // Run a non-fireAndForget command and verify that we get a CommandFailed response.
    auto nonFireAndForgetRequest = makeTestCommand(kNoTimeout, makeEchoCmdObj());
    auto result = runCommandSync(nonFireAndForgetRequest);
    ASSERT(result.elapsedMillis);
    uassertStatusOK(result.status);
    ASSERT_EQ(0, result.data.getIntField("ok"));
    ASSERT_EQ(ErrorCodes::CommandFailed, result.data.getIntField("code"));
    assertNumOps(0u, 0u, 0u, 5u);
}

TEST_F(NetworkInterfaceTest, StartCommandOnAny) {
    auto commandRequest = makeEchoCmdObj();
    auto request = [&] {
        auto cs = fixture();
        RemoteCommandRequestBase::HedgeOptions ho;
        ho.count = 1;

        return RemoteCommandRequestOnAny({cs.getServers()},
                                         "admin",
                                         std::move(commandRequest),
                                         BSONObj(),
                                         nullptr,
                                         RemoteCommandRequest::kNoTimeout,
                                         ho);
    }();

    auto deferred = runCommandOnAny(makeCallbackHandle(), std::move(request));
    auto res = deferred.get();

    auto cmdObj = res.data.getObjectField("echo");
    uassertStatusOK(res.status);
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

TEST_F(NetworkInterfaceTest, IsMasterRequestContainsOutgoingWireVersionInternalClientInfo) {
    WireSpec::instance().isInternalClient = true;

    auto deferred = runCommand(makeCallbackHandle(), makeTestCommand(kNoTimeout, makeEchoCmdObj()));
    auto isMasterHandshake = waitForIsMaster();

    // Verify that the isMaster reply has the expected internalClient data.
    auto internalClientElem = isMasterHandshake.request["internalClient"];
    ASSERT_EQ(internalClientElem.type(), BSONType::Object);
    auto minWireVersionElem = internalClientElem.Obj()["minWireVersion"];
    auto maxWireVersionElem = internalClientElem.Obj()["maxWireVersion"];
    ASSERT_EQ(minWireVersionElem.type(), BSONType::NumberInt);
    ASSERT_EQ(maxWireVersionElem.type(), BSONType::NumberInt);
    ASSERT_EQ(minWireVersionElem.numberInt(), WireSpec::instance().outgoing.minWireVersion);
    ASSERT_EQ(maxWireVersionElem.numberInt(), WireSpec::instance().outgoing.maxWireVersion);

    // Verify that the ping op is counted as a success.
    auto res = deferred.get();
    ASSERT(res.elapsedMillis);
    assertNumOps(0u, 0u, 0u, 1u);
}

TEST_F(NetworkInterfaceTest, IsMasterRequestMissingInternalClientInfoWhenNotInternalClient) {
    WireSpec::instance().isInternalClient = false;

    auto deferred = runCommand(makeCallbackHandle(), makeTestCommand(kNoTimeout, makeEchoCmdObj()));
    auto isMasterHandshake = waitForIsMaster();

    // Verify that the isMaster reply has the expected internalClient data.
    ASSERT_FALSE(isMasterHandshake.request["internalClient"]);
    // Verify that the ping op is counted as a success.
    auto res = deferred.get();
    ASSERT(res.elapsedMillis);
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
    assertCommandOK("admin", configureFailpointCmd);

    ON_BLOCK_EXIT([&] {
        auto stopFpRequest = BSON("configureFailPoint"
                                  << "failCommand"
                                  << "mode"
                                  << "off");
        assertCommandOK("admin", stopFpRequest);
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

}  // namespace
}  // namespace executor
}  // namespace mongo
