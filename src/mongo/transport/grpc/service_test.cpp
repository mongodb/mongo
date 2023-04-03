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

#include <cstddef>
#include <memory>

#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/support/byte_buffer.h>

#include "mongo/logv2/log.h"
#include "mongo/rpc/message.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/grpc/metadata.h"
#include "mongo/transport/grpc/service.h"
#include "mongo/transport/grpc/test_fixtures.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::transport::grpc {

class TestStub {
public:
    using ReadMessageType = SharedBuffer;
    using WriteMessageType = ConstSharedBuffer;
    using ClientStream = ::grpc::ClientReaderWriter<WriteMessageType, ReadMessageType>;

    TestStub(const std::shared_ptr<::grpc::ChannelInterface> channel)
        : _channel(channel),
          _unauthenticatedCommandStreamMethod(Service::kUnauthenticatedCommandStreamMethodName,
                                              ::grpc::internal::RpcMethod::BIDI_STREAMING,
                                              channel),
          _authenticatedCommandStreamMethod(Service::kAuthenticatedCommandStreamMethodName,
                                            ::grpc::internal::RpcMethod::BIDI_STREAMING,
                                            channel) {}
    ~TestStub() = default;

    ClientStream* authenticatedCommandStream(::grpc::ClientContext* context) {
        return ::grpc::internal::ClientReaderWriterFactory<WriteMessageType, ReadMessageType>::
            Create(_channel.get(), _authenticatedCommandStreamMethod, context);
    }

    ClientStream* unauthenticatedCommandStream(::grpc::ClientContext* context) {
        return ::grpc::internal::ClientReaderWriterFactory<WriteMessageType, ReadMessageType>::
            Create(_channel.get(), _unauthenticatedCommandStreamMethod, context);
    }

    /**
     * Executes both RPCs defined in the service sequentially and passes the resultant stream to the
     * body closure each time. The makeCtx closure will be invoked once before each RPC invocation,
     * and the resultant context will then be used for the RPC.
     */
    void executeBoth(std::function<void(::grpc::ClientContext&)> makeCtx,
                     std::function<void(::grpc::ClientContext&, ClientStream&)> body) {
        ::grpc::ClientContext unauthenticatedCtx;
        makeCtx(unauthenticatedCtx);
        auto unauthenticatedStream = unauthenticatedCommandStream(&unauthenticatedCtx);
        body(unauthenticatedCtx, *unauthenticatedStream);

        ::grpc::ClientContext authenticatedCtx;
        makeCtx(authenticatedCtx);
        auto authenticatedStream = unauthenticatedCommandStream(&authenticatedCtx);
        body(authenticatedCtx, *authenticatedStream);
    }

private:
    std::shared_ptr<::grpc::ChannelInterface> _channel;
    const ::grpc::internal::RpcMethod _unauthenticatedCommandStreamMethod;
    const ::grpc::internal::RpcMethod _authenticatedCommandStreamMethod;
};

class ServiceTest : public unittest::Test {
public:
    static constexpr auto kServerAddress = "localhost:50051";

    TestStub makeStub() {
        LOGV2(7401202,
              "gRPC client is attempting to connect to the server",
              "address"_attr = kServerAddress);
        return TestStub{
            ::grpc::CreateChannel(kServerAddress, ::grpc::InsecureChannelCredentials())};
    }

    /**
     * Starts up a gRPC server with a Service registered that uses the provided handler for both RPC
     * methods. Executes the clientThreadBody in a separate thread and then waits for it to exit
     * before shutting down the server.
     */
    void runTest(Service::RpcHandler handler,
                 std::function<void(unittest::ThreadAssertionMonitor&)> clientThreadBody) {
        unittest::threadAssertionMonitoredTest([&](unittest::ThreadAssertionMonitor& monitor) {
            Service service{handler, handler};

            ::grpc::ServerBuilder builder;
            builder.AddListeningPort(kServerAddress, ::grpc::InsecureServerCredentials())
                .RegisterService(&service);
            std::unique_ptr<::grpc::Server> server(builder.BuildAndStart());

            auto serverThread = monitor.spawn([&] {
                LOGV2(7401201,
                      "gRPC server is listening for connections",
                      "address"_attr = kServerAddress);
                server->Wait();
            });
            auto clientThread = monitor.spawn([&] { clientThreadBody(monitor); });

            clientThread.join();
            server->Shutdown();
            serverThread.join();
        });
    }
};

