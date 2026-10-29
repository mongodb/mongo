// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/dbclient_grpc_stream.h"

#include "mongo/transport/grpc/grpc_transport_layer_mock.h"
#include "mongo/transport/grpc/mock_wire_version_provider.h"
#include "mongo/transport/grpc/test_fixtures.h"
#include "mongo/transport/transport_layer_manager_impl.h"

/**
 * This file contains tests for DBClientGRPCStream. It utilizes the mocking framework provided by
 * the gRPC TL to mock the gRPC transport layer.
 */
namespace mongo {
namespace {

using namespace transport::grpc;

class DBClientGRPCTest : public ServiceContextTest {
public:
    inline static const HostAndPort kServerHostAndPort = HostAndPort("localhost", 12345);

    void setUp() override {
        ServiceContextTest::setUp();

        // Mock resolver that automatically returns the producer end of the test's pipe.
        auto resolver = [&](const HostAndPort&) -> MockRPCQueue::Producer {
            return _pipe.producer;
        };

        auto tl = std::make_unique<GRPCTransportLayerMock>(
            getServiceContext(),
            CommandServiceTestFixtures::makeTLOptions(),
            resolver,
            HostAndPort(MockStubTestFixtures::kClientAddress));

        getServiceContext()->setTransportLayerManager(
            std::make_unique<transport::TransportLayerManagerImpl>(std::move(tl)));
        uassertStatusOK(getServiceContext()->getTransportLayerManager()->setup());
        uassertStatusOK(getServiceContext()->getTransportLayerManager()->start());

        _server = std::make_unique<MockServer>(std::move(_pipe.consumer));
    }

    void tearDown() override {
        getServiceContext()->getTransportLayerManager()->shutdown();
        ServiceContextTest::tearDown();
    }

    Message helloResponse() {
        OpMsg response;
        WireVersionInfo wv = WireSpec::getWireSpec(getServiceContext()).getIncomingExternalClient();
        BSONObjBuilder bob;
        bob.append("ok", 1);
        WireVersionInfo::appendToBSON(wv, &bob);
        response.body = bob.obj();
        return response.serialize();
    }

    OpMsgRequest pingRequest() {
        OpMsgRequest request;
        request.body = BSON("msg" << "ping");
        return request;
    }

    Message pongResponse() {
        OpMsg response;
        response.body = BSON("ok" << 1 << "msg"
                                  << "pong");
        return response.serialize();
    }

    void confirmHelloAndRespond(std::shared_ptr<mongo::transport::grpc::GRPCSession> session) {
        ASSERT_EQ(session->remote().toString(), MockStubTestFixtures::kClientAddress);
        auto msg = session->sourceMessage();
        ASSERT_OK(msg);
        OpMsg request = OpMsg::parse(msg.getValue());
        ASSERT_EQ(request.body["hello"].numberInt(), 1);
        ASSERT_OK(session->sinkMessage(helloResponse()));
    }

    void confirmPingAndRespondPong(std::shared_ptr<mongo::transport::grpc::GRPCSession> session) {
        ASSERT_EQ(session->remote().toString(), MockStubTestFixtures::kClientAddress);
        auto ping = session->sourceMessage();
        ASSERT_OK(ping);
        OpMsg pingRequest = OpMsg::parse(ping.getValue());
        ASSERT_EQ(pingRequest.body["msg"].str(), "ping");
        ASSERT_OK(session->sinkMessage(pongResponse()));
    }

