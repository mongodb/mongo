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

#include <memory>

#include "mongo/db/dbmessage.h"
#include "mongo/transport/asio/asio_session.h"
#include "mongo/transport/asio/asio_session_impl.h"
#include "mongo/transport/asio/asio_transport_layer.h"
#include "mongo/transport/grpc/grpc_session.h"
#include "mongo/transport/grpc/grpc_transport_layer_impl.h"
#include "mongo/transport/grpc/test_fixtures.h"
#include "mongo/transport/session_manager.h"
#include "mongo/transport/test_fixtures.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_manager_impl.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/periodic_runner_factory.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::transport {
namespace {

class AsioGRPCTransportLayerManagerTest : public ServiceContextTest {
public:
    using ServerCb = std::function<void(Session&)>;

    void setUp() override {
        ServiceContextTest::setUp();
        auto* svcCtx = getServiceContext();

        svcCtx->setPeriodicRunner(makePeriodicRunner(getServiceContext()));

        sslGlobalParams.sslCAFile = grpc::CommandServiceTestFixtures::kCAFile;
        sslGlobalParams.sslPEMKeyFile = grpc::CommandServiceTestFixtures::kServerCertificateKeyFile;

        svcCtx->getService()->setServiceEntryPoint(
            std::make_unique<test::ServiceEntryPointUnimplemented>());
        svcCtx->setSessionManager(
            std::make_unique<test::MockSessionManager>([this](test::SessionThread& sessionThread) {
                if (_serverCb) {
                    _serverCb(sessionThread.session());
                }
            }));

        std::vector<std::unique_ptr<TransportLayer>> layers;
        auto asioTl = _makeAsioTransportLayer();
        _asioTL = asioTl.get();
        layers.push_back(std::move(asioTl));

        auto grpcTL = _makeGRPCTransportLayer();
        _grpcTL = grpcTL.get();
        layers.push_back(std::move(grpcTL));

        getServiceContext()->setTransportLayerManager(
            std::make_unique<TransportLayerManagerImpl>(std::move(layers), _asioTL));
        uassertStatusOK(getServiceContext()->getTransportLayerManager()->setup());
        uassertStatusOK(getServiceContext()->getTransportLayerManager()->start());
    }

    void tearDown() override {
        getServiceContext()->getSessionManager()->endAllSessions({});
        getServiceContext()->getTransportLayerManager()->shutdown();
        ServiceContextTest::tearDown();
    }

    TransportLayerManager& getTransportLayerManager() {
        return *getServiceContext()->getTransportLayerManager();
    }

    AsioTransportLayer& getAsioTransportLayer() {
        return *_asioTL;
    }

    /**
     * This must be set before any requests to the transport layer have been received.
     */
    void setServerCallback(ServerCb cb) {
        _serverCb = std::move(cb);
    }

private:
    std::unique_ptr<AsioTransportLayer> _makeAsioTransportLayer() {
        return std::make_unique<AsioTransportLayer>(AsioTransportLayer::Options{},
                                                    getServiceContext()->getSessionManager());
    }

    std::unique_ptr<grpc::GRPCTransportLayer> _makeGRPCTransportLayer() {
        grpc::GRPCTransportLayer::Options grpcOpts;
        grpcOpts.bindPort = grpc::CommandServiceTestFixtures::kBindPort;
        grpcOpts.maxServerThreads = grpc::CommandServiceTestFixtures::kMaxThreads;
        grpcOpts.enableEgress = true;
        grpcOpts.clientMetadata = grpc::makeClientMetadataDocument();
        auto grpcLayer = std::make_unique<grpc::GRPCTransportLayerImpl>(getServiceContext(),
                                                                        std::move(grpcOpts));
        uassertStatusOK(grpcLayer->registerService(std::make_unique<grpc::CommandService>(
            grpcLayer.get(),
            [&](auto session) { _serverCb(*session); },
            std::make_shared<grpc::WireVersionProvider>())));
        return grpcLayer;
    }

