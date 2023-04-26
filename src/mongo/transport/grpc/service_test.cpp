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

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>

#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/support/byte_buffer.h>

#include "mongo/db/wire_version.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/rpc/message.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/grpc/grpc_server_context.h"
#include "mongo/transport/grpc/metadata.h"
#include "mongo/transport/grpc/service.h"
#include "mongo/transport/grpc/test_fixtures.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::transport::grpc {

class CommandServiceTest : public unittest::Test {
public:
    using MethodCallback =
        std::function<std::shared_ptr<CommandServiceTestFixtures::Stub::ClientStream>(
            ::grpc::ClientContext&)>;

    /**
     * Runs a test twice: once for each method provided by CommandService.
     *
     * On each run of the test, this creates a new CommandService instance using
     * the provided handler, starts a new server instance, and then spawns a client thread that
     * constructs a stub towards the server and runs clientThreadBody. A callback that invokes the
     * method being tested is passed into clientThreadBody.
     */
    void runTestWithBothMethods(std::function<::grpc::Status(IngressSession&)> serverStreamHandler,
                                std::function<void(Server& server,
                                                   unittest::ThreadAssertionMonitor& monitor,
                                                   MethodCallback makeStream)> clientThreadBody) {
        CommandServiceTestFixtures::runWithServer(
            serverStreamHandler, [&](Server& server, unittest::ThreadAssertionMonitor& monitor) {
                auto stub = CommandServiceTestFixtures::makeStub();
                clientThreadBody(server, monitor, [&stub](::grpc::ClientContext& ctx) {
                    return stub.unauthenticatedCommandStream(&ctx);
                });
            });

        CommandServiceTestFixtures::runWithServer(
            serverStreamHandler, [&](Server& server, unittest::ThreadAssertionMonitor& monitor) {
                auto stub = CommandServiceTestFixtures::makeStub();
                clientThreadBody(server, monitor, [&stub](::grpc::ClientContext& ctx) {
                    return stub.authenticatedCommandStream(&ctx);
                });
            });
    }
};

TEST_F(CommandServiceTest, Echo) {
    auto echoHandler = [](IngressSession& session) {
        while (true) {
            auto swMsg = session.sourceMessage();
            if (!swMsg.isOK()) {
                ASSERT_EQ(swMsg.getStatus(), ErrorCodes::StreamTerminated);
                return ::grpc::Status::OK;
            }
            ASSERT_OK(session.sinkMessage(swMsg.getValue()));
        }
        return ::grpc::Status::OK;
    };

    runTestWithBothMethods(echoHandler, [&](auto&, auto& monitor, auto methodCallback) {
        auto maxWireVersion =
            std::to_string(WireSpec::instance().get()->incomingExternalClient.maxWireVersion);
        ::grpc::ClientContext ctx;
        CommandServiceTestFixtures::addAllClientMetadata(ctx);
        auto stream = methodCallback(ctx);

        auto message = makeUniqueMessage();
        ASSERT_TRUE(stream->Write(message.sharedBuffer()));
        SharedBuffer readMsg;
        ASSERT_TRUE(stream->Read(&readMsg)) << stream->Finish().error_message();
        ASSERT_EQ_MSG(Message{readMsg}, message);
    });
}

TEST_F(CommandServiceTest, ClientSendsMultipleMessages) {
    auto serverHandler = [](IngressSession& session) {
        int32_t nReceived = 0;
        while (true) {
            auto swMsg = session.sourceMessage();
            if (!swMsg.isOK()) {
                ASSERT_EQ(swMsg.getStatus(), ErrorCodes::StreamTerminated);
                break;
            }
            nReceived++;
        }

        OpMsg response;
        response.body = BSON("nReceived" << nReceived);
        ASSERT_OK(session.sinkMessage(response.serialize()));
        return ::grpc::Status::OK;
    };

    runTestWithBothMethods(serverHandler, [&](auto&, auto& monitor, auto methodCallback) {
        ::grpc::ClientContext ctx;
        CommandServiceTestFixtures::addAllClientMetadata(ctx);
        auto stream = methodCallback(ctx);

        size_t nSent = 12;
        auto msg = makeUniqueMessage();
        for (size_t i = 0; i < nSent; i++) {
            ASSERT_TRUE(stream->Write(msg.sharedBuffer()));
        }
        ASSERT_TRUE(stream->WritesDone());

        SharedBuffer serverResponse;
        ASSERT_TRUE(stream->Read(&serverResponse));

        auto responseMsg = OpMsg::parse(Message{serverResponse});
        int32_t nReceived = responseMsg.body.getIntField("nReceived");
        ASSERT_EQ(nReceived, nSent);
    });
}

