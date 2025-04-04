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
#include "mongo/rpc/message.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/grpc/grpc_server_context.h"
#include "mongo/transport/grpc/metadata.h"
#include "mongo/transport/grpc/service.h"
#include "mongo/transport/grpc/test_fixtures.h"
#include "mongo/transport/grpc/util.h"
#include "mongo/transport/grpc/wire_version_provider.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/notification.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::transport::grpc {
namespace {

class CommandServiceTest : public CommandServiceTestFixtures {
public:
    using ClientContextType = ::grpc::ClientContext;
    using StubType = CommandServiceTestFixtures::Stub;
    using StreamFactoryType =
        std::function<std::shared_ptr<StubType::ClientStream>(ClientContextType&, StubType&)>;

    /**
     * Provides the client-side implementation for a gRPC client. `runTestWithBothMethods` will
     * invoke this callback and provide it with its arguments.
     */
    using ClientCallbackType = std::function<void(
        Server&, StubType, StreamFactoryType, unittest::ThreadAssertionMonitor&)>;

    /**
     * Runs a test twice: once for each method provided by CommandService.
     *
     * On each run of the test, this creates a new CommandService instance that uses the provided
     * handler, starts a new server instance, and then spawns a client thread that constructs a stub
     * towards the server and runs `callback`. The client may use the provided factory to make new
     * streams based on the method (e.g., unauthenticated) being tested.
     */
    void runTestWithBothMethods(CommandService::RPCHandler serverStreamHandler,
                                ClientCallbackType callback) {
        StreamFactoryType unauthCmdStreamFactory = [](auto& ctx, auto& stub) {
            return stub.unauthenticatedCommandStream(&ctx);
        };
        StreamFactoryType authCmdStreamFactory = [](auto& ctx, auto& stub) {
            return stub.authenticatedCommandStream(&ctx);
        };

        for (auto& factory : {unauthCmdStreamFactory, authCmdStreamFactory}) {
            CommandServiceTestFixtures::runWithServer(
                serverStreamHandler,
                [&](Server& server, unittest::ThreadAssertionMonitor& monitor) {
                    callback(
                        server,
                        CommandServiceTestFixtures::makeStub(server.getListeningAddresses().at(0)),
                        factory,
                        monitor);
                });
        }
    }

    using ContextInitializerType = std::function<void(::grpc::ClientContext&)>;

    /**
     * Fixture used to test the error codes returned to the client as the result of metadata
     * validation. Each test provides its own logic for initializing the client context, and
     * compares the returned gRPC status code against an expected value.
     */
    void runMetadataValidationTest(::grpc::StatusCode expectedStatusCode,
                                   ContextInitializerType initContext) {
        runTestWithBothMethods(
            [](auto) {},
            [&](auto&, auto stub, auto streamFactory, auto&) {
                ::grpc::ClientContext ctx;
                initContext(ctx);
                auto stream = streamFactory(ctx, stub);
                ASSERT_EQ(stream->Finish().error_code(), expectedStatusCode);

                // The server should always respond with the cluster's max wire version, regardless
                // of whether metadata validation failed. The one exception is for authentication
                // failures.
                auto serverMetadata = ctx.GetServerInitialMetadata();
                auto it = serverMetadata.find(util::constants::kClusterMaxWireVersionKey);
                ASSERT_NE(it, serverMetadata.end());
                ASSERT_EQ(it->second,
                          std::to_string(wireVersionProvider().getClusterMaxWireVersion()));
            });
    }