TEST_F(ServiceTest, Echo) {
    auto echoHandler = [](auto& serverCtx, auto& stream) {
        while (auto msg = stream.read()) {
            stream.write(*msg);
        }
        return ::grpc::Status::OK;
    };

    runTest(echoHandler, [&](auto&) {
        auto stub = makeStub();
        stub.executeBoth([](auto& ctx) {},
                         [&](auto& ctx, TestStub::ClientStream& stream) {
                             auto message = makeUniqueMessage();
                             ASSERT_TRUE(stream.Write(message.sharedBuffer()));
                             SharedBuffer readMsg;
                             ASSERT_TRUE(stream.Read(&readMsg));
                             ASSERT_EQ_MSG(Message{readMsg}, message);
                         });
    });
}

TEST_F(ServiceTest, ClientMetadataIsAccessible) {
    MetadataView clientMetadata = {{"foo", "bar"}, {"baz", "quux"}};

    Service::RpcHandler metadataHandler = [&clientMetadata](auto& serverCtx, auto& stream) {
        MetadataView receivedClientMetadata = serverCtx.getClientMetadata();
        for (auto& kvp : clientMetadata) {
            auto it = receivedClientMetadata.find(kvp.first);
            ASSERT_NE(it, receivedClientMetadata.end());
            ASSERT_EQ(it->second, kvp.second);
        }
        ASSERT_TRUE(stream.write(makeUniqueMessage().sharedBuffer()));
        return ::grpc::Status::OK;
    };

    runTest(metadataHandler, [&](auto&) {
        auto stub = makeStub();

        auto makeContext = [&](auto& ctx) {
            for (auto kvp : clientMetadata) {
                ctx.AddMetadata(std::string{kvp.first}, std::string{kvp.second});
            }
        };
        auto body = [&](auto& ctx, auto& stream) {
            SharedBuffer msg;
            ASSERT_TRUE(stream.Read(&msg));
        };
        stub.executeBoth(makeContext, body);
    });
}

TEST_F(ServiceTest, ServerMetadataIsAccessible) {
    MetadataContainer serverMetadata = {{"foo", "bar"}, {"baz", "quux"}};

    Service::RpcHandler metadataHandler = [&serverMetadata](ServerContext& serverCtx,
                                                            ServerStream& stream) {
        for (auto& kvp : serverMetadata) {
            serverCtx.addInitialMetadataEntry(kvp.first, kvp.second);
        }
        ASSERT_TRUE(stream.write(makeUniqueMessage().sharedBuffer()));
        return ::grpc::Status::OK;
    };

    runTest(metadataHandler, [&](auto&) {
        auto stub = makeStub();

        auto body = [&](::grpc::ClientContext& ctx, auto& stream) {
            SharedBuffer msg;
            ASSERT_TRUE(stream.Read(&msg));
            auto receivedServerMetadata = ctx.GetServerInitialMetadata();

            for (auto& kvp : serverMetadata) {
                auto it = receivedServerMetadata.find(kvp.first);
                ASSERT_NE(it, receivedServerMetadata.end());
                ASSERT_EQ((std::string_view{it->second.data(), it->second.length()}), kvp.second);
            }
        };
        stub.executeBoth([](auto&) {}, body);
    });
}