    AsioTransportLayer* _asioTL;
    grpc::GRPCTransportLayer* _grpcTL;
    ServerCb _serverCb;
};

TEST_F(AsioGRPCTransportLayerManagerTest, IngressAsioGRPC) {
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        setServerCallback([](Session& session) {
            ON_BLOCK_EXIT([&] { session.end(); });
            auto swMsg = session.sourceMessage();
            ASSERT_OK(swMsg);
            ASSERT_OK(session.sinkMessage(swMsg.getValue()));
        });

        constexpr auto kNumSessions = 5;

        auto grpcThread = monitor.spawn([&] {
            auto client = std::make_shared<grpc::GRPCClient>(
                nullptr,
                grpc::makeClientMetadataDocument(),
                grpc::CommandServiceTestFixtures::makeClientOptions());
            client->start(getServiceContext());
            ON_BLOCK_EXIT([&] { client->shutdown(); });

            for (auto i = 0; i < kNumSessions; i++) {
                auto session =
                    client->connect(grpc::CommandServiceTestFixtures::defaultServerAddress(),
                                    grpc::CommandServiceTestFixtures::kDefaultConnectTimeout,
                                    {});
                ON_BLOCK_EXIT([&] { ASSERT_OK(session->finish()); });
                assertEchoSucceeds(*session);
            }
        });

        auto asioThread = monitor.spawn([&] {
            for (auto i = 0; i < kNumSessions; i++) {
                auto swSession = getAsioTransportLayer().connect(
                    HostAndPort("localhost", 27017),
                    ConnectSSLMode::kGlobalSSLMode,
                    grpc::CommandServiceTestFixtures::kDefaultConnectTimeout,
                    boost::none);
                ASSERT_OK(swSession);
                ON_BLOCK_EXIT([&] { swSession.getValue()->end(); });
                grpc::assertEchoSucceeds(*swSession.getValue());
            }
        });

        grpcThread.join();
        asioThread.join();
    });
}

TEST_F(AsioGRPCTransportLayerManagerTest, EgressAsio) {
    setServerCallback([](auto& session) {
        ON_BLOCK_EXIT([&] { session.end(); });
        ASSERT_TRUE(dynamic_cast<SyncAsioSession*>(&session));
        auto swMsg = session.sourceMessage();
        uassertStatusOK(swMsg);
        uassertStatusOK(session.sinkMessage(swMsg.getValue()));
    });

    auto swSession = getTransportLayerManager().getEgressLayer()->connect(
        HostAndPort("localhost", 27017),
        ConnectSSLMode::kGlobalSSLMode,
        grpc::CommandServiceTestFixtures::kDefaultConnectTimeout,
        boost::none);
    ASSERT_OK(swSession);
    ON_BLOCK_EXIT([&] { swSession.getValue()->end(); });
    grpc::assertEchoSucceeds(*swSession.getValue());
}

TEST_F(AsioGRPCTransportLayerManagerTest, MarkKillOnGRPCClientDisconnect) {
    // When set with a value, the client side thread will disconnect the gRPC session.
    auto killSessionPf = makePromiseFuture<void>();

    // Set with a value once the server's callback has completed fully.
    auto serverCbCompletePf = makePromiseFuture<void>();

    setServerCallback([&](auto& session) mutable {
        try {
            ON_BLOCK_EXIT([&] { session.end(); });
            ASSERT_TRUE(dynamic_cast<grpc::IngressSession*>(&session));

            auto newClient = getServiceContext()->getService()->makeClient(
                "MyClient", session.shared_from_this());
            newClient->setDisconnectErrorCode(ErrorCodes::StreamTerminated);
            AlternativeClientRegion acr(newClient);
            auto opCtx = cc().makeOperationContext();

            auto baton = opCtx->getBaton();
            ASSERT_TRUE(baton);
            ASSERT_TRUE(baton->networking());

            opCtx->markKillOnClientDisconnect();

            ASSERT_DOES_NOT_THROW(opCtx->sleepFor(Milliseconds(10)));

            auto clkSource = getServiceContext()->getFastClockSource();
            auto start = clkSource->now();
            killSessionPf.promise.emplaceValue();
            ASSERT_THROWS_CODE(
                opCtx->sleepFor(Seconds(10)), DBException, cc().getDisconnectErrorCode());
            ASSERT_LT(clkSource->now() - start, Seconds(5)) << "sleep did not wake up early enough";

            serverCbCompletePf.promise.emplaceValue();
        } catch (...) {  // Need to catch all errors so we can wake up the main test thread if this
                         // handler failed.
            auto error = Status(ErrorCodes::UnknownError,
                                "server handler failed, see logs for offending exception");
            if (!killSessionPf.future.isReady()) {
                killSessionPf.promise.setError(std::move(error));
            } else {
                serverCbCompletePf.promise.setError(std::move(error));
            }
            throw;
        }
    });

    {
        auto client = std::make_shared<grpc::GRPCClient>(
            nullptr,
            grpc::makeClientMetadataDocument(),
            grpc::CommandServiceTestFixtures::makeClientOptions());
        client->start(getServiceContext());
        ON_BLOCK_EXIT([&] { client->shutdown(); });
        auto session = client->connect(grpc::CommandServiceTestFixtures::defaultServerAddress(),
                                       grpc::CommandServiceTestFixtures::kDefaultConnectTimeout,
                                       {});
        ON_BLOCK_EXIT([&] { session->end(); });
        ASSERT_OK(killSessionPf.future.getNoThrow());
        sleepFor(Milliseconds(100));
    }

    ASSERT_OK(serverCbCompletePf.future.getNoThrow());
}

}  // namespace
}  // namespace mongo::transport