    void runTest(
        CommandService::RPCHandler serverCb,
        std::function<void(DBClientGRPCStream&)> clientThread,
        std::shared_ptr<WireVersionProvider> wvProvider = std::make_unique<WireVersionProvider>()) {

        unittest::threadAssertionMonitoredTest([&](unittest::ThreadAssertionMonitor& monitor) {
            _server->start(monitor, serverCb, wvProvider);
            ON_BLOCK_EXIT([&] { _server->shutdown(); });

            DBClientGRPCStream dbclient =
                DBClientGRPCStream(/* authToken */ boost::none, /* autoReconnect */ true);
            ASSERT_DOES_NOT_THROW(clientThread(dbclient));
        });
    }

private:
    MockRPCQueue::Pipe _pipe;
    std::unique_ptr<MockServer> _server;
    unittest::MinimumLoggedSeverityGuard logSeverityGuardNetwork{logv2::LogComponent::kNetwork,
                                                                 logv2::LogSeverity::Debug(4)};
};

TEST_F(DBClientGRPCTest, BasicConnect) {
    auto serverCb = [&](auto session) {
        ON_BLOCK_EXIT([&] { session->setTerminationStatus(Status::OK()); });

        confirmHelloAndRespond(session);
    };

    auto clientThread = [&](DBClientGRPCStream& dbclient) {
        dbclient.connect(kServerHostAndPort, "test", boost::none);
        ON_BLOCK_EXIT([&] { dbclient.shutdown(); });
        ASSERT_TRUE(dbclient.isStillConnected());
    };

    runTest(serverCb, clientThread);
}

TEST_F(DBClientGRPCTest, BasicRunCommand) {
    auto serverCb = [&](auto session) {
        ON_BLOCK_EXIT([&] { session->setTerminationStatus(Status::OK()); });

        confirmHelloAndRespond(session);
        confirmPingAndRespondPong(session);
    };

    auto clientThread = [&](DBClientGRPCStream& dbclient) {
        dbclient.connect(kServerHostAndPort, "test", boost::none);
        ON_BLOCK_EXIT([&] { dbclient.shutdown(); });
        auto reply = dbclient.runCommand(pingRequest())->getCommandReply();
        ASSERT_OK(getStatusFromCommandResult(reply));
    };

    runTest(serverCb, clientThread);
}

TEST_F(DBClientGRPCTest, BasicConnectNoHelloWithCommand) {
    auto serverCb = [&](auto session) {
        ON_BLOCK_EXIT([&] { session->setTerminationStatus(Status::OK()); });

        // We should only get a ping.
        confirmPingAndRespondPong(session);
    };

    auto clientThread = [&](DBClientGRPCStream& dbclient) {
        dbclient.connectNoHello(kServerHostAndPort, boost::none);
        ON_BLOCK_EXIT([&] { dbclient.shutdown(); });
        ASSERT_TRUE(dbclient.isStillConnected());
        auto reply = dbclient.runCommand(pingRequest())->getCommandReply();
        ASSERT_OK(getStatusFromCommandResult(reply));
    };

    runTest(serverCb, clientThread);
}

TEST_F(DBClientGRPCTest, GetMaxWireVersionCallsClusterMaxWireVersion) {
    const int kDefaultMaxWireVersion = 10;
    const int kNewMaxWireVersion = 70;
    // Make the server use a mocked wire version to ensure that the client actually receives the
    // wire version from the server.
    auto wvProvider = std::make_shared<MockWireVersionProvider>();
    wvProvider->setClusterMaxWireVersion(kNewMaxWireVersion);

    auto serverCb = [&](auto session) {
        ON_BLOCK_EXIT([&] { session->setTerminationStatus(Status::OK()); });
        confirmHelloAndRespond(session);
    };

    auto clientThread = [&](DBClientGRPCStream& dbclient) {
        // Before connecting, it should fall back to called DBClientSession::getMaxWireVersion().
        ASSERT_EQ(dbclient.getMaxWireVersion(), 0);
        dbclient.setWireVersions(0, kDefaultMaxWireVersion);
        ASSERT_EQ(dbclient.getMaxWireVersion(), kDefaultMaxWireVersion);

        // After connecting, returns the value of EgressSession::getClusterMaxWireVersion(), as
        // determined by the value received by the server hello.
        dbclient.connect(kServerHostAndPort, "test", boost::none);
        ON_BLOCK_EXIT([&] { dbclient.shutdown(); });
        ASSERT_EQ(dbclient.getMaxWireVersion(), kNewMaxWireVersion);
    };

    runTest(serverCb, clientThread, wvProvider);
}

TEST_F(DBClientGRPCTest, AutoReconnectionSucceeds) {
    Atomic<bool> hasBeenCancelledOnce(false);
    auto serverCb = [&](auto session) {
        // Cancel the first RPC.
        if (!hasBeenCancelledOnce.load()) {
            session->setTerminationStatus(Status(ErrorCodes::CallbackCanceled, "test"));
            hasBeenCancelledOnce.store(true);
            return;
        }

        ON_BLOCK_EXIT([&] { session->setTerminationStatus(Status::OK()); });
        // Respond hello to the second connection request, and then respond to the ping.
        confirmHelloAndRespond(session);
        confirmPingAndRespondPong(session);
    };

    auto clientThread = [&](DBClientGRPCStream& dbclient) {
        // Initially, connect fails.
        ASSERT_THROWS(dbclient.connect(kServerHostAndPort, "test", boost::none), DBException);
        ON_BLOCK_EXIT([&] { dbclient.shutdown(); });
        ASSERT_TRUE(dbclient.isFailed());

        // runCommand calls ensureConnection under the hood to auto-reconnect if possible.
        auto reply = dbclient.runCommand(pingRequest())->getCommandReply();
        ASSERT_OK(getStatusFromCommandResult(reply));
    };

    runTest(serverCb, clientThread);
}

TEST_F(DBClientGRPCTest, ShutdownBehavior) {
    Atomic<bool> firstRun(true);
    Notification<void> firstSessionReady;

    auto serverCb = [&](auto session) {
        ON_BLOCK_EXIT([&] { session->setTerminationStatus(Status::OK()); });

        confirmHelloAndRespond(session);

        if (firstRun.swap(false)) {
            firstSessionReady.set();

            // Cannot read from the stream after shutdown.
            auto ping = session->sourceMessage();
            ASSERT_NOT_OK(ping.getStatus());
            ASSERT_EQ(ping.getStatus().code(), ErrorCodes::CallbackCanceled);
            return;
        }

        // Now that we have reconnected, we can successfully receive messages again.
        confirmPingAndRespondPong(session);
    };

    auto clientThread = [&](DBClientGRPCStream& dbclient) {
        dbclient.connect(kServerHostAndPort, "test", boost::none);
        ON_BLOCK_EXIT([&] { dbclient.shutdown(); });
        ASSERT_TRUE(dbclient.isStillConnected());

        // Wait for the first server session to get and update the firstRun atomic before shutting
        // down and spawning a second server session to prevent a race where the second server
        // session could acquire firstRun before the first server session leading to test failures.
        firstSessionReady.get();

        // After shutdown, we can reconnect and send messages over the stream again.
        dbclient.shutdown();
        ASSERT_FALSE(dbclient.isStillConnected());
        auto reply = dbclient.runCommand(pingRequest())->getCommandReply();
        ASSERT_OK(getStatusFromCommandResult(reply));
    };

    runTest(serverCb, clientThread);
}

TEST_F(DBClientGRPCTest, ShutdownDisallowReconnectBehavior) {
    auto serverCb = [&](auto session) {
        ON_BLOCK_EXIT([&] { session->setTerminationStatus(Status::OK()); });
        confirmHelloAndRespond(session);
    };

    auto clientThread = [&](DBClientGRPCStream& dbclient) {
        dbclient.connect(kServerHostAndPort, "test", boost::none);
        ON_BLOCK_EXIT([&] { dbclient.shutdown(); });
        ASSERT_TRUE(dbclient.isStillConnected());

        dbclient.shutdownAndDisallowReconnect();
        ASSERT_THROWS(dbclient.runCommand(pingRequest())->getCommandReply(), DBException);
    };

    runTest(serverCb, clientThread);
}

TEST_F(DBClientGRPCTest, EnsureConnectionFailsAfterShutdown) {
    DBClientGRPCStream dbclient;
    dbclient.shutdown();
    // ensureConnection fails when the client connection is in a failed state.
    ASSERT_THROWS(dbclient.ensureConnection(), DBException);
}

}  // namespace
}  // namespace mongo
