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

#include <boost/filesystem.hpp>

#include "mongo/db/dbmessage.h"
#include "mongo/stdx/mutex.h"
#include "mongo/transport/asio/asio_session.h"
#include "mongo/transport/asio/asio_session_impl.h"
#include "mongo/transport/asio/asio_transport_layer.h"
#include "mongo/transport/grpc/client_cache.h"
#include "mongo/transport/grpc/grpc_session.h"
#include "mongo/transport/grpc/grpc_session_manager.h"
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

        _monitor = std::make_unique<unittest::ThreadAssertionMonitor>();

        auto* svcCtx = getServiceContext();

        svcCtx->setPeriodicRunner(makePeriodicRunner(getServiceContext()));
        svcCtx->getService()->setServiceEntryPoint(
            std::make_unique<test::ServiceEntryPointUnimplemented>());

        std::vector<std::unique_ptr<TransportLayer>> layers;
        auto asioTl = _makeAsioTransportLayer();
        _asioTL = asioTl.get();
        layers.push_back(std::move(asioTl));

        auto grpcTL = _makeGRPCTransportLayer();
        _grpcTL = grpcTL.get();
        layers.push_back(std::move(grpcTL));

        sslGlobalParams.sslCAFile = _sslCAFile;
        sslGlobalParams.sslPEMKeyFile = _sslPEMKeyFile;
        sslGlobalParams.sslMode.store(SSLParams::SSLModes::SSLMode_requireSSL);

        getServiceContext()->setTransportLayerManager(
            std::make_unique<TransportLayerManagerImpl>(std::move(layers), _asioTL));
        uassertStatusOK(getServiceContext()->getTransportLayerManager()->setup());
        uassertStatusOK(getServiceContext()->getTransportLayerManager()->start());

        _asioListenAddress = HostAndPort("127.0.0.1", _asioTL->listenerPort());
        _grpcEgressReactor = std::dynamic_pointer_cast<grpc::GRPCReactor>(
            _grpcTL->getReactor(TransportLayer::WhichReactor::kEgress));
    }

    void tearDown() override {
        getServiceContext()->getTransportLayerManager()->shutdown();
        _monitor.reset();
        ServiceContextTest::tearDown();
    }

    void setTLSCertificatePaths(std::string sslCAFile, std::string sslPEMKeyFile) {
        _sslCAFile = std::move(sslCAFile);
        _sslPEMKeyFile = std::move(sslPEMKeyFile);
    }

    TransportLayerManager& getTransportLayerManager() {
        return *getServiceContext()->getTransportLayerManager();
    }

    AsioTransportLayer& getAsioTransportLayer() {
        return *_asioTL;
    }

    const HostAndPort& getGRPCListenAddress() const {
        return _grpcTL->getListeningAddresses().at(0);
    }

    const std::shared_ptr<grpc::GRPCReactor>& getGRPCReactor() {
        return _grpcEgressReactor;
    }

    const HostAndPort& getAsioListenAddress() const {
        return _asioListenAddress;
    }

    /**
     * Run the test using this test case's ThreadAssertionMonitor.
     */
    void runTest(std::function<void(unittest::ThreadAssertionMonitor& monitor)> test) {
        _monitor->spawnController([&] { test(*_monitor); }).join();
        _monitor->wait();
    }

    /**
     * This must be set before any requests to the transport layer have been received.
     * The callback will be run in a thread spawned from a ThreadAssertionMonitor to ensure that
     * failed assertions terminate the test. As a result, exceptions thrown from the handler will
     * fail the test rather than be handled by the transport layer.
     */
    void setServerCallback(ServerCb cb) {
        _serverCb = [&, cb = std::move(cb)](Session& s) {
            _monitor->spawn([&] { ASSERT_DOES_NOT_THROW(cb(s)); }).join();
        };
    }