TEST_F(ServiceTest, ClientSendsMultipleMessages) {
    Service::RpcHandler serverHandler = [](ServerContext& serverCtx, ServerStream& stream) {
        size_t nReceived = 0;
        while (auto msg = stream.read()) {
            nReceived++;
        }
        auto response = SharedBuffer::allocate(sizeof(size_t));
        memcpy(response.get(), &nReceived, sizeof(size_t));
        ASSERT_TRUE(stream.write(response));
        return ::grpc::Status::OK;
    };

    runTest(serverHandler, [&](auto&) {
        auto stub = makeStub();

        auto body = [&](::grpc::ClientContext& ctx, TestStub::ClientStream& stream) {
            size_t nSent = 12;
            auto msg = makeUniqueMessage();
            for (size_t i = 0; i < nSent; i++) {
                ASSERT_TRUE(stream.Write(msg.sharedBuffer()));
            }
            ASSERT_TRUE(stream.WritesDone());

            SharedBuffer serverResponse;
            ASSERT_TRUE(stream.Read(&serverResponse));

            size_t nReceived = *reinterpret_cast<size_t*>(serverResponse.get());
            ASSERT_EQ(nReceived, nSent);
        };
        stub.executeBoth([](auto&) {}, body);
    });
}

TEST_F(ServiceTest, ServerSendsMultipleMessages) {
    Service::RpcHandler serverHandler = [&](ServerContext& serverCtx, ServerStream& stream) {
        size_t nSent = 13;
        for (size_t i = 0; i < nSent - 1; i++) {
            auto msg = makeUniqueMessage();
            OpMsg::setFlag(&msg, OpMsg::kMoreToCome);
            ASSERT_TRUE(stream.write(msg.sharedBuffer()));
        }
        ASSERT_TRUE(stream.write(makeUniqueMessage().sharedBuffer()));

        auto response = stream.read();
        ASSERT_TRUE(response);
        ASSERT_EQ(*reinterpret_cast<size_t*>(response->get()), nSent);
        return ::grpc::Status::OK;
    };

    runTest(serverHandler, [&](auto&) {
        auto stub = makeStub();

        auto body = [&](::grpc::ClientContext&, TestStub::ClientStream& stream) {
            size_t nReceived = 0;

            while (true) {
                SharedBuffer buf;
                ASSERT_TRUE(stream.Read(&buf));
                nReceived++;

                if (!OpMsg::isFlagSet(Message{buf}, OpMsg::kMoreToCome)) {
                    break;
                }
            }

            auto response = SharedBuffer::allocate(sizeof(size_t));
            memcpy(response.get(), &nReceived, sizeof(size_t));
            ASSERT_TRUE(stream.Write(response));
        };
        stub.executeBoth([](auto&) {}, body);
    });
}

TEST_F(ServiceTest, ServerHandlesMultipleClients) {
    const auto kMetadataId = "client-thread";
    Service::RpcHandler serverHandler = [&](ServerContext& serverCtx, ServerStream& stream) {
        auto threadIdIt = serverCtx.getClientMetadata().find(kMetadataId);
        ASSERT_NE(threadIdIt, serverCtx.getClientMetadata().end());

        LOGV2(7401203,
              "ServerHandlesMultipleClients received stream request",
              "thread-id"_attr = threadIdIt->second);
        serverCtx.addInitialMetadataEntry(kMetadataId, std::string{threadIdIt->second});
        while (auto msg = stream.read()) {
            ASSERT_TRUE(stream.write(*msg));
        }
        return ::grpc::Status::OK;
    };

    runTest(serverHandler, [&](unittest::ThreadAssertionMonitor& monitor) {
        auto stub = makeStub();

        std::vector<stdx::thread> threads;
        for (size_t i = 0; i < 10; i++) {
            threads.push_back(monitor.spawn([&, i] {
                auto makeCtx = [i, &kMetadataId](::grpc::ClientContext& ctx) {
                    ctx.AddMetadata(kMetadataId, std::to_string(i));
                };

                auto body = [&, i](::grpc::ClientContext& ctx, TestStub::ClientStream& stream) {
                    auto msg = makeUniqueMessage();
                    ASSERT_TRUE(stream.Write(msg.sharedBuffer()));

                    SharedBuffer receivedMsg;
                    ASSERT_TRUE(stream.Read(&receivedMsg));
                    ASSERT_EQ_MSG(Message{receivedMsg}, msg);

                    auto serverMetadata = ctx.GetServerInitialMetadata();
                    auto threadIdIt = serverMetadata.find(kMetadataId);
                    ASSERT_NE(threadIdIt, serverMetadata.end());
                    ASSERT_EQ(threadIdIt->second, std::to_string(i));
                };

                stub.executeBoth(makeCtx, body);
            }));
        }

        for (auto& t : threads) {
            t.join();
        }
    });
}