TEST_F(CommandServiceTest, ServerSendsMultipleMessages) {
    auto serverHandler = [&](IngressSession& session) {
        int32_t nSent = 13;
        for (int32_t i = 0; i < nSent - 1; i++) {
            auto msg = makeUniqueMessage();
            OpMsg::setFlag(&msg, OpMsg::kMoreToCome);
            ASSERT_OK(session.sinkMessage(msg));
        }
        ASSERT_OK(session.sinkMessage(makeUniqueMessage()));

        auto swResponse = session.sourceMessage();
        ASSERT_OK(swResponse.getStatus());

        auto responseMsg = OpMsg::parse(swResponse.getValue());
        int32_t nReceived = responseMsg.body.getIntField("nReceived");
        ASSERT_EQ(nReceived, nSent);
        return ::grpc::Status::OK;
    };

    runTestWithBothMethods(serverHandler, [&](auto&, auto&, auto methodCallback) {
        ::grpc::ClientContext ctx;
        CommandServiceTestFixtures::addAllClientMetadata(ctx);
        auto stream = methodCallback(ctx);

        int32_t nReceived = 0;
        while (true) {
            SharedBuffer buf;
            ASSERT_TRUE(stream->Read(&buf));
            nReceived++;

            if (!OpMsg::isFlagSet(Message{buf}, OpMsg::kMoreToCome)) {
                break;
            }
        }

        OpMsg response;
        response.body = BSON("nReceived" << nReceived);
        ASSERT_TRUE(stream->Write(response.serialize().sharedBuffer()));
    });
}

TEST_F(CommandServiceTest, ServerHandlesMultipleClients) {
    auto serverHandler = [&](IngressSession& session) {
        while (true) {
            auto swMsg = session.sourceMessage();
            if (!swMsg.isOK()) {
                ASSERT_EQ(swMsg.getStatus(), ErrorCodes::StreamTerminated);
                break;
            }

            auto clientId = session.clientId();
            ASSERT_TRUE(clientId);
            auto response = OpMsg::parseOwned(swMsg.getValue());
            response.body =
                response.body.addFields(BSON(CommandService::kClientIdKey << clientId->toString()));
            ASSERT_OK(session.sinkMessage(response.serialize()));
        }
        return ::grpc::Status::OK;
    };

    runTestWithBothMethods(
        serverHandler,
        [&](auto&, unittest::ThreadAssertionMonitor& monitor, MethodCallback methodCallback) {
            std::vector<stdx::thread> threads;
            for (int32_t i = 0; i < 10; i++) {
                auto clientId = UUID::gen().toString();
                threads.push_back(monitor.spawn([&, clientId] {
                    ::grpc::ClientContext ctx;
                    CommandServiceTestFixtures::addRequiredClientMetadata(ctx);
                    ctx.AddMetadata(std::string{CommandService::kClientIdKey}, clientId);
                    CommandServiceTestFixtures::addClientMetadataDocument(ctx);

                    auto stream = methodCallback(ctx);

                    OpMsg msg;
                    msg.body = BSON("thread" << i);
                    ASSERT_TRUE(stream->Write(msg.serialize().sharedBuffer()));

                    SharedBuffer receivedMsg;
                    ASSERT_TRUE(stream->Read(&receivedMsg));

                    auto response = OpMsg::parse(Message{receivedMsg});
                    ASSERT_EQ(response.body.getIntField("thread"), i);
                    ASSERT_EQ(response.body.getStringField(CommandService::kClientIdKey), clientId);
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

    // Even though the source buffer is out of scope, the serialized gRPC ByteBuffer should
    // still be valid.
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
