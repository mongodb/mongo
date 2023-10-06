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

#pragma once

#include <map>
#include <memory>
#include <string>

#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/support/sync_stream.h>

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/wire_version.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/transport/grpc/bidirectional_pipe.h"
#include "mongo/transport/grpc/grpc_transport_layer.h"
#include "mongo/transport/grpc/metadata.h"
#include "mongo/transport/grpc/mock_client.h"
#include "mongo/transport/grpc/mock_client_context.h"
#include "mongo/transport/grpc/mock_client_stream.h"
#include "mongo/transport/grpc/mock_server_context.h"
#include "mongo/transport/grpc/mock_server_stream.h"
#include "mongo/transport/grpc/mock_stub.h"
#include "mongo/transport/grpc/server.h"
#include "mongo/transport/grpc/service.h"
#include "mongo/transport/grpc/util.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/net/ssl_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/uuid.h"

namespace mongo::transport::grpc {

#define ASSERT_EQ_MSG(a, b) ASSERT_EQ((a).opMsgDebugString(), (b).opMsgDebugString())

inline Message makeUniqueMessage() {
    OpMsg msg;
    msg.body = BSON("id" << UUID::gen().toBSON());
    auto out = msg.serialize();
    out.header().setId(nextMessageId());
    out.header().setResponseToMsgId(0);
    return out;
}

inline BSONObj makeClientMetadataDocument() {
    static constexpr auto kDriverName = "myDriver";
    static constexpr auto kDriverVersion = "0.1.2";
    static constexpr auto kAppName = "MyAppName";

    BSONObjBuilder bob;
    uassertStatusOK(ClientMetadata::serialize(kDriverName, kDriverVersion, kAppName, &bob));
    auto metadataDoc = bob.obj();
    return metadataDoc.getObjectField(kMetadataDocumentName).getOwned();
}

struct MockStreamTestFixtures {
    std::shared_ptr<MockClientStream> clientStream;
    std::shared_ptr<MockClientContext> clientCtx;
    std::unique_ptr<MockRPC> rpc;
};

class MockStubTestFixtures {
public:
    static constexpr auto kBindAddress = "localhost:1234";
    static constexpr auto kClientAddress = "abc:5678";

    MockStubTestFixtures() {
        MockRPCQueue::Pipe pipe;

        _channel = std::make_shared<MockChannel>(
            HostAndPort(kClientAddress), HostAndPort(kBindAddress), std::move(pipe.producer));
        _server = std::make_unique<MockServer>(std::move(pipe.consumer));
    }

    MockStub makeStub() {
        return MockStub(_channel);
    }

    std::unique_ptr<MockStreamTestFixtures> makeStreamTestFixtures(
        Date_t deadline, const MetadataView& clientMetadata) {
        MockStreamTestFixtures fixtures{nullptr, std::make_shared<MockClientContext>(), nullptr};

        auto clientThread = stdx::thread([&] {
            fixtures.clientCtx->setDeadline(deadline);
            for (auto& kvp : clientMetadata) {
                fixtures.clientCtx->addMetadataEntry(kvp.first.toString(), kvp.second.toString());
            }
            fixtures.clientStream =
                makeStub().unauthenticatedCommandStream(fixtures.clientCtx.get());
        });

        auto rpc = getServer().acceptRPC();
        ASSERT_TRUE(rpc);
        fixtures.rpc = std::make_unique<MockRPC>(std::move(*rpc));
        clientThread.join();

        return std::make_unique<MockStreamTestFixtures>(std::move(fixtures));
    }

    MockServer& getServer() {
        return *_server;
    }

private:
    std::unique_ptr<MockServer> _server;
    std::shared_ptr<MockChannel> _channel;
};

class ServiceContextWithClockSourceMockTest : public ServiceContextTest {
public:
    void setUp() override {
        _clkSource = std::make_shared<ClockSourceMock>();
        getServiceContext()->setFastClockSource(
            std::make_unique<SharedClockSourceAdapter>(_clkSource));
        getServiceContext()->setPreciseClockSource(
            std::make_unique<SharedClockSourceAdapter>(_clkSource));
    }

