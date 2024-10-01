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


#include <memory>

#include "mongo/bson/bsonmisc.h"
#include "mongo/client/async_client.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/client.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/asio/asio_transport_layer.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/scopeguard.h"

#include <asio.hpp>
#include <boost/none.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo::transport {
namespace {

class AsioTransportLayerTest : public unittest::Test {
private:
    unittest::MinimumLoggedSeverityGuard networkSeverityGuard{logv2::LogComponent::kNetwork,
                                                              logv2::LogSeverity::Debug(4)};
};

TEST_F(AsioTransportLayerTest, HTTPRequestGetsHTTPError) {
    auto connectionString = unittest::getFixtureConnectionString();
    auto server = connectionString.getServers().front();

    asio::io_context ioContext;
    asio::ip::tcp::resolver resolver(ioContext);
    asio::ip::tcp::socket socket(ioContext);

    LOGV2(23028, "Connecting to server", "server"_attr = server);
    auto resolverIt = resolver.resolve(server.host(), std::to_string(server.port()));
    asio::connect(socket, resolverIt);

    LOGV2(23029, "Sending HTTP request");
    std::string httpReq = str::stream() << "GET /\r\n"
                                           "Host: "
                                        << server
                                        << "\r\n"
                                           "User-Agent: MongoDB Integration test\r\n"
                                           "Accept: */*";
    asio::write(socket, asio::buffer(httpReq.data(), httpReq.size()));

    LOGV2(23030, "Waiting for response");
    std::array<char, 256> httpRespBuf;
    std::error_code ec;
    auto size = asio::read(socket, asio::buffer(httpRespBuf.data(), httpRespBuf.size()), ec);
    StringData httpResp(httpRespBuf.data(), size);

    LOGV2(23031, "Received http response", "response"_attr = httpResp);
    ASSERT_TRUE(httpResp.startsWith("HTTP/1.0 200 OK"));

// Why oh why can't ASIO unify their error codes
#ifdef _WIN32
    ASSERT_EQ(ec, asio::error::connection_reset);
#else
    ASSERT_EQ(ec, asio::error::eof);
#endif
}

class AsyncClientIntegrationTest : public AsioTransportLayerTest {
public:
    void setUp() override {
        auto connectionString = unittest::getFixtureConnectionString();
        auto server = connectionString.getServers().front();

        auto sc = getGlobalServiceContext();
        auto tl = sc->getTransportLayerManager()->getEgressLayer();
        _reactor = tl->getReactor(transport::TransportLayer::kNewReactor);
        _reactorThread = stdx::thread([&] {
            _reactor->run();
            _reactor->drain();
        });
    }

    void tearDown() override {
        _reactor->stop();
        _reactorThread.join();
    }

    static BSONObj assertOK(executor::RemoteCommandResponse resp) {
        ASSERT_OK(resp.status);
        ASSERT_OK(getStatusFromCommandResult(resp.data));
        return resp.data;
    };

    static HostAndPort getServer() {
        return unittest::getFixtureConnectionString().getServers().front();
    }

    ServiceContext* getServiceContext() {
        return getGlobalServiceContext();
    }

    std::shared_ptr<Reactor> getReactor() {
        return _reactor;
    }

    std::shared_ptr<AsyncDBClient> makeClient() {
        auto metrics =
            std::make_shared<ConnectionMetrics>(getGlobalServiceContext()->getFastClockSource());
        auto client = AsyncDBClient::connect(getServer(),
                                             transport::kGlobalSSLMode,
                                             getGlobalServiceContext(),
                                             _reactor,
                                             Milliseconds::max(),
                                             metrics)
                          .get();
        client->initWireVersion(__FILE__, nullptr).get();
        return client;
    }

    static executor::RemoteCommandRequest makeTestRequest(
        DatabaseName dbName,
        BSONObj cmdObj,
        boost::optional<UUID> clientOperationKey = boost::none,
        Milliseconds timeout = executor::RemoteCommandRequest::kNoTimeout) {
        if (clientOperationKey) {
            cmdObj = cmdObj.addField(
                BSON(GenericArguments::kClientOperationKeyFieldName << *clientOperationKey)
                    .firstElement());
        }

        return executor::RemoteCommandRequest(getServer(),
                                              std::move(dbName),
                                              std::move(cmdObj),
                                              BSONObj(),
                                              nullptr,
                                              timeout,
                                              {},
                                              clientOperationKey);
    }