private:
    std::unique_ptr<SessionManager> _makeSessionManager() {
        return std::make_unique<test::MockSessionManager>(
            [this](test::SessionThread& sessionThread) {
                if (_serverCb) {
                    _serverCb(*sessionThread.session());
                }
            });
    }

    std::unique_ptr<AsioTransportLayer> _makeAsioTransportLayer() {
        AsioTransportLayer::Options options{};
        options.port = test::kLetKernelChoosePort;
        return std::make_unique<AsioTransportLayer>(std::move(options), _makeSessionManager());
    }

    std::unique_ptr<grpc::GRPCTransportLayer> _makeGRPCTransportLayer() {
        grpc::GRPCTransportLayer::Options grpcOpts;
        grpcOpts.bindPort = test::kLetKernelChoosePort;
        grpcOpts.maxServerThreads = grpc::CommandServiceTestFixtures::kMaxThreads;
        grpcOpts.enableEgress = true;
        grpcOpts.clientMetadata = grpc::makeClientMetadataDocument();
        auto* svcCtx = getServiceContext();
        auto clientCache = std::make_shared<grpc::ClientCache>();
        std::vector<std::shared_ptr<ClientTransportObserver>> observers;
        auto sm =
            std::make_unique<grpc::GRPCSessionManager>(svcCtx, clientCache, std::move(observers));
        auto grpcLayer = std::make_unique<grpc::GRPCTransportLayerImpl>(
            svcCtx, std::move(grpcOpts), std::move(sm));
        uassertStatusOK(grpcLayer->registerService(std::make_unique<grpc::CommandService>(
            grpcLayer.get(),
            [&](auto session) { _serverCb(*session); },
            std::make_shared<grpc::WireVersionProvider>(),
            std::move(clientCache))));
        return grpcLayer;
    }

    std::unique_ptr<unittest::ThreadAssertionMonitor> _monitor;
    AsioTransportLayer* _asioTL;
    grpc::GRPCTransportLayer* _grpcTL;
    ServerCb _serverCb;
    std::string _sslCAFile = grpc::CommandServiceTestFixtures::kCAFile;
    std::string _sslPEMKeyFile = grpc::CommandServiceTestFixtures::kServerCertificateKeyFile;
    test::SSLGlobalParamsGuard _sslGlobalParamsGuard;

    HostAndPort _asioListenAddress;
    std::shared_ptr<grpc::GRPCReactor> _grpcEgressReactor;
};