    auto& clockSource() {
        return *_clkSource;
    }

private:
    std::shared_ptr<ClockSourceMock> _clkSource;
};

class CommandServiceTestFixtures : public ServiceContextTest {
public:
    static constexpr auto kBindAddress = "localhost";
    static constexpr auto kBindPort = 1234;
    static constexpr auto kMaxThreads = 100;
    static constexpr auto kServerCertificateKeyFile = "jstests/libs/server_SAN.pem";
    static constexpr auto kClientCertificateKeyFile = "jstests/libs/client.pem";
    static constexpr auto kClientSelfSignedCertificateKeyFile =
        "jstests/libs/client-self-signed.pem";
    static constexpr auto kCAFile = "jstests/libs/ca.pem";
    static constexpr auto kMockedClientAddr = "client-def:123";
    static constexpr auto kDefaultConnectTimeout = Milliseconds(5000);

    class Stub {
    public:
        using ReadMessageType = SharedBuffer;
        using WriteMessageType = ConstSharedBuffer;
        using ClientStream = ::grpc::ClientReaderWriter<WriteMessageType, ReadMessageType>;
        using Options = GRPCClient::Options;

        Stub(const std::shared_ptr<::grpc::ChannelInterface>& channel)
            : _channel(channel),
              _unauthenticatedCommandStreamMethod(
                  util::constants::kUnauthenticatedCommandStreamMethodName,
                  ::grpc::internal::RpcMethod::BIDI_STREAMING,
                  channel),
              _authenticatedCommandStreamMethod(
                  util::constants::kAuthenticatedCommandStreamMethodName,
                  ::grpc::internal::RpcMethod::BIDI_STREAMING,
                  channel) {}

        ::grpc::Status connect() {
            ::grpc::ClientContext ctx;
            CommandServiceTestFixtures::addAllClientMetadata(ctx);
            auto stream = unauthenticatedCommandStream(&ctx);
            return stream->Finish();
        }

        std::shared_ptr<ClientStream> authenticatedCommandStream(::grpc::ClientContext* context) {
            return std::shared_ptr<ClientStream>{
                ::grpc::internal::ClientReaderWriterFactory<WriteMessageType, ReadMessageType>::
                    Create(_channel.get(), _authenticatedCommandStreamMethod, context)};
        }

        std::shared_ptr<ClientStream> unauthenticatedCommandStream(::grpc::ClientContext* context) {
            return std::shared_ptr<ClientStream>{
                ::grpc::internal::ClientReaderWriterFactory<WriteMessageType, ReadMessageType>::
                    Create(_channel.get(), _unauthenticatedCommandStreamMethod, context)};
        }

    private:
        std::shared_ptr<::grpc::ChannelInterface> _channel;
        ::grpc::internal::RpcMethod _unauthenticatedCommandStreamMethod;
        ::grpc::internal::RpcMethod _authenticatedCommandStreamMethod;
    };

    static HostAndPort defaultServerAddress() {
        return HostAndPort(kBindAddress, kBindPort);
    }

    static Server::Options makeServerOptions() {
        Server::Options options;
        options.addresses = {HostAndPort(kBindAddress, kBindPort)};
        options.maxThreads = kMaxThreads;
        options.tlsCAFile = kCAFile;
        options.tlsPEMKeyFile = kServerCertificateKeyFile;
        options.tlsAllowInvalidCertificates = false;
        options.tlsAllowConnectionsWithoutCertificates = false;
        return options;
    }

    static GRPCClient::Options makeClientOptions() {
        GRPCClient::Options options;
        options.tlsCAFile = kCAFile;
        options.tlsCertificateKeyFile = kClientCertificateKeyFile;
        return options;
    }