    /**
     * Verifies the number and the severity of logs, concerning client-metadata, that are emitted in
     * response to accepting gRPC streams on the server side.
     */
    void runMetadataLogTest(ContextInitializerType initContext,
                            size_t nStreamsToCreate,
                            size_t nExpectedLogLines,
                            logv2::LogSeverity expectedSeverity) {
        stdx::unordered_set<int> clientMetadataLogIds = {7401301, 7401302, 7401303};
        runTestWithBothMethods(
            [](auto) {},
            [&](auto&, auto stub, auto streamFactory, auto&) {
                // Temporarily maximize verbosity for networking logs.
                unittest::MinimumLoggedSeverityGuard severityGuard{
                    logv2::LogComponent::kNetwork,
                    logv2::LogSeverity::Debug(logv2::LogSeverity::kMaxDebugLevel)};

                startCapturingLogMessages();
                for (size_t i = 0; i < nStreamsToCreate; i++) {
                    ::grpc::ClientContext ctx;
                    initContext(ctx);
                    auto stream = streamFactory(ctx, stub);
                    ASSERT_EQ(stream->Finish().error_code(), ::grpc::OK);
                }
                stopCapturingLogMessages();

                auto logLines = getCapturedBSONFormatLogMessages();
                auto observed =
                    std::count_if(logLines.cbegin(), logLines.cend(), [&](const BSONObj& line) {
                        return line.getStringField(logv2::constants::kSeverityFieldName) ==
                            expectedSeverity.toStringDataCompact() &&
                            clientMetadataLogIds.contains(
                                line.getIntField(logv2::constants::kIdFieldName));
                    });

                ASSERT_EQ(observed, nExpectedLogLines);
            });
    }

    using TerminationCallbackType = std::function<void(IngressSession&)>;

    /**
     * Creates a stream against each command stream method, and then uses the provided callback to
     * terminate the stream. The goal is to verify the termination status (and potentially the
     * reason) that is visible to the client-side of the stream.
     */
    void runTerminationTest(TerminationCallbackType terminationCallback,
                            ::grpc::StatusCode expectedStatus,
                            boost::optional<std::string> expectedReason = boost::none) {
        const size_t kMessageCount = 5;
        std::unique_ptr<Notification<void>> terminated;

        CommandService::RPCHandler serverHandler = [&](auto session) {
            for (size_t i = 0; i < kMessageCount; i++) {
                ASSERT_OK(session->sinkMessage(makeUniqueMessage()));
            }
            terminationCallback(*session);
            terminated->set();
            ASSERT_NOT_OK(session->sinkMessage(makeUniqueMessage()));
        };

        runTestWithBothMethods(serverHandler, [&](auto&, auto stub, auto streamFactory, auto&) {
            // Initialize the termination notification for this run.
            terminated = std::make_unique<Notification<void>>();

            ::grpc::ClientContext ctx;
            CommandServiceTestFixtures::addAllClientMetadata(ctx);
            auto stream = streamFactory(ctx, stub);

            terminated->get();

            // We should be able to read messages sent before the RPC was cancelled.
            for (size_t i = 0; i < kMessageCount; i++) {
                SharedBuffer buffer;
                ASSERT_TRUE(stream->Read(&buffer));
            }

            SharedBuffer buffer;
            ASSERT_FALSE(stream->Read(&buffer));

            auto status = stream->Finish();
            ASSERT_EQ(status.error_code(), expectedStatus);
            if (expectedReason) {
                ASSERT_EQ(status.error_message(), *expectedReason);
            }
        });
    }

