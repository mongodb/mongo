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

#include "mongo/db/concurrency/locker_noop_service_context_test_fixture.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/transport/grpc/bidirectional_pipe.h"
#include "mongo/transport/grpc/metadata.h"
#include "mongo/transport/grpc/mock_client_context.h"
#include "mongo/transport/grpc/mock_client_stream.h"
#include "mongo/transport/grpc/mock_server_context.h"
#include "mongo/transport/grpc/mock_server_stream.h"
#include "mongo/transport/grpc/server.h"
#include "mongo/transport/grpc/service.h"
#include "mongo/transport/grpc/util.h"
#include "mongo/unittest/thread_assertion_monitor.h"
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
    return msg.serialize();
}

struct MockStreamTestFixtures {
    MockStreamTestFixtures(HostAndPort hostAndPort,
                           Milliseconds timeout,
                           MetadataView clientMetadata) {
        BidirectionalPipe pipe;
        auto promiseAndFuture = makePromiseFuture<MetadataContainer>();

        serverStream = std::make_unique<MockServerStream>(hostAndPort,
                                                          timeout,
                                                          std::move(promiseAndFuture.promise),
                                                          std::move(*pipe.left),
                                                          clientMetadata);
        serverCtx = std::make_unique<MockServerContext>(serverStream.get());

        clientStream = std::make_unique<MockClientStream>(
            hostAndPort, timeout, std::move(promiseAndFuture.future), std::move(*pipe.right));
        clientCtx = std::make_unique<MockClientContext>(clientStream.get());
    }

    std::unique_ptr<MockClientStream> clientStream;
    std::unique_ptr<MockClientContext> clientCtx;
    std::unique_ptr<MockServerStream> serverStream;
    std::unique_ptr<MockServerContext> serverCtx;
};

class ServiceContextWithClockSourceMockTest : public LockerNoopServiceContextTest {
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

class CommandServiceTestFixtures {
public:
    static constexpr auto kBindAddress = "localhost";
    static constexpr auto kBindPort = 1234;
    static constexpr auto kMaxThreads = 12;
    static constexpr auto kServerCertificateKeyFile = "jstests/libs/server.pem";
    static constexpr auto kClientCertificateKeyFile = "jstests/libs/client.pem";
    static constexpr auto kClientSelfSignedCertificateKeyFile =
        "jstests/libs/client-self-signed.pem";
    static constexpr auto kCAFile = "jstests/libs/ca.pem";

    class Stub {
    public:
        using ReadMessageType = SharedBuffer;
        using WriteMessageType = ConstSharedBuffer;
        using ClientStream = ::grpc::ClientReaderWriter<WriteMessageType, ReadMessageType>;

        struct Options {
            boost::optional<std::string> tlsCertificateKeyFile;
            boost::optional<std::string> tlsCAFile;
        };

        Stub(const std::shared_ptr<::grpc::ChannelInterface>& channel)
            : _channel(channel),
              _unauthenticatedCommandStreamMethod(
                  CommandService::kUnauthenticatedCommandStreamMethodName,
                  ::grpc::internal::RpcMethod::BIDI_STREAMING,
                  channel),
              _authenticatedCommandStreamMethod(
                  CommandService::kAuthenticatedCommandStreamMethodName,
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

    static Server::Options makeServerOptions() {
        Server::Options options;
        options.addresses = std::vector<std::string>{kBindAddress};
        options.port = kBindPort;
        options.maxThreads = kMaxThreads;
        options.tlsCAFile = kCAFile;
        options.tlsPEMKeyFile = kServerCertificateKeyFile;
        options.tlsAllowInvalidCertificates = false;
        options.tlsAllowConnectionsWithoutCertificates = false;
        return options;
    }

    static Server makeServer(CommandService::RpcHandler handler, Server::Options options) {
        std::vector<std::unique_ptr<Service>> services;
        services.push_back(std::make_unique<CommandService>(
            /* GRPCTransportLayer */ nullptr,
            std::move(handler),
            std::make_shared<WireVersionProvider>()));

        return Server{std::move(services), std::move(options)};
    }

    /**
     * Starts up a gRPC server with a CommandService registered that uses the provided handler for
     * both RPC methods. Executes the clientThreadBody in a separate thread and then waits for it to
     * exit before shutting down the server.
     */
    static void runWithServer(
        std::function<::grpc::Status(IngressSession&)> callback,
        std::function<void(Server&, unittest::ThreadAssertionMonitor&)> clientThreadBody,
        Server::Options options = makeServerOptions()) {
        unittest::threadAssertionMonitoredTest([&](unittest::ThreadAssertionMonitor& monitor) {
            auto handler = [callback](auto session) {
                ON_BLOCK_EXIT([&] { session->end(); });
                return callback(*session);
            };
            auto server = makeServer(std::move(handler), std::move(options));
            server.start();

            auto clientThread = monitor.spawn([&] { clientThreadBody(server, monitor); });

            clientThread.join();
            if (server.isRunning()) {
                server.shutdown();
            }
        });
    }

    static Stub makeStub(boost::optional<Stub::Options> options = boost::none) {
        return makeStub("localhost:{}"_format(kBindPort), options);
    }

    static Stub makeStub(StringData address, boost::optional<Stub::Options> options = boost::none) {
        if (!options) {
            options.emplace();
            options->tlsCAFile = kCAFile;
            options->tlsCertificateKeyFile = kClientCertificateKeyFile;
        }

        ::grpc::SslCredentialsOptions sslOps;
        if (options->tlsCertificateKeyFile) {
            auto certKeyPair = parsePEMKeyFile(*options->tlsCertificateKeyFile);
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
        ctx.AddMetadata(
            CommandService::kWireVersionKey.toString(),
            std::to_string(WireSpec::instance().get()->incomingExternalClient.maxWireVersion));
        ctx.AddMetadata(CommandService::kAuthenticationTokenKey.toString(), "my-token");
    }

    static void addClientMetadataDocument(::grpc::ClientContext& ctx) {
        static constexpr auto kDriverName = "myDriver";
        static constexpr auto kDriverVersion = "0.1.2";
        static constexpr auto kAppName = "MyAppName";

        BSONObjBuilder bob;
        ASSERT_OK(ClientMetadata::serialize(kDriverName, kDriverVersion, kAppName, &bob));
        auto metadataDoc = bob.obj();
        auto clientDoc = metadataDoc.getObjectField(kMetadataDocumentName);
        ctx.AddMetadata(CommandService::kClientMetadataKey.toString(),
                        base64::encode(clientDoc.objdata(), clientDoc.objsize()));
    }

    static void addAllClientMetadata(::grpc::ClientContext& ctx) {
        addRequiredClientMetadata(ctx);
        ctx.AddMetadata(CommandService::kClientIdKey.toString(), UUID::gen().toString());
        addClientMetadataDocument(ctx);
    }
};

}  // namespace mongo::transport::grpc