    static std::unique_ptr<Server> makeServer(CommandService::RPCHandler handler,
                                              Server::Options options) {
        std::vector<std::unique_ptr<Service>> services;
        services.push_back(std::make_unique<CommandService>(
            /* GRPCTransportLayer */ nullptr,
            std::move(handler),
            std::make_shared<WireVersionProvider>()));

        return std::make_unique<Server>(std::move(services), std::move(options));
    }

    /**
     * This is just a variant of runWithServers() that consumes a single set of options.
     */
    static void runWithServer(
        CommandService::RPCHandler callback,
        std::function<void(Server&, unittest::ThreadAssertionMonitor&)> clientThreadBody,
        Server::Options options = makeServerOptions()) {
        runWithServers(
            {std::move(options)},
            [cb = std::move(callback)](auto, auto session) { cb(std::move(session)); },
            [client = std::move(clientThreadBody)](auto& servers, auto& monitor) {
                client(*servers[0], monitor);
            });
    }

    /**
     * Starts up gRPC servers (one per set of options provided) with CommandServices registered that
     * use the provided handler for both RPC methods. Executes the clientThreadBody in a separate
     * thread and then waits for it to exit before shutting down the servers.
     *
     * The addresses of the server is passed to the RPC handler in addition to the IngressSession.
     * The IngressSession passed to the provided RPC handler is automatically ended after the
     * handler is returned.
     */
    static void runWithServers(
        std::vector<Server::Options> serverOptions,
        std::function<void(const Server::Options&, std::shared_ptr<IngressSession>)> rpcHandler,
        std::function<void(std::vector<std::unique_ptr<Server>>&,
                           unittest::ThreadAssertionMonitor&)> clientThreadBody) {
        unittest::threadAssertionMonitoredTest([&](unittest::ThreadAssertionMonitor& monitor) {
            std::vector<std::unique_ptr<Server>> servers;

            for (auto& options : serverOptions) {
                auto handler = [rpcHandler, &options](auto session) {
                    ON_BLOCK_EXIT([&] { session->end(); });
                    rpcHandler(options, session);
                };
                auto server = makeServer(handler, options);
                server->start();
                servers.push_back(std::move(server));
            }
            ON_BLOCK_EXIT([&servers] {
                for (auto& server : servers) {
                    if (server->isRunning()) {
                        server->shutdown();
                    }
                }
            });

            auto clientThread = monitor.spawn([&] { clientThreadBody(servers, monitor); });
            clientThread.join();
        });
    }

    /**
     * Starts up mocked gRPC servers (one per address provided) with CommandServices registered that
     * use the provided handler for both RPC methods. Executes the clientThreadBody in a separate
     * thread and then waits for it to exit before shutting down the servers.
     *
     * The address of the server is passed to the RPC handler in addition to the IngressSession. The
     * IngressSession passed to the provided RPC handler is automatically ended after the handler is
     * returned.
     */
    static void runWithMockServers(
        std::vector<HostAndPort> addresses,
        std::function<void(HostAndPort, std::shared_ptr<IngressSession>)> rpcHandler,
        std::function<void(MockClient&, unittest::ThreadAssertionMonitor&)> clientThreadBody,
        const BSONObj& md = makeClientMetadataDocument(),
        std::shared_ptr<WireVersionProvider> wvProvider = std::make_shared<WireVersionProvider>()) {

        unittest::threadAssertionMonitoredTest([&](unittest::ThreadAssertionMonitor& monitor) {
            std::vector<std::unique_ptr<MockServer>> servers;
            stdx::unordered_map<HostAndPort, MockRPCQueue::Producer> addrMap;

            for (auto& address : addresses) {
                MockRPCQueue::Pipe pipe;
                addrMap.insert({address, std::move(pipe.producer)});
                auto server = std::make_unique<MockServer>(std::move(pipe.consumer));
                server->start(
                    monitor,
                    [address, rpcHandler](std::shared_ptr<IngressSession> session) {
                        ON_BLOCK_EXIT([&]() { session->end(); });
                        rpcHandler(address, session);
                    },
                    wvProvider);
                servers.push_back(std::move(server));
            }
            ON_BLOCK_EXIT([&servers] {
                for (auto& server : servers) {
                    server->shutdown();
                }
            });

            auto resolver = [&addrMap](const HostAndPort& addr) {
                auto entry = addrMap.find(addr);
                invariant(entry != addrMap.end());
                return entry->second;
            };

            auto client =
                std::make_shared<MockClient>(nullptr, HostAndPort(kMockedClientAddr), resolver, md);
            clientThreadBody(*client, monitor);
        });
    }