    executor::RemoteCommandRequest makeExhaustHello(
        Milliseconds maxAwaitTime, boost::optional<UUID> clientOperationKey = boost::none) {
        // Send a dummy topologyVersion because the mongod generates this and sends it to the client
        // on the initial handshake.
        auto helloCmd =
            BSON("hello" << 1 << "maxAwaitTimeMS" << maxAwaitTime.count() << "topologyVersion"
                         << TopologyVersion(OID::max(), 0).toBSON());
        return makeTestRequest(DatabaseName::kAdmin, std::move(helloCmd), clientOperationKey);
    }

    class FailPointGuard {
    public:
        FailPointGuard(StringData fpName,
                       std::shared_ptr<AsyncDBClient> client,
                       int initalTimesEntered)
            : _fpName(fpName),
              _client(std::move(client)),
              _initialTimesEntered(initalTimesEntered) {}

        FailPointGuard(const FailPointGuard&) = delete;
        FailPointGuard& operator=(const FailPointGuard&) = delete;

        ~FailPointGuard() {
            auto cmdObj = BSON("configureFailPoint" << _fpName << "mode"
                                                    << "off");
            assertOK(
                _client->runCommandRequest(makeTestRequest(DatabaseName::kAdmin, cmdObj)).get());
        }

        void waitForTimesEntered(Interruptible* interruptible, int count) {
            auto cmdObj =
                BSON("waitForFailPoint" << _fpName << "timesEntered" << _initialTimesEntered + count
                                        << "maxTimeMS" << 30000);
            assertOK(_client->runCommandRequest(makeTestRequest(DatabaseName::kAdmin, cmdObj))
                         .get(interruptible));
        }

        void waitForTimesEntered(int count) {
            waitForTimesEntered(Interruptible::notInterruptible(), count);
        }

    private:
        std::string _fpName;
        std::shared_ptr<AsyncDBClient> _client;
        int _initialTimesEntered;
    };

    FailPointGuard configureFailPoint(const std::shared_ptr<AsyncDBClient>& client,
                                      StringData fp,
                                      BSONObj data) {
        auto configureFailPointRequest =
            makeTestRequest(DatabaseName::kAdmin,
                            BSON("configureFailPoint" << fp << "mode"
                                                      << "alwaysOn"
                                                      << "data" << data));
        auto resp = assertOK(client->runCommandRequest(configureFailPointRequest).get());
        return FailPointGuard(fp, client, resp.getField("count").Int());
    }

    FailPointGuard configureFailCommand(const std::shared_ptr<AsyncDBClient>& client,
                                        StringData failCommand,
                                        boost::optional<ErrorCodes::Error> errorCode = boost::none,
                                        boost::optional<Milliseconds> blockTime = boost::none) {
        auto data = BSON("failCommands" << BSON_ARRAY(failCommand));

        if (errorCode) {
            data = data.addField(BSON("errorCode" << *errorCode).firstElement());
        }

        if (blockTime) {
            data = data.addFields(
                BSON("blockConnection" << true << "blockTimeMS" << blockTime->count()));
        }
        return configureFailPoint(client, "failCommand", data);
    }

    void killOp(AsyncDBClient& client, UUID opKey) {
        auto killOp =
            makeTestRequest(DatabaseName::kAdmin,
                            BSON("_killOperations" << 1 << "operationKeys" << BSON_ARRAY(opKey)));
        assertOK(client.runCommandRequest(killOp).get());
    }

    /**
     * Returns a Baton that can be used to run commands on, or nullptr for reactor-only operation.
     */
    virtual BatonHandle baton() {
        return nullptr;
    }

    /** Returns an Interruptible appropriate for the Baton returned from baton(). */
    virtual Interruptible* interruptible() {
        return Interruptible::notInterruptible();
    }