TEST_F(AsioGRPCTransportLayerManagerTest, IngressAsioGRPC) {
    runTest([&](auto& monitor) {
        setServerCallback([](Session& session) {
            ON_BLOCK_EXIT([&] {
                if (auto grpcSession = dynamic_cast<grpc::IngressSession*>(&session)) {
                    grpcSession->setTerminationStatus(Status::OK());
                } else {
                    session.end();
                }
            });
            auto swMsg = session.sourceMessage();
            ASSERT_OK(swMsg);
            ASSERT_OK(session.sinkMessage(swMsg.getValue()));
        });

        constexpr auto kNumSessions = 5;

        auto grpcThread = monitor.spawn([&] {
            auto client = std::make_shared<grpc::GRPCClient>(
                nullptr,
                getServiceContext(),
                grpc::makeClientMetadataDocument(),
                grpc::CommandServiceTestFixtures::makeClientOptions());
            client->start();
            ON_BLOCK_EXIT([&] { client->shutdown(); });

            for (auto i = 0; i < kNumSessions; i++) {
                auto session =
                    client->connect(getGRPCListenAddress(),
                                    getGRPCReactor(),
                                    grpc::CommandServiceTestFixtures::kDefaultConnectTimeout,
                                    {});
                ON_BLOCK_EXIT([&] { ASSERT_OK(session->finish()); });
                assertEchoSucceeds(*session);
            }
        });

        auto asioThread = monitor.spawn([&] {
            for (auto i = 0; i < kNumSessions; i++) {
                auto swSession = getAsioTransportLayer().connect(
                    getAsioListenAddress(),
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
    runTest([&](auto& monitor) {
        setServerCallback([](auto& session) {
            ON_BLOCK_EXIT([&] { session.end(); });
            ASSERT_TRUE(dynamic_cast<SyncAsioSession*>(&session));
            auto swMsg = session.sourceMessage();
            ASSERT_OK(swMsg);
            ASSERT_OK(session.sinkMessage(swMsg.getValue()));
        });

        auto swSession = getTransportLayerManager().getDefaultEgressLayer()->connect(
            getAsioListenAddress(),
            ConnectSSLMode::kGlobalSSLMode,
            grpc::CommandServiceTestFixtures::kDefaultConnectTimeout,
            boost::none);
        ASSERT_OK(swSession);
        ON_BLOCK_EXIT([&] { swSession.getValue()->end(); });
        grpc::assertEchoSucceeds(*swSession.getValue());
    });
}

TEST_F(AsioGRPCTransportLayerManagerTest, MarkKillOnGRPCClientDisconnect) {
    runTest([&](auto&) {
        stdx::condition_variable cv;
        stdx::mutex mutex;
        // When set to true, the client side thread will disconnect the gRPC session.
        bool killSession = false;
        bool serverCbComplete = false;

        setServerCallback([&](auto& session) mutable {
            ON_BLOCK_EXIT([&] {
                session.end();
                {
                    stdx::unique_lock lk(mutex);
                    serverCbComplete = true;
                }
                cv.notify_all();
            });
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
            {
                stdx::lock_guard lk(mutex);
                killSession = true;
            }
            cv.notify_all();
            ASSERT_THROWS_CODE(
                opCtx->sleepFor(Seconds(10)), DBException, cc().getDisconnectErrorCode());
            ASSERT_LT(clkSource->now() - start, Seconds(5)) << "sleep did not wake up early enough";
        });

        {
            auto client = std::make_shared<grpc::GRPCClient>(
                nullptr,
                getServiceContext(),
                grpc::makeClientMetadataDocument(),
                grpc::CommandServiceTestFixtures::makeClientOptions());
            client->start();
            ON_BLOCK_EXIT([&] { client->shutdown(); });
            auto session = client->connect(getGRPCListenAddress(),
                                           getGRPCReactor(),
                                           grpc::CommandServiceTestFixtures::kDefaultConnectTimeout,
                                           {});
            ON_BLOCK_EXIT([&] { session->end(); });
            {
                stdx::unique_lock lk(mutex);
                cv.wait(lk, [&]() { return killSession || serverCbComplete; });
                ASSERT_FALSE(serverCbComplete) << "server callback terminated early";
            }
            // Wait for server callback to enter sleep before ending session by hitting end of
            // scope.
            sleepFor(Milliseconds(100));
        }

        stdx::unique_lock lk(mutex);
        cv.wait(lk, [&]() { return serverCbComplete; });
    });
}

TEST_F(AsioGRPCTransportLayerManagerTest, StartupUsingCreateWithConfig) {
    runTest([&](auto& monitor) {
        auto tlm = transport::TransportLayerManagerImpl::createWithConfig(
            &serverGlobalParams, getServiceContext(), true);

        uassertStatusOK(tlm->setup());
        uassertStatusOK(tlm->start());

        bool hasAsioTl = false;
        bool hasGRPCTl = false;
        for (auto& tl : tlm->getTransportLayers()) {
            auto protocol = tl->getTransportProtocol();
            if (protocol == TransportProtocol::MongoRPC)
                hasAsioTl = true;
            if (protocol == TransportProtocol::GRPC)
                hasGRPCTl = true;
        }
        ASSERT_TRUE(hasAsioTl);
        ASSERT_TRUE(hasGRPCTl);

        tlm->shutdown();
    });
}

class RotateCertificatesTransportLayerManagerTest : public AsioGRPCTransportLayerManagerTest {
public:
    void setUp() override {
        _tempDir =
            test::copyCertsToTempDir(grpc::CommandServiceTestFixtures::kCAFile,
                                     grpc::CommandServiceTestFixtures::kServerCertificateKeyFile,
                                     "tlm_gprc");

        setTLSCertificatePaths(_tempDir->getCAFile().toString(),
                               _tempDir->getPEMKeyFile().toString());
        AsioGRPCTransportLayerManagerTest::setUp();

        setServerCallback([](Session& session) {
            ON_BLOCK_EXIT([&] {
                if (auto grpcSession = dynamic_cast<grpc::IngressSession*>(&session)) {
                    grpcSession->setTerminationStatus(Status::OK());
                } else {
                    session.end();
                }
            });
            auto asioSession = dynamic_cast<AsioSession*>(&session);
            if (asioSession) {
                auto swMsg = session.sourceMessage();
                uassertStatusOK(swMsg);
                uassertStatusOK(session.sinkMessage(swMsg.getValue()));
            }
        });
    }

    StringData getFilePathCA() {
        return _tempDir->getCAFile();
    }

    StringData getFilePathPEM() {
        return _tempDir->getPEMKeyFile();
    }

private:
    std::unique_ptr<test::TempCertificatesDir> _tempDir;
};

#define ASSERT_ASIO_ECHO_SUCCEEDS(tl)                                                         \
    {                                                                                         \
        auto swSession = tl.connect(HostAndPort("localhost", tl.listenerPort()),              \
                                    ConnectSSLMode::kEnableSSL,                               \
                                    grpc::CommandServiceTestFixtures::kDefaultConnectTimeout, \
                                    boost::none);                                             \
        ASSERT_OK(swSession);                                                                 \
        ON_BLOCK_EXIT([&] { swSession.getValue()->end(); });                                  \
        grpc::assertEchoSucceeds(*swSession.getValue());                                      \
    }

TEST_F(RotateCertificatesTransportLayerManagerTest, RotateCertificatesSucceeds) {
    runTest([&](auto&) {
        // Ceritificates that we wil rotate to.
        const std::string kEcdsaCAFile = "jstests/libs/ecdsa-ca.pem";
        const std::string kEcdsaPEMFile = "jstests/libs/ecdsa-server.pem";
        const std::string kEcdsaClientFile = "jstests/libs/ecdsa-client.pem";

        auto initialGoodStub = grpc::CommandServiceTestFixtures::makeStubWithCerts(
            getGRPCListenAddress(),
            grpc::CommandServiceTestFixtures::kCAFile,
            grpc::CommandServiceTestFixtures::kClientCertificateKeyFile);
        auto initialBadStub = grpc::CommandServiceTestFixtures::makeStubWithCerts(
            getGRPCListenAddress(), kEcdsaCAFile, kEcdsaClientFile);

        ASSERT_ASIO_ECHO_SUCCEEDS(getAsioTransportLayer());
        ASSERT_GRPC_STUB_CONNECTED(initialGoodStub);
        ASSERT_GRPC_STUB_NOT_CONNECTED(initialBadStub);

        // Overwrite the tmp files to hold new certs.
        boost::filesystem::copy_file(kEcdsaCAFile,
                                     getFilePathCA().toString(),
                                     boost::filesystem::copy_options::overwrite_existing);
        boost::filesystem::copy_file(kEcdsaPEMFile,
                                     getFilePathPEM().toString(),
                                     boost::filesystem::copy_options::overwrite_existing);

        ASSERT_DOES_NOT_THROW(SSLManagerCoordinator::get()->rotate());

        ASSERT_ASIO_ECHO_SUCCEEDS(getAsioTransportLayer());
        ASSERT_GRPC_STUB_CONNECTED(initialGoodStub);
        ASSERT_GRPC_STUB_CONNECTED(initialBadStub);
    });
}

TEST_F(RotateCertificatesTransportLayerManagerTest,
       RotateCertificatesThrowsAndUsesOldCertsWhenEmpty) {
    runTest([&](auto&) {
        // Connect using the existing certs.
        auto stub = grpc::CommandServiceTestFixtures::makeStubWithCerts(
            getGRPCListenAddress(),
            grpc::CommandServiceTestFixtures::kCAFile,
            grpc::CommandServiceTestFixtures::kClientCertificateKeyFile);

        ASSERT_GRPC_STUB_CONNECTED(stub);
        ASSERT_ASIO_ECHO_SUCCEEDS(getAsioTransportLayer());

        boost::filesystem::resize_file(getFilePathCA().toString(), 0);

        ASSERT_THROWS_CODE(SSLManagerCoordinator::get()->rotate(),
                           DBException,
                           ErrorCodes::InvalidSSLConfiguration);

        ASSERT_ASIO_ECHO_SUCCEEDS(getAsioTransportLayer());
        auto stub2 = grpc::CommandServiceTestFixtures::makeStubWithCerts(
            getGRPCListenAddress(),
            grpc::CommandServiceTestFixtures::kCAFile,
            grpc::CommandServiceTestFixtures::kClientCertificateKeyFile);
        ASSERT_GRPC_STUB_CONNECTED(stub2);
    });
}

TEST_F(RotateCertificatesTransportLayerManagerTest, RotateCertificatesUsesOldCertsWithNewBadCerts) {
    runTest([&](auto&) {
        const std::string kInvalidPEMFile = "jstests/libs/expired.pem";

        // Connect using the existing certs.
        auto stub = grpc::CommandServiceTestFixtures::makeStubWithCerts(
            getGRPCListenAddress(),
            grpc::CommandServiceTestFixtures::kCAFile,
            grpc::CommandServiceTestFixtures::kClientCertificateKeyFile);
        ASSERT_GRPC_STUB_CONNECTED(stub);
        ASSERT_ASIO_ECHO_SUCCEEDS(getAsioTransportLayer());

        // Overwrite the tmp files to hold new, invalid certs.
        boost::filesystem::copy_file(kInvalidPEMFile,
                                     getFilePathPEM().toString(),
                                     boost::filesystem::copy_options::overwrite_existing);

        ASSERT_THROWS_CODE(SSLManagerCoordinator::get()->rotate(),
                           DBException,
                           ErrorCodes::InvalidSSLConfiguration);

        auto stub2 = grpc::CommandServiceTestFixtures::makeStubWithCerts(
            getGRPCListenAddress(),
            grpc::CommandServiceTestFixtures::kCAFile,
            grpc::CommandServiceTestFixtures::kClientCertificateKeyFile);
        ASSERT_GRPC_STUB_CONNECTED(stub2);
        ASSERT_ASIO_ECHO_SUCCEEDS(getAsioTransportLayer());
    });
}

}  // namespace
}  // namespace mongo::transport