    static Stub makeStub(boost::optional<Stub::Options> options = boost::none) {
        return makeStub("localhost:{}"_format(kBindPort), options);
    }

    static CommandService::RPCHandler makeEchoHandler() {
        return [](std::shared_ptr<IngressSession> session) {
            auto msg = uassertStatusOK(session->sourceMessage());
            uassertStatusOK(session->sinkMessage(std::move(msg)));
            session->end();
        };
    }

    static Stub makeStub(StringData address, boost::optional<Stub::Options> options = boost::none) {
        if (!options) {
            options.emplace();
            options->tlsCAFile = kCAFile;
            options->tlsCertificateKeyFile = kClientCertificateKeyFile;
        }

        ::grpc::SslCredentialsOptions sslOps;
        if (options->tlsCertificateKeyFile) {
            auto certKeyPair = util::parsePEMKeyFile(*options->tlsCertificateKeyFile);
            sslOps.pem_cert_chain = std::move(certKeyPair.cert_chain);
            sslOps.pem_private_key = std::move(certKeyPair.private_key);
        }
        if (options->tlsCAFile) {
            sslOps.pem_root_certs = ssl_util::readPEMFile(options->tlsCAFile.get()).getValue();
        }
        auto credentials = ::grpc::SslCredentials(sslOps);

        return Stub{::grpc::CreateChannel(address.toString(), credentials)};
    }

    /**
     * Sets the metadata entries necessary to ensure any CommandStream RPC can succeed. This may set
     * a superset of the required metadata for any individual RPC.
     */
    static void addRequiredClientMetadata(::grpc::ClientContext& ctx) {
        ctx.AddMetadata(util::constants::kWireVersionKey.toString(),
                        std::to_string(WireSpec::getWireSpec(getGlobalServiceContext())
                                           .get()
                                           ->incomingExternalClient.maxWireVersion));
        ctx.AddMetadata(util::constants::kAuthenticationTokenKey.toString(), "my-token");
    }

    static void addClientMetadataDocument(::grpc::ClientContext& ctx) {
        auto clientDoc = makeClientMetadataDocument();
        ctx.AddMetadata(util::constants::kClientMetadataKey.toString(),
                        base64::encode(clientDoc.objdata(), clientDoc.objsize()));
    }

    static void addAllClientMetadata(::grpc::ClientContext& ctx) {
        addRequiredClientMetadata(ctx);
        ctx.AddMetadata(util::constants::kClientIdKey.toString(), UUID::gen().toString());
        addClientMetadataDocument(ctx);
    }
};

inline std::shared_ptr<EgressSession> makeEgressSession(
    GRPCTransportLayer& tl,
    const HostAndPort& addr = CommandServiceTestFixtures::defaultServerAddress()) {
    auto swSession = tl.connect(
        addr, ConnectSSLMode::kGlobalSSLMode, CommandServiceTestFixtures::kDefaultConnectTimeout);
    return std::dynamic_pointer_cast<EgressSession>(uassertStatusOK(swSession));
}

inline void assertEchoSucceeds(Session& session) {
    auto msg = makeUniqueMessage();
    ASSERT_OK(session.sinkMessage(msg));
    auto swResponse = session.sourceMessage();
    ASSERT_OK(swResponse);
    ASSERT_EQ_MSG(swResponse.getValue(), msg);
}

}  // namespace mongo::transport::grpc