class GRPCInteropTest : public unittest::Test {};

TEST_F(GRPCInteropTest, SharedBufferDeserialize) {
    std::string_view expected{"foobar"};

    auto deserializationTest = [&](std::vector<::grpc::Slice> slices) {
        ::grpc::ByteBuffer buffer(&slices[0], slices.size());

        SharedBuffer out;
        auto status = ::grpc::SerializationTraits<SharedBuffer>::Deserialize(&buffer, &out);
        ASSERT_TRUE(status.ok()) << "expected deserialization to succed but got error: "
                                 << status.error_message();
        ASSERT_EQ((std::string_view{out.get(), expected.length()}), expected);
    };

    deserializationTest({std::string{"foo"}, std::string{"bar"}});
    deserializationTest({std::string{"foobar"}});
}

TEST_F(GRPCInteropTest, SerializationRoundTrip) {
    ::grpc::ByteBuffer grpcBuf;
    auto message = makeUniqueMessage();

    {
        bool own_buf;
        auto newBuf = SharedBuffer::allocate(message.capacity());
        std::memcpy(newBuf.get(), message.buf(), newBuf.capacity());
        auto status =
            ::grpc::SerializationTraits<ConstSharedBuffer>::Serialize(newBuf, &grpcBuf, &own_buf);
        ASSERT_TRUE(status.ok()) << "expected serialization to succeed: " << status.error_message();
    }

    // Even though the source buffer is out of scope, the serialized gRPC ByteBuffer should still be
    // valid.
    SharedBuffer outputBuffer;
    auto status = ::grpc::SerializationTraits<SharedBuffer>::Deserialize(&grpcBuf, &outputBuffer);
    ASSERT_TRUE(status.ok()) << "expected deserialization to succeed: " << status.error_message();

    ASSERT_EQ_MSG(Message{outputBuffer}, message);
}

TEST_F(GRPCInteropTest, URIParsing) {
    {
        HostAndPort hp = GRPCServerContext::parseURI("ipv4:127.0.0.1");
        ASSERT_TRUE(hp.isLocalHost());
    }
    {
        HostAndPort hp = GRPCServerContext::parseURI("ipv4:192.168.0.1:123");
        ASSERT_EQ(hp.host(), "192.168.0.1");
        ASSERT_EQ(hp.port(), 123);
    }
    {
        HostAndPort hp = GRPCServerContext::parseURI("ipv6:[::1]");
        ASSERT_TRUE(hp.isLocalHost());
    }
    {
        HostAndPort hp =
            GRPCServerContext::parseURI("ipv6:[2001:db8:3333:4444:5555:6666:7777:8888]:123");
        ASSERT_EQ(hp.host(), "2001:db8:3333:4444:5555:6666:7777:8888");
        ASSERT_EQ(hp.port(), 123);
    }
    {
        HostAndPort hp = GRPCServerContext::parseURI("unix://path/to/socket.sock");
        ASSERT_EQ(hp.host(), "//path/to/socket.sock");
    }
}

}  // namespace mongo::transport::grpc
