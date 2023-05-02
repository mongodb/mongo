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
#include <grpcpp/security/credentials.h>
#include <grpcpp/support/byte_buffer.h>
#include <grpcpp/support/status_code_enum.h>
#include <grpcpp/support/sync_stream.h>

#include "mongo/db/wire_version.h"
#include "mongo/logv2/constants.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/rpc/message.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/grpc/grpc_server_context.h"
#include "mongo/transport/grpc/metadata.h"
#include "mongo/transport/grpc/service.h"
#include "mongo/transport/grpc/test_fixtures.h"
#include "mongo/transport/grpc/wire_version_provider.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/notification.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::transport::grpc {

class CommandServiceTest : public unittest::Test {
public:
    void setUp() override {
        _wvProvider = std::make_shared<WireVersionProvider>();
    }

    WireVersionProvider& getWireVersionProvider() {
        return *_wvProvider;
    }

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

    void runMetadataValidationTest(::grpc::StatusCode expectedStatus,
                                   std::function<void(::grpc::ClientContext&)> makeCtx) {
        auto serverHandler = [](IngressSession&) {
            return ::grpc::Status::OK;
        };

        runTestWithBothMethods(serverHandler, [&](auto&, auto&, MethodCallback methodCallback) {
            ::grpc::ClientContext ctx;
            makeCtx(ctx);
            auto stream = methodCallback(ctx);
            ASSERT_EQ(stream->Finish().error_code(), expectedStatus);

            // The server should always respond with the cluster's max wire version, regardless
            // of whether metadata validation failed. The one exception is for authentication
            // failures.
            auto serverMetadata = ctx.GetServerInitialMetadata();
            auto it = serverMetadata.find(CommandService::kClusterMaxWireVersionKey);
            ASSERT_NE(it, serverMetadata.end());
            ASSERT_EQ(it->second,
                      std::to_string(getWireVersionProvider().getClusterMaxWireVersion()));
        });
    }