    WireVersionProvider& wireVersionProvider() const {
        return *_wvProvider;
    }

private:
    std::shared_ptr<WireVersionProvider> _wvProvider = std::make_shared<WireVersionProvider>();
};

TEST_F(CommandServiceTest, Echo) {
    runTestWithBothMethods(CommandServiceTestFixtures::makeEchoHandler(),
                           [&](auto&, auto stub, auto streamFactory, auto&) {
                               ::grpc::ClientContext ctx;
                               CommandServiceTestFixtures::addAllClientMetadata(ctx);

                               auto stream = streamFactory(ctx, stub);
                               auto toWrite = makeUniqueMessage();
                               ASSERT_TRUE(stream->Write(toWrite.sharedBuffer()));
                               SharedBuffer toRead;
                               ASSERT_TRUE(stream->Read(&toRead))
                                   << stream->Finish().error_message();
                               ASSERT_EQ_MSG(Message{toRead}, toWrite);
                               ASSERT_TRUE(stream->WritesDone());
                               ASSERT_EQ(stream->Finish().error_code(), ::grpc::OK);
                           });
}

TEST_F(CommandServiceTest, CancelSession) {
    runTerminationTest(
        [](auto& session) {
            session.cancel(Status(ErrorCodes::ShutdownInProgress, "some reason"));
        },
        ::grpc::CANCELLED);
}

TEST_F(CommandServiceTest, TerminateSessionWithShutdownError) {
    Status shutdownError(ErrorCodes::ShutdownInProgress, "shutdown error");
    runTerminationTest([&](auto& session) { session.setTerminationStatus(shutdownError); },
                       util::errorToStatusCode(shutdownError.code()),
                       shutdownError.reason());
}

TEST_F(CommandServiceTest, TerminateSessionWithCancellationError) {
    Status cancellationError(ErrorCodes::CallbackCanceled, "cancelled");
    runTerminationTest([&](auto& session) { session.setTerminationStatus(cancellationError); },
                       util::errorToStatusCode(cancellationError.code()),
                       cancellationError.reason());
}

TEST_F(CommandServiceTest, EndSession) {
    runTerminationTest([](auto& session) { session.end(); }, ::grpc::CANCELLED);
}

TEST_F(CommandServiceTest, TerminateSession) {
    runTerminationTest([](auto& session) { session.setTerminationStatus(Status::OK()); },
                       ::grpc::OK);
}

TEST_F(CommandServiceTest, ClientCancellation) {
    std::unique_ptr<Notification<void>> serverCbDone;

    auto serverCb = [&serverCbDone](auto session) {
        ON_BLOCK_EXIT([&] { serverCbDone->set(); });
        ASSERT_OK(session->sourceMessage());
        ASSERT_OK(session->sinkMessage(makeUniqueMessage()));

        auto swMsg = session->sourceMessage();
        ASSERT_EQ(swMsg.getStatus().code(), ErrorCodes::CallbackCanceled);
        ASSERT_TRUE(session->terminationStatus().has_value());
        ASSERT_EQ(session->terminationStatus()->code(), ErrorCodes::CallbackCanceled);
    };

    runTestWithBothMethods(serverCb, [&](auto&, auto stub, auto streamFactory, auto&) {
        serverCbDone = std::make_unique<Notification<void>>();

        ::grpc::ClientContext ctx;
        CommandServiceTestFixtures::addAllClientMetadata(ctx);

        auto stream = streamFactory(ctx, stub);
        auto toWrite = makeUniqueMessage();
        ASSERT_TRUE(stream->Write(toWrite.sharedBuffer()));
        SharedBuffer toRead;
        ASSERT_TRUE(stream->Read(&toRead)) << stream->Finish().error_message();

        ctx.TryCancel();
        ASSERT_EQ(stream->Finish().error_code(), ::grpc::CANCELLED);

        // Wait for server to receive cancellation before exiting.
        serverCbDone->get();
    });
}

TEST_F(CommandServiceTest, TooLowWireVersionIsRejected) {
    ContextInitializerType initContext = [](auto& ctx) {
        ctx.AddMetadata(util::constants::kWireVersionKey.toString(), "-1");
        ctx.AddMetadata(util::constants::kAuthenticationTokenKey.toString(), "my-token");
    };
    runMetadataValidationTest(::grpc::StatusCode::FAILED_PRECONDITION, initContext);
}

TEST_F(CommandServiceTest, InvalidWireVersionIsRejected) {
    ContextInitializerType initContext = [](auto& ctx) {
        ctx.AddMetadata(util::constants::kWireVersionKey.toString(), "foo");
        ctx.AddMetadata(util::constants::kAuthenticationTokenKey.toString(), "my-token");
    };
    runMetadataValidationTest(::grpc::StatusCode::INVALID_ARGUMENT, initContext);
}

TEST_F(CommandServiceTest, InvalidClientIdIsRejected) {
    ContextInitializerType initContext = [](auto& ctx) {
        CommandServiceTestFixtures::addRequiredClientMetadata(ctx);
        ctx.AddMetadata(util::constants::kClientIdKey.toString(), "not a valid UUID");
    };
    runMetadataValidationTest(::grpc::StatusCode::INVALID_ARGUMENT, initContext);
}

TEST_F(CommandServiceTest, MissingWireVersionIsRejected) {
    ContextInitializerType initContext = [](auto& ctx) {
        ctx.AddMetadata(util::constants::kAuthenticationTokenKey.toString(), "my-token");
    };
    runMetadataValidationTest(::grpc::StatusCode::FAILED_PRECONDITION, initContext);
}

TEST_F(CommandServiceTest, ClientMetadataDocumentIsOptional) {
    ContextInitializerType initContext = [](auto& ctx) {
        CommandServiceTestFixtures::addRequiredClientMetadata(ctx);
        ctx.AddMetadata(util::constants::kClientIdKey.toString(), UUID::gen().toString());
    };
    runMetadataValidationTest(::grpc::StatusCode::OK, initContext);
}

TEST_F(CommandServiceTest, ClientIdIsOptional) {
    ContextInitializerType initContext = [](auto& ctx) {
        CommandServiceTestFixtures::addRequiredClientMetadata(ctx);
        CommandServiceTestFixtures::addClientMetadataDocument(ctx);
    };
    runMetadataValidationTest(::grpc::StatusCode::OK, initContext);
}

TEST_F(CommandServiceTest, InvalidMetadataDocumentBase64Encoding) {
    // The MongoDB gRPC Protocol doesn't specify how an invalid metadata document should be handled,
    // and since invalid metadata doesn't affect the server's ability to execute the operation, it
    // was decided the server should just continue with the command and log a warning rather than
    // returning an error in such cases.
    ContextInitializerType initContext = [](auto& ctx) {
        CommandServiceTestFixtures::addRequiredClientMetadata(ctx);
        ctx.AddMetadata(util::constants::kClientMetadataKey.toString(), "notvalidbase64:l;;?");
    };
    runMetadataValidationTest(::grpc::StatusCode::OK, initContext);
}

TEST_F(CommandServiceTest, InvalidMetadataDocumentBSON) {
    ContextInitializerType initContext = [](auto& ctx) {
        CommandServiceTestFixtures::addRequiredClientMetadata(ctx);
        ctx.AddMetadata(util::constants::kClientMetadataKey.toString(), base64::encode("Not BSON"));
    };
    runMetadataValidationTest(::grpc::StatusCode::OK, initContext);
}

TEST_F(CommandServiceTest, UnrecognizedReservedKey) {
    ContextInitializerType initContext = [](auto& ctx) {
        CommandServiceTestFixtures::addRequiredClientMetadata(ctx);
        ctx.AddMetadata("mongodb-not-recognized", "some value");
    };
    runMetadataValidationTest(::grpc::StatusCode::INVALID_ARGUMENT, initContext);
}

TEST_F(CommandServiceTest, NewClientsAreLogged) {
    ContextInitializerType initContext = [clientId = UUID::gen().toString()](auto& ctx) {
        CommandServiceTestFixtures::addRequiredClientMetadata(ctx);
        CommandServiceTestFixtures::addClientMetadataDocument(ctx);
        ctx.AddMetadata(util::constants::kClientIdKey.toString(), clientId);
    };
    runMetadataLogTest(initContext,
                       5,  // nStreamsToCreate
                       1,  // nExpectedLogLines
                       logv2::LogSeverity::Info());
}

TEST_F(CommandServiceTest, OmittedClientIdIsLogged) {
    ContextInitializerType initContext = [](auto& ctx) {
        CommandServiceTestFixtures::addRequiredClientMetadata(ctx);
        CommandServiceTestFixtures::addClientMetadataDocument(ctx);
    };
    runMetadataLogTest(initContext,
                       3,  // nStreamsToCreate
                       3,  // nExpectedLogLines
                       logv2::LogSeverity::Debug(2));
}

TEST_F(CommandServiceTest, NoLogsForMissingMetadataDocument) {
    ContextInitializerType initContext = [clientId = UUID::gen().toString()](auto& ctx) {
        CommandServiceTestFixtures::addRequiredClientMetadata(ctx);
        ctx.AddMetadata(util::constants::kClientIdKey.toString(), clientId);
    };
    runMetadataLogTest(initContext,
                       7,  // nStreamsToCreate
                       0,  // nExpectedLogLines
                       logv2::LogSeverity::Info());
}

TEST_F(CommandServiceTest, ClientSendsMultipleMessages) {
    CommandService::RPCHandler serverHandler = [](auto session) {
        int nReceived = 0;
        while (true) {
            auto msg = uassertStatusOK(session->sourceMessage());
            nReceived++;
            if (!OpMsg::isFlagSet(msg, OpMsg::kMoreToCome)) {
                break;
            }
        }
        OpMsg response;
        response.body = BSON("nReceived" << nReceived);
        ASSERT_OK(session->sinkMessage(response.serialize()));
    };

    ClientCallbackType clientCallback = [&](auto&, auto stub, auto streamFactory, auto&) {
        ::grpc::ClientContext ctx;
        CommandServiceTestFixtures::addAllClientMetadata(ctx);
        auto stream = streamFactory(ctx, stub);

        const int kMessages = 13;
        for (auto i = 0; i < kMessages; i++) {
            auto msg = makeUniqueMessage();
            if (i < kMessages - 1) {
                OpMsg::setFlag(&msg, OpMsg::kMoreToCome);
            }
            ASSERT_TRUE(stream->Write(msg.sharedBuffer()));
        }

        SharedBuffer serverResponse;
        ASSERT_TRUE(stream->Read(&serverResponse));

        auto responseMsg = OpMsg::parse(Message{serverResponse});
        int32_t nReceived = responseMsg.body.getIntField("nReceived");
        ASSERT_EQ(nReceived, kMessages);

        ASSERT_EQ(stream->Finish().error_code(), ::grpc::OK);
    };

    runTestWithBothMethods(serverHandler, clientCallback);
}

TEST_F(CommandServiceTest, ServerSendsMultipleMessages) {
    CommandService::RPCHandler serverHandler = [](auto session) {
        const int kMessages = 17;
        for (auto i = 0; i < kMessages - 1; i++) {
            auto msg = makeUniqueMessage();
            OpMsg::setFlag(&msg, OpMsg::kMoreToCome);
            ASSERT_OK(session->sinkMessage(msg));
        }
        ASSERT_OK(session->sinkMessage(makeUniqueMessage()));

        auto swResponse = session->sourceMessage();
        ASSERT_OK(swResponse);
        int32_t nReceived = OpMsg::parse(swResponse.getValue()).body.getIntField("nReceived");
        ASSERT_EQ(nReceived, kMessages);
    };

    ClientCallbackType clientCallback = [](auto&, auto stub, auto streamFactory, auto&) {
        ::grpc::ClientContext ctx;
        CommandServiceTestFixtures::addAllClientMetadata(ctx);
        auto stream = streamFactory(ctx, stub);

        int nReceived = 0;
        while (true) {
            SharedBuffer buffer;
            ASSERT_TRUE(stream->Read(&buffer));
            nReceived++;

            if (!OpMsg::isFlagSet(Message{buffer}, OpMsg::kMoreToCome)) {
                break;
            }
        }

        OpMsg response;
        response.body = BSON("nReceived" << nReceived);
        ASSERT_TRUE(stream->Write(response.serialize().sharedBuffer()));
        ASSERT_EQ(stream->Finish().error_code(), ::grpc::OK);
    };

    runTestWithBothMethods(serverHandler, clientCallback);
}

TEST_F(CommandServiceTest, AuthTokenHandling) {
    auto makeStream =
        [&](const HostAndPort& addr, bool useAuth, boost::optional<std::string> authToken) {
            ::grpc::ClientContext ctx;
            auto stub = CommandServiceTestFixtures::makeStub(addr);
            ctx.AddMetadata(util::constants::kWireVersionKey.toString(),
                            std::to_string(wireVersionProvider().getClusterMaxWireVersion()));
            if (authToken) {
                ctx.AddMetadata(util::constants::kAuthenticationTokenKey.toString(), *authToken);
            }
            auto stream = useAuth ? stub.authenticatedCommandStream(&ctx)
                                  : stub.unauthenticatedCommandStream(&ctx);
            return stream->Finish();
        };

    CommandServiceTestFixtures::runWithServer(
        [](auto session) { ASSERT_FALSE(session->authToken()); },
        [&](auto& server, auto&) {
            ASSERT_TRUE(makeStream(server.getListeningAddresses().at(0),
                                   false /* Don't use Auth */,
                                   boost::none)
                            .ok());
        });
    CommandServiceTestFixtures::runWithServer(
        [](auto) { FAIL("RPC should fail before invoking handler"); },
        [&](auto& server, auto&) {
            ASSERT_EQ(
                makeStream(server.getListeningAddresses().at(0), true /* Use Auth */, boost::none)
                    .error_code(),
                ::grpc::StatusCode::UNAUTHENTICATED);
        });
    const std::string kAuthToken = "my-auth-token";
    CommandServiceTestFixtures::runWithServer(
        [&](auto session) { ASSERT_EQ(session->authToken(), kAuthToken); },
        [&](auto& server, auto&) {
            ASSERT_EQ(
                makeStream(server.getListeningAddresses().at(0), true /* Use Auth */, kAuthToken)
                    .error_code(),
                ::grpc::StatusCode::OK);
        });
    CommandServiceTestFixtures::runWithServer(
        [](auto session) { ASSERT_FALSE(session->authToken()); },
        [&](auto& server, auto&) {
            ASSERT_EQ(makeStream(server.getListeningAddresses().at(0),
                                 false /* Don't use Auth */,
                                 kAuthToken)
                          .error_code(),
                      ::grpc::StatusCode::OK);
        });
}

TEST_F(CommandServiceTest, ServerProvidesClusterMaxWireVersion) {
    ClientCallbackType clientCallback = [&](auto&, auto stub, auto streamFactory, auto&) {
        ::grpc::ClientContext ctx;
        ctx.AddMetadata(util::constants::kAuthenticationTokenKey.toString(), "my-token");
        ctx.AddMetadata(util::constants::kWireVersionKey.toString(),
                        std::to_string(WireVersion::WIRE_VERSION_50));

        auto stream = streamFactory(ctx, stub);
        ASSERT_TRUE(stream->Write(makeUniqueMessage().sharedBuffer()));

        SharedBuffer buffer;
        ASSERT_TRUE(stream->Read(&buffer));

        auto serverMetadata = ctx.GetServerInitialMetadata();
        auto it = serverMetadata.find(util::constants::kClusterMaxWireVersionKey);
        ASSERT_NE(it, serverMetadata.end());
        ASSERT_EQ(it->second, std::to_string(wireVersionProvider().getClusterMaxWireVersion()));

        ASSERT_TRUE(stream->WritesDone());
        ASSERT_EQ(stream->Finish().error_code(), ::grpc::OK);
    };

    runTestWithBothMethods(CommandServiceTestFixtures::makeEchoHandler(), clientCallback);
}

TEST_F(CommandServiceTest, ServerHandlesMultipleClients) {
    CommandService::RPCHandler serverHandler = [](auto session) {
        while (true) {
            try {
                auto msg = uassertStatusOK(session->sourceMessage());
                auto response = OpMsg::parseOwned(msg);
                response.body = response.body.addFields(BSON(
                    util::constants::kClientIdKey << session->getRemoteClientId()->toString()));
                ASSERT_OK(session->sinkMessage(response.serialize()));
            } catch (ExceptionFor<ErrorCodes::StreamTerminated>&) {
                // Continues to serve the echo commands until the stream is terminated.
                return;
            }
        }
    };

    ClientCallbackType clientCallback = [](auto&, auto stub, auto streamFactory, auto& monitor) {
        const auto kNumClients = 10;
        std::vector<stdx::thread> threads;
        for (auto i = 0; i < kNumClients; i++) {
            threads.push_back(monitor.spawn([&, i] {
                const auto clientId = UUID::gen().toString();

                ::grpc::ClientContext ctx;
                CommandServiceTestFixtures::addRequiredClientMetadata(ctx);
                ctx.AddMetadata(std::string{util::constants::kClientIdKey}, clientId);
                CommandServiceTestFixtures::addClientMetadataDocument(ctx);

                auto stream = streamFactory(ctx, stub);

                OpMsg msg;
                msg.body = BSON("thread" << i);
                ASSERT_TRUE(stream->Write(msg.serialize().sharedBuffer()));

                SharedBuffer receivedMsg;
                ASSERT_TRUE(stream->Read(&receivedMsg));

                auto response = OpMsg::parse(Message{receivedMsg});
                ASSERT_EQ(response.body.getIntField("thread"), i);
                ASSERT_EQ(response.body.getStringField(util::constants::kClientIdKey), clientId);

                ASSERT_TRUE(stream->WritesDone());
                ASSERT_EQ(stream->Finish().error_code(), ::grpc::OK);
            }));
        }

        for (auto& t : threads) {
            t.join();
        }
    };

    runTestWithBothMethods(serverHandler, clientCallback);
}

TEST_F(CommandServiceTest, ServerHandlerThrows) {
    CommandService::RPCHandler serverHandler = [](auto s) {
        s->end();
        iasserted(Status{ErrorCodes::StreamTerminated, "test error"});
    };
    auto server = CommandServiceTestFixtures::makeServer(
        serverHandler, CommandServiceTestFixtures::makeServerOptions());
    server->start();
    ON_BLOCK_EXIT([&server] {
        if (server->isRunning()) {
            server->shutdown();
        }
    });

    auto stub = CommandServiceTestFixtures::makeStub(server->getListeningAddresses().at(0));
    ::grpc::ClientContext ctx;
    CommandServiceTestFixtures::addRequiredClientMetadata(ctx);
    ASSERT_NE(stub.connect().error_code(), ::grpc::StatusCode::OK);
}

TEST_F(CommandServiceTest, Shutdown) {
    std::unique_ptr<Notification<void>> rpcStarted;

    ClientCallbackType clientCallback = [&](auto& server, auto stub, auto streamFactory, auto&) {
        // Initialize the notification for this run.
        rpcStarted = std::make_unique<Notification<void>>();

        ::grpc::ClientContext ctx;
        CommandServiceTestFixtures::addRequiredClientMetadata(ctx);
        auto stream = streamFactory(ctx, stub);

        rpcStarted->get();
        server.shutdown();

        SharedBuffer buffer;
        ASSERT_FALSE(stream->Read(&buffer));
        ASSERT_EQ(stream->Finish().error_code(), ::grpc::StatusCode::CANCELLED);
    };

    CommandService::RPCHandler serverHandler = [&](auto session) {
        rpcStarted->set();
        ASSERT_NOT_OK(session->sourceMessage());

        auto ts = session->terminationStatus();
        ASSERT_TRUE(ts);
        ASSERT_NOT_OK(*ts);
    };

    runTestWithBothMethods(serverHandler, clientCallback);
}

TEST(GRPCInteropTest, SharedBufferDeserialize) {
    auto deserializationTest = [](std::vector<::grpc::Slice> slices, StringData expected) {
        SharedBuffer out;
        ::grpc::ByteBuffer buffer(&slices[0], slices.size());
        auto status = ::grpc::SerializationTraits<SharedBuffer>::Deserialize(&buffer, &out);
        ASSERT_TRUE(status.ok()) << "expected deserialization to succeed: "
                                 << status.error_message();
        ASSERT_EQ(StringData(out.get(), expected.size()), expected);
    };

    StringData expected{"foobar"};
    deserializationTest({std::string{"foobar"}}, expected);
    deserializationTest({std::string{"foo"}, std::string{"bar"}}, expected);
}

TEST(GRPCInteropTest, SerializationRoundTrip) {
    ::grpc::ByteBuffer grpcBuffer;
    auto message = makeUniqueMessage();

    {
        auto buffer = SharedBuffer::allocate(message.capacity());
        std::memcpy(buffer.get(), message.buf(), buffer.capacity());
        bool ownsBuffer;
        auto status = ::grpc::SerializationTraits<ConstSharedBuffer>::Serialize(
            buffer, &grpcBuffer, &ownsBuffer);
        ASSERT_TRUE(status.ok()) << "expected serialization to succeed: " << status.error_message();
    }

    // The source buffer is out of scope, but the serialized gRPC ByteBuffer should still be
    // valid.
    SharedBuffer buffer;
    auto status = ::grpc::SerializationTraits<SharedBuffer>::Deserialize(&grpcBuffer, &buffer);
    ASSERT_TRUE(status.ok()) << "expected deserialization to succeed: " << status.error_message();
    ASSERT_EQ_MSG(Message{buffer}, message);
}

TEST(GRPCInteropTest, URIParsing) {
    {
        HostAndPort hp = grpc::util::parseGRPCFormattedURI("127.0.0.1");
        ASSERT_TRUE(hp.isLocalHost());
    }
    {
        HostAndPort hp = grpc::util::parseGRPCFormattedURI("ipv4:127.0.0.1");
        ASSERT_TRUE(hp.isLocalHost());
    }
    {
        HostAndPort hp = grpc::util::parseGRPCFormattedURI("ipv4:192.168.0.1:123");
        ASSERT_EQ(hp.host(), "192.168.0.1");
        ASSERT_EQ(hp.port(), 123);
    }
    {
        HostAndPort hp = grpc::util::parseGRPCFormattedURI("[::1]");
        ASSERT_TRUE(hp.isLocalHost());
    }
    {
        HostAndPort hp = grpc::util::parseGRPCFormattedURI("ipv6:[::1]");
        ASSERT_TRUE(hp.isLocalHost());
    }
    {
        HostAndPort hp = grpc::util::parseGRPCFormattedURI("ipv6:%5B%3A%3A1%5D");
        ASSERT_TRUE(hp.isLocalHost());
    }
    {
        HostAndPort hp =
            grpc::util::parseGRPCFormattedURI("ipv6:[2001:db8:3333:4444:5555:6666:7777:8888]:123");
        ASSERT_EQ(hp.host(), "2001:db8:3333:4444:5555:6666:7777:8888");
        ASSERT_EQ(hp.port(), 123);
    }

    {
        HostAndPort hp = grpc::util::parseGRPCFormattedURI(
            "ipv6:%5B2001%3Adb8%3A3333%3A4444%3A5555%3A6666%3A7777%3A8888%5D%3A123");
        ASSERT_EQ(hp.host(), "2001:db8:3333:4444:5555:6666:7777:8888");
        ASSERT_EQ(hp.port(), 123);
    }

    {
        HostAndPort hp = grpc::util::parseGRPCFormattedURI("unix:///path/to/socket.sock");
        ASSERT_EQ(hp.host(), "///path/to/socket.sock");
    }

    {
        HostAndPort hp = grpc::util::parseGRPCFormattedURI("unix:%2F%2F%2Fpath%2Fto%2Fsocket.sock");
        ASSERT_EQ(hp.host(), "///path/to/socket.sock");
    }
}

}  // namespace
}  // namespace mongo::transport::grpc