    // Test case declarations.
    void testShortReadsAndWritesWork();
    void testAsyncConnectTimeoutCleansUpSocket();
    void testExhaustHelloShouldReceiveMultipleReplies();
    void testExhaustHelloShouldStopOnFailure();
    void testRunCommandRequest();
    void testRunCommandRequestCancel();
    void testRunCommandRequestCancelEarly();
    void testRunCommandRequestCancelSourceDismissed();
    void testRunCommandCancelBetweenSendAndRead();
    void testExhaustCommand();
    void testBeginExhaustCommandRequestCancel();
    void testBeginExhaustCommandCancelEarly();
    void testAwaitExhaustCommandCancel();
    void testAwaitExhaustCommandCancelEarly();

private:
    ReactorHandle _reactor;
    stdx::thread _reactorThread;
};

class AsyncClientIntegrationTestWithBaton : public AsyncClientIntegrationTest {
public:
    void setUp() override {
        AsyncClientIntegrationTest::setUp();
        _client = getGlobalServiceContext()->getService()->makeClient("BatonClient");
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
    ServiceContext::UniqueClient _client;
    ServiceContext::UniqueOperationContext _opCtx;
    BatonHandle _baton;
};

using AsyncClientIntegrationTestWithoutBaton = AsyncClientIntegrationTest;

#define TEST_WITH_AND_WITHOUT_BATON_F(suite, name) \
    TEST_F(suite##WithBaton, name) {               \
        test##name();                              \
    }                                              \
    TEST_F(suite##WithoutBaton, name) {            \
        test##name();                              \
    }                                              \
    void suite::test##name()

// This test forces reads and writes to occur one byte at a time, verifying SERVER-34506
// (the isJustForContinuation optimization works).
//
// Because of the file size limit, it's only an effective check on debug builds (where the future
// implementation checks the length of the future chain).
TEST_WITH_AND_WITHOUT_BATON_F(AsyncClientIntegrationTest, ShortReadsAndWritesWork) {
    auto dbClient = makeClient();

    FailPointEnableBlock fp("asioTransportLayerShortOpportunisticReadWrite");

    auto req = makeTestRequest(DatabaseName::kAdmin, BSON("ping" << 1));
    assertOK(dbClient->runCommandRequest(req, baton()).get(interruptible()));
}

TEST_WITH_AND_WITHOUT_BATON_F(AsyncClientIntegrationTest, AsyncConnectTimeoutCleansUpSocket) {
    FailPointEnableBlock fp("asioTransportLayerAsyncConnectTimesOut");
    auto metrics = std::make_shared<ConnectionMetrics>(getServiceContext()->getFastClockSource());
    auto client = AsyncDBClient::connect(getServer(),
                                         transport::kGlobalSSLMode,
                                         getServiceContext(),
                                         getReactor(),
                                         Milliseconds{500},
                                         metrics)
                      .getNoThrow();
    ASSERT_EQ(client.getStatus(), ErrorCodes::NetworkTimeout);
}

TEST_WITH_AND_WITHOUT_BATON_F(AsyncClientIntegrationTest,
                              ExhaustHelloShouldReceiveMultipleReplies) {
    auto client = makeClient();

    auto req = makeExhaustHello(Milliseconds(100));

    Future<executor::RemoteCommandResponse> beginExhaustFuture =
        client->beginExhaustCommandRequest(req, baton());

    Date_t prevTime;
    TopologyVersion topologyVersion;
    {
        auto reply = beginExhaustFuture.get(interruptible());
        ASSERT(reply.moreToCome);
        assertOK(reply);
        prevTime = reply.data.getField("localTime").Date();
        topologyVersion = TopologyVersion::parse(IDLParserContext("TopologyVersion"),
                                                 reply.data.getField("topologyVersion").Obj());
    }

    Future<executor::RemoteCommandResponse> awaitExhaustFuture =
        client->awaitExhaustCommand(baton());
    {
        auto reply = awaitExhaustFuture.get(interruptible());
        assertOK(reply);
        ASSERT(reply.moreToCome);
        auto replyTime = reply.data.getField("localTime").Date();
        ASSERT_GT(replyTime, prevTime);

        auto replyTopologyVersion = TopologyVersion::parse(
            IDLParserContext("TopologyVersion"), reply.data.getField("topologyVersion").Obj());
        ASSERT_EQ(replyTopologyVersion.getProcessId(), topologyVersion.getProcessId());
        ASSERT_EQ(replyTopologyVersion.getCounter(), topologyVersion.getCounter());
    }

    Future<executor::RemoteCommandResponse> cancelExhaustFuture =
        client->awaitExhaustCommand(baton());
    {
        client->cancel();
        client->end();
        auto swReply = cancelExhaustFuture.getNoThrow(interruptible());

        // The original hello request has maxAwaitTimeMs = 1000 ms, if the cancel executes
        // before the 1000ms then we expect the future to resolve with an error. It should
        // resolve with CallbackCanceled unless the socket is already closed, in which case
        // it will resolve with HostUnreachable. If the network is slow, the server may
        // response before the cancel executes however.
        if (!swReply.getStatus().isOK()) {
            ASSERT((swReply.getStatus() == ErrorCodes::CallbackCanceled) ||
                   (swReply.getStatus() == ErrorCodes::HostUnreachable));
        }
    }
}

TEST_WITH_AND_WITHOUT_BATON_F(AsyncClientIntegrationTest, ExhaustHelloShouldStopOnFailure) {
    auto client = makeClient();
    auto fpClient = makeClient();

    // Turn on the failCommand fail point for hello
    auto fpGuard = configureFailCommand(fpClient, "hello", ErrorCodes::CommandFailed);

    auto helloReq = makeExhaustHello(Seconds(1));

    auto resp = client->beginExhaustCommandRequest(helloReq, baton()).get(interruptible());
    ASSERT_OK(resp.status);
    ASSERT(!resp.moreToCome);
    ASSERT_EQ(getStatusFromCommandResult(resp.data), ErrorCodes::CommandFailed);
}

TEST_WITH_AND_WITHOUT_BATON_F(AsyncClientIntegrationTest, RunCommandRequest) {
    auto client = makeClient();
    auto req = makeTestRequest(DatabaseName::kAdmin, BSON("echo" << std::string(1 << 10, 'x')));
    assertOK(client->runCommandRequest(req, baton()).get(interruptible()));
}

TEST_WITH_AND_WITHOUT_BATON_F(AsyncClientIntegrationTest, RunCommandRequestCancel) {
    auto fpClient = makeClient();
    CancellationSource cancelSource;

    auto fpGuard = configureFailCommand(fpClient, "ping", boost::none, Milliseconds(60000));

    auto pf = makePromiseFuture<executor::RemoteCommandResponse>();
    auto cmdThread = stdx::thread([&] {
        // Limit dbClient's scope to this thread to test the cancellation callback outliving
        // the caller's client.
        auto dbClient = makeClient();

        auto opKey = UUID::gen();
        auto req = makeTestRequest(DatabaseName::kAdmin, BSON("ping" << 1), opKey);
        auto fut = dbClient->runCommandRequest(req, baton(), nullptr, cancelSource.token());
        ON_BLOCK_EXIT([&]() { killOp(*fpClient, opKey); });

        pf.promise.setFrom(fut.getNoThrow(interruptible()));
    });

    auto client = getGlobalServiceContext()->getService()->makeClient(__FILE__);
    auto opCtx = client->makeOperationContext();
    opCtx->setDeadlineAfterNowBy(Seconds(30), ErrorCodes::ExceededTimeLimit);

    fpGuard.waitForTimesEntered(opCtx.get(), 1);
    cancelSource.cancel();

    ASSERT_EQ(pf.future.getNoThrow(opCtx.get()), ErrorCodes::CallbackCanceled);
    cmdThread.join();
}

TEST_WITH_AND_WITHOUT_BATON_F(AsyncClientIntegrationTest, RunCommandRequestCancelEarly) {
    auto client = makeClient();
    CancellationSource cancelSource;

    cancelSource.cancel();

    auto req = makeTestRequest(DatabaseName::kAdmin,
                               BSON("echo"
                                    << "RunCommandRequestCancelEarly"));
    auto fut = client->runCommandRequest(req, baton(), nullptr, cancelSource.token());
    ASSERT(fut.isReady());
    ASSERT_EQ(fut.getNoThrow(interruptible()), ErrorCodes::CallbackCanceled);
}

TEST_WITH_AND_WITHOUT_BATON_F(AsyncClientIntegrationTest, RunCommandRequestCancelSourceDismissed) {
    auto client = makeClient();
    auto fpClient = makeClient();
    auto cancelSource = boost::make_optional(CancellationSource());

    auto fpGuard = configureFailCommand(fpClient, "echo", boost::none, Milliseconds(1000));

    auto req = makeTestRequest(DatabaseName::kAdmin,
                               BSON("echo"
                                    << "RunCommandRequestCancelSourceDismissed"));

    auto runCommandFuture = client->runCommandRequest(req, baton(), nullptr, cancelSource->token());
    cancelSource.reset();
    assertOK(runCommandFuture.get(interruptible()));
}

// Tests that cancellation doesn't "miss" if it happens to occur between sending a command request
// and reading its response.
TEST_WITH_AND_WITHOUT_BATON_F(AsyncClientIntegrationTest, RunCommandCancelBetweenSendAndRead) {
    auto client = makeClient();
    CancellationSource cancelSource;

    boost::optional<FailPointEnableBlock> fpb("hangBeforeReadResponse");

    auto pf = makePromiseFuture<executor::RemoteCommandResponse>();
    auto commandThread = stdx::thread([&] {
        // Run runCommandRequest in a separate thread in case hanging on the failpoint
        // happens to run on the current thread due to the sending future being immediately
        // available.
        auto req = makeTestRequest(DatabaseName::kAdmin, BSON("ping" << 1));
        pf.promise.setFrom(client->runCommandRequest(req, baton(), nullptr, cancelSource.token())
                               .getNoThrow(interruptible()));
    });

    fpb.get()->waitForTimesEntered(fpb->initialTimesEntered() + 1);
    cancelSource.cancel();
    fpb.reset();

    ASSERT_EQ(pf.future.getNoThrow(), ErrorCodes::CallbackCanceled);
    commandThread.join();
}

TEST_WITH_AND_WITHOUT_BATON_F(AsyncClientIntegrationTest, ExhaustCommand) {
    auto client = makeClient();
    auto opKey = UUID::gen();

    auto helloReq = makeExhaustHello(Milliseconds(100), opKey);
    assertOK(client->beginExhaustCommandRequest(helloReq, baton()).get(interruptible()));

    for (auto i = 0; i < 3; i++) {
        assertOK(client->awaitExhaustCommand(baton()).get(interruptible()));
    }

    auto killOpClient = makeClient();
    killOp(*killOpClient, opKey);

    auto now = getGlobalServiceContext()->getFastClockSource()->now();
    for (auto deadline = now + Seconds(30); now < deadline;
         now = getGlobalServiceContext()->getFastClockSource()->now()) {
        auto finalResp = client->awaitExhaustCommand(baton()).get(interruptible());
        ASSERT_OK(finalResp.status);
        if (getStatusFromCommandResult(finalResp.data).code() == ErrorCodes::Interrupted) {
            break;
        }
    }

    assertOK(
        client->runCommandRequest(makeTestRequest(DatabaseName::kAdmin, BSON("ping" << 1)), baton())
            .get(interruptible()));
}

TEST_WITH_AND_WITHOUT_BATON_F(AsyncClientIntegrationTest, BeginExhaustCommandRequestCancel) {
    auto client = makeClient();
    auto fpClient = makeClient();
    CancellationSource cancelSource;

    auto fpGuard = configureFailCommand(fpClient, "hello", boost::none, Milliseconds(60000));

    UUID operationKey = UUID::gen();
    auto helloReq = makeExhaustHello(Milliseconds(100), operationKey);
    auto beginExhaustFuture =
        client->beginExhaustCommandRequest(helloReq, baton(), cancelSource.token());
    ON_BLOCK_EXIT([&]() { killOp(*fpClient, operationKey); });

    fpGuard.waitForTimesEntered(1);
    cancelSource.cancel();
    ASSERT_EQ(beginExhaustFuture.getNoThrow(interruptible()), ErrorCodes::CallbackCanceled);
}

TEST_WITH_AND_WITHOUT_BATON_F(AsyncClientIntegrationTest, BeginExhaustCommandCancelEarly) {
    auto client = makeClient();
    CancellationSource cancelSource;
    cancelSource.cancel();

    auto beginExhaustFuture = client->beginExhaustCommandRequest(
        makeExhaustHello(Milliseconds(100)), baton(), cancelSource.token());
    ASSERT(beginExhaustFuture.isReady());
    ASSERT_EQ(beginExhaustFuture.getNoThrow(interruptible()), ErrorCodes::CallbackCanceled);
}

TEST_WITH_AND_WITHOUT_BATON_F(AsyncClientIntegrationTest, AwaitExhaustCommandCancel) {
    auto client = makeClient();
    CancellationSource cancelSource;

    // Use long maxAwaitTimeMS.
    auto helloReq = makeExhaustHello(Milliseconds(30000));
    Future<executor::RemoteCommandResponse> beginExhaustFuture =
        client->beginExhaustCommandRequest(helloReq, baton(), cancelSource.token());

    // The first response should come in quickly.
    assertOK(beginExhaustFuture.get(interruptible()));

    // The second response will take maxAwaitTimeMS.
    Future<executor::RemoteCommandResponse> awaitExhaustFuture =
        client->awaitExhaustCommand(baton(), cancelSource.token());
    cancelSource.cancel();
    ASSERT_EQ(awaitExhaustFuture.getNoThrow(interruptible()), ErrorCodes::CallbackCanceled);
}

TEST_WITH_AND_WITHOUT_BATON_F(AsyncClientIntegrationTest, AwaitExhaustCommandCancelEarly) {
    auto client = makeClient();
    CancellationSource cancelSource;

    auto helloReq = makeExhaustHello(Milliseconds(100));
    Future<executor::RemoteCommandResponse> beginExhaustFuture =
        client->beginExhaustCommandRequest(helloReq, baton(), cancelSource.token());
    assertOK(beginExhaustFuture.get(interruptible()));

    cancelSource.cancel();
    Future<executor::RemoteCommandResponse> awaitExhaustFuture =
        client->awaitExhaustCommand(baton(), cancelSource.token());
    ASSERT(awaitExhaustFuture.isReady());
    ASSERT_EQ(awaitExhaustFuture.getNoThrow(interruptible()), ErrorCodes::CallbackCanceled);
}

}  // namespace
}  // namespace mongo::transport