    void runMetadataLogTest(std::function<void(::grpc::ClientContext&)> makeCtx,
                            size_t nStreamsToCreate,
                            size_t nExpectedLogLines,
                            logv2::LogSeverity expectedSeverity) {
        unittest::MinimumLoggedSeverityGuard severityGuard{
            logv2::LogComponent::kNetwork,
            logv2::LogSeverity::Debug(logv2::LogSeverity::kMaxDebugLevel)};

        stdx::unordered_set<int> kLogMessageIds = {7401301, 7401302, 7401303};

        auto serverHandler = [](IngressSession& session) {
            return ::grpc::Status::OK;
        };

        runTestWithBothMethods(serverHandler, [&](auto&, auto& monitor, auto methodCallback) {
            startCapturingLogMessages();
            for (size_t i = 0; i < nStreamsToCreate; i++) {
                ::grpc::ClientContext ctx;
                makeCtx(ctx);
                auto stream = methodCallback(ctx);
                ASSERT_TRUE(stream->Finish().ok());
            }
            stopCapturingLogMessages();

            auto logLines = getCapturedBSONFormatLogMessages();
            auto n = std::count_if(logLines.cbegin(), logLines.cend(), [&](const BSONObj& line) {
                return line.getStringField(logv2::constants::kSeverityFieldName) ==
                    expectedSeverity.toStringDataCompact() &&
                    kLogMessageIds.contains(line.getIntField(logv2::constants::kIdFieldName));
            });

            ASSERT_EQ(n, nExpectedLogLines);
        });
    }

private:
    std::shared_ptr<WireVersionProvider> _wvProvider;
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

TEST_F(CommandServiceTest, NewClientsAreLogged) {
    runMetadataLogTest(
        [clientId = UUID::gen().toString()](::grpc::ClientContext& ctx) {
            CommandServiceTestFixtures::addRequiredClientMetadata(ctx);
            CommandServiceTestFixtures::addClientMetadataDocument(ctx);
            ctx.AddMetadata(CommandService::kClientIdKey.toString(), clientId);
        },
        /* nStreamsToCreate */ 3,
        /* nExpectedLogLines */ 1,
        logv2::LogSeverity::Info());
}

TEST_F(CommandServiceTest, OmittedClientIdIsLogged) {
    runMetadataLogTest(
        [](::grpc::ClientContext& ctx) {
            CommandServiceTestFixtures::addRequiredClientMetadata(ctx);
            CommandServiceTestFixtures::addClientMetadataDocument(ctx);
        },
        /* nStreamsToCreate */ 3,
        /* nExpectedLogLines */ 3,
        logv2::LogSeverity::Debug(2));
}

TEST_F(CommandServiceTest, NoMetadataDocumentNoLogs) {
    runMetadataLogTest(
        [clientId = UUID::gen().toString()](::grpc::ClientContext& ctx) {
            CommandServiceTestFixtures::addRequiredClientMetadata(ctx);
            ctx.AddMetadata(CommandService::kClientIdKey.toString(), clientId);
        },
        /* nStreamsToCreate */ 3,
        /* nExpectedLogLines */ 0,
        logv2::LogSeverity::Info());
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

TEST_F(CommandServiceTest, TooLowWireVersionIsRejected) {
    runMetadataValidationTest(
        ::grpc::StatusCode::FAILED_PRECONDITION, [](::grpc::ClientContext& ctx) {
            ctx.AddMetadata(CommandService::kWireVersionKey.toString(), "-1");
            ctx.AddMetadata(CommandService::kAuthenticationTokenKey.toString(), "my-token");
        });
}

TEST_F(CommandServiceTest, InvalidWireVersionIsRejected) {
    runMetadataValidationTest(::grpc::StatusCode::INVALID_ARGUMENT, [](::grpc::ClientContext& ctx) {
        ctx.AddMetadata(CommandService::kWireVersionKey.toString(), "foo");
        ctx.AddMetadata(CommandService::kAuthenticationTokenKey.toString(), "my-token");
    });
}

TEST_F(CommandServiceTest, InvalidClientIdIsRejected) {
    runMetadataValidationTest(::grpc::StatusCode::INVALID_ARGUMENT, [](::grpc::ClientContext& ctx) {
        CommandServiceTestFixtures::addRequiredClientMetadata(ctx);
        ctx.AddMetadata(CommandService::kClientIdKey.toString(), "not a valid UUID");
    });
}

TEST_F(CommandServiceTest, MissingWireVersionIsRejected) {
    runMetadataValidationTest(
        ::grpc::StatusCode::FAILED_PRECONDITION, [](::grpc::ClientContext& ctx) {
            ctx.AddMetadata(CommandService::kAuthenticationTokenKey.toString(), "my-token");
        });
}

TEST_F(CommandServiceTest, ClientMetadataDocumentIsOptional) {
    runMetadataValidationTest(::grpc::StatusCode::OK, [](::grpc::ClientContext& ctx) {
        CommandServiceTestFixtures::addRequiredClientMetadata(ctx);
        ctx.AddMetadata(CommandService::kClientIdKey.toString(), UUID::gen().toString());
    });
}

TEST_F(CommandServiceTest, ClientIdIsOptional) {
    runMetadataValidationTest(::grpc::StatusCode::OK, [](::grpc::ClientContext& ctx) {
        CommandServiceTestFixtures::addRequiredClientMetadata(ctx);
        CommandServiceTestFixtures::addClientMetadataDocument(ctx);
    });
}

TEST_F(CommandServiceTest, InvalidMetadataDocumentBase64Encoding) {
    // The MongoDB gRPC Protocol doesn't specify how an invalid metadata document should be handled,
    // and since invalid metadata doesn't affect the server's ability to execute the operation, it
    // was decided the server should just continue with the command and log a warning rather than
    // returning an error in such cases.
    runMetadataValidationTest(::grpc::StatusCode::OK, [](::grpc::ClientContext& ctx) {
        CommandServiceTestFixtures::addRequiredClientMetadata(ctx);
        ctx.AddMetadata(CommandService::kClientMetadataKey.toString(), "notvalidbase64:l;;?");
    });
}

TEST_F(CommandServiceTest, InvalidMetadataDocumentBSON) {
    runMetadataValidationTest(::grpc::StatusCode::OK, [](::grpc::ClientContext& ctx) {
        CommandServiceTestFixtures::addRequiredClientMetadata(ctx);
        ctx.AddMetadata(CommandService::kClientMetadataKey.toString(),
                        base64::encode("not valid BSON"));
    });
}

TEST_F(CommandServiceTest, UnrecognizedReservedKey) {
    runMetadataValidationTest(::grpc::StatusCode::INVALID_ARGUMENT, [](::grpc::ClientContext& ctx) {
        CommandServiceTestFixtures::addRequiredClientMetadata(ctx);
        ctx.AddMetadata("mongodb-not-recognized", "some value");
    });
}

TEST_F(CommandServiceTest, MissingAuthTokenIsRejected) {
    auto serverHandler = [](IngressSession&) {
        return ::grpc::Status::OK;
    };

    CommandServiceTestFixtures::runWithServer(serverHandler, [&](auto&, auto&) {
        auto stub = CommandServiceTestFixtures::makeStub();

        ::grpc::ClientContext ctx;
        ctx.AddMetadata(CommandService::kWireVersionKey.toString(),
                        std::to_string(getWireVersionProvider().getClusterMaxWireVersion()));

        auto stream = stub.authenticatedCommandStream(&ctx);
        ASSERT_EQ(stream->Finish().error_code(), ::grpc::StatusCode::UNAUTHENTICATED);
    });
}

TEST_F(CommandServiceTest, MissingAuthTokenIsAccepted) {
    auto serverHandler = [](IngressSession&) {
        return ::grpc::Status::OK;
    };

    CommandServiceTestFixtures::runWithServer(serverHandler, [&](auto&, auto&) {
        auto stub = CommandServiceTestFixtures::makeStub();

        ::grpc::ClientContext ctx;
        ctx.AddMetadata(CommandService::kWireVersionKey.toString(),
                        std::to_string(getWireVersionProvider().getClusterMaxWireVersion()));

        auto stream = stub.unauthenticatedCommandStream(&ctx);
        ASSERT_TRUE(stream->Finish().ok());
    });
}

TEST_F(CommandServiceTest, ServerProvidesClusterMaxWireVersion) {
    auto serverHandler = [](IngressSession& session) {
        auto swMessage = session.sourceMessage();
        ASSERT_OK(swMessage.getStatus());
        ASSERT_OK(session.sinkMessage(swMessage.getValue()));
        return ::grpc::Status::OK;
    };

    runTestWithBothMethods(serverHandler, [&](auto&, auto&, MethodCallback methodCallback) {
        ::grpc::ClientContext ctx;
        ctx.AddMetadata(CommandService::kAuthenticationTokenKey.toString(), "my-token");
        ctx.AddMetadata(CommandService::kWireVersionKey.toString(),
                        std::to_string(WireVersion::WIRE_VERSION_50));

        auto stream = methodCallback(ctx);
        ASSERT_TRUE(stream->Write(makeUniqueMessage().sharedBuffer()));
        SharedBuffer m;
        ASSERT_TRUE(stream->Read(&m));

        auto serverMetadata = ctx.GetServerInitialMetadata();
        auto it = serverMetadata.find(CommandService::kClusterMaxWireVersionKey);
        ASSERT_NE(it, serverMetadata.end());
        ASSERT_EQ(it->second, std::to_string(getWireVersionProvider().getClusterMaxWireVersion()));
    });
}

TEST_F(CommandServiceTest, ServerHandlesMultipleClients) {
    auto serverHandler = [](IngressSession& session) {
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

TEST_F(CommandServiceTest, HandleException) {
    auto serverHandler = [](IngressSession&) -> ::grpc::Status {
        iasserted(Status{ErrorCodes::StreamTerminated, "test error"});
    };

    auto clientThread =
        [&](Server&, unittest::ThreadAssertionMonitor& monitor, MethodCallback methodCallback) {
            ::grpc::ClientContext ctx;
            CommandServiceTestFixtures::addRequiredClientMetadata(ctx);
            auto stream = methodCallback(ctx);
            ASSERT_NE(stream->Finish().error_code(), ::grpc::StatusCode::OK);
        };

    runTestWithBothMethods(serverHandler, clientThread);
}

TEST_F(CommandServiceTest, Shutdown) {
    Notification<void> rpcStarted;

    auto serverHandler = [&rpcStarted](IngressSession& session) {
        rpcStarted.set();
        ASSERT_NOT_OK(session.sourceMessage());

        auto ts = session.terminationStatus();
        ASSERT_TRUE(ts);
        ASSERT_EQ(ts->code(), ErrorCodes::ShutdownInProgress);

        // This RPC is cancelled via shutdown, so the client will get an UNAVAILABLE or CANCELLED
        // status before this OK return can happen.
        return ::grpc::Status::OK;
    };

    auto clientThread = [&](Server& server,
                            unittest::ThreadAssertionMonitor& monitor,
                            MethodCallback methodCallback) {
        auto client = monitor.spawn([&methodCallback]() {
            ::grpc::ClientContext ctx;
            CommandServiceTestFixtures::addRequiredClientMetadata(ctx);
            auto stream = methodCallback(ctx);
            SharedBuffer buf;
            ASSERT_FALSE(stream->Read(&buf));
            ASSERT_NE(stream->Finish().error_code(), ::grpc::StatusCode::OK);
        });

        rpcStarted.get();
        server.shutdown();
        client.join();
    };

    runTestWithBothMethods(serverHandler, clientThread);
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
