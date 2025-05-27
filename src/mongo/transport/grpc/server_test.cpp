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

#include "mongo/transport/grpc/server.h"

#include "mongo/logv2/log.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/grpc/test_fixtures.h"
#include "mongo/transport/grpc/util.h"
#include "mongo/transport/test_fixtures.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/socket_utils.h"

#include <string>

#include <grpcpp/client_context.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/support/status.h>
#include <grpcpp/support/status_code_enum.h>
#include <grpcpp/support/sync_stream.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::transport::grpc {

class ServerTest : public CommandServiceTestFixtures {
public:
    void runCertificateValidationTest(Server::Options options,
                                      ::grpc::StatusCode validCertResult,
                                      ::grpc::StatusCode noClientCertResult,
                                      ::grpc::StatusCode selfSignedClientCertResult) {
        auto clientThread = [&](Server& s, unittest::ThreadAssertionMonitor& monitor) {
            auto addr = s.getListeningAddresses().at(0);

            // For connection attempts that are expected to fail, use a shorter timeout to speed up
            // the test.
            auto getTimeout = [](::grpc::StatusCode expected) {
                if (expected == ::grpc::StatusCode::OK) {
                    return CommandServiceTestFixtures::kDefaultConnectTimeout;
                } else {
                    return Milliseconds(50);
                }
            };
            {
                auto stub = CommandServiceTestFixtures::makeStub(addr);
                ASSERT_EQ(stub.connect(getTimeout(validCertResult)).error_code(), validCertResult);
            }
            {
                CommandServiceTestFixtures::Stub::Options options;
                options.tlsCAFile = CommandServiceTestFixtures::kCAFile;
                auto stub = CommandServiceTestFixtures::makeStub(addr, options);

                ASSERT_EQ(stub.connect(getTimeout(noClientCertResult)).error_code(),
                          noClientCertResult);
            }
            {
                CommandServiceTestFixtures::Stub::Options options;
                options.tlsCertificateKeyFile =
                    CommandServiceTestFixtures::kClientSelfSignedCertificateKeyFile;
                options.tlsCAFile = CommandServiceTestFixtures::kCAFile;
                auto stub = CommandServiceTestFixtures::makeStub(addr, options);
                ASSERT_EQ(stub.connect(getTimeout(selfSignedClientCertResult)).error_code(),
                          selfSignedClientCertResult);
            }
        };

        CommandServiceTestFixtures::runWithServer([](auto) {}, clientThread, options);
    }
};

TEST_F(ServerTest, MaxThreads) {
    constexpr auto kMaxThreads = 12;
    unittest::Barrier waitForWorkerThreads(kMaxThreads);
    Notification<void> okayToReturn;

    auto callback = [&](auto session) {
        waitForWorkerThreads.countDownAndWait();
        ASSERT_OK(session->sinkMessage(makeUniqueMessage()));
        // Block this thread until the test thread notifies it to return.
        okayToReturn.get();
    };

    auto clientThread = [&](Server& s, unittest::ThreadAssertionMonitor& monitor) {
        auto stub = CommandServiceTestFixtures::makeStub(s.getListeningAddresses().at(0));
        std::vector<stdx::thread> threads;

        // A minimum of one thread is reserved for the completion queue, so we can only create
        // kMaxThreads - 1 streams.
        for (int i = 0; i < kMaxThreads - 1; i++) {
            auto thread = monitor.spawn([&, i]() {
                ::grpc::ClientContext ctx;
                CommandServiceTestFixtures::addRequiredClientMetadata(ctx);
                auto stream = stub.unauthenticatedCommandStream(&ctx);
                SharedBuffer msg;
                ASSERT_TRUE(stream->Read(&msg));
                // We don't need to block this thread, since the gRPC thread does not return until
                // we set `okayToReturn`.
            });
            threads.push_back(std::move(thread));
        }

        waitForWorkerThreads.countDownAndWait();

        // Now that we've reached the maximum number of threads, the next RPC should fail.
        {
            auto status = stub.connect();
            ASSERT_EQ(status.error_code(), ::grpc::StatusCode::RESOURCE_EXHAUSTED)
                << status.error_message();
        }

        okayToReturn.set();
        for (auto& thread : threads) {
            thread.join();
        }
    };

    auto serverOptions = CommandServiceTestFixtures::makeServerOptions();
    serverOptions.maxThreads = kMaxThreads;
    CommandServiceTestFixtures::runWithServer(callback, clientThread, std::move(serverOptions));
}

TEST_F(ServerTest, ECDSACertificates) {
    const std::string kECDSACAFile = "jstests/libs/ecdsa-ca.pem";

    auto options = CommandServiceTestFixtures::makeServerOptions();
    options.tlsCertificateKeyFile = "jstests/libs/ecdsa-server.pem";
    options.tlsCAFile = kECDSACAFile;

    CommandServiceTestFixtures::runWithServer(
        [](auto) {},
        [&](auto& s, auto&) {
            auto stubOptions = CommandServiceTestFixtures::Stub::Options{};
            stubOptions.tlsCAFile = kECDSACAFile;
            stubOptions.tlsCertificateKeyFile = "jstests/libs/ecdsa-client.pem";

            auto stub =
                CommandServiceTestFixtures::makeStub(s.getListeningAddresses().at(0), stubOptions);
            ASSERT_EQ(stub.connect().error_code(), ::grpc::StatusCode::OK);
        },
        options);
}

TEST_F(ServerTest, IntermediateCA) {
    auto options = CommandServiceTestFixtures::makeServerOptions();
    options.tlsCertificateKeyFile = "jstests/libs/server-intermediate-ca.pem";

    CommandServiceTestFixtures::runWithServer(
        [](auto) {},
        [&](Server& s, auto&) {
            auto stub = CommandServiceTestFixtures::makeStub(s.getListeningAddresses().at(0));
            ASSERT_EQ(stub.connect().error_code(), ::grpc::StatusCode::OK);
        },
        options);
}

TEST_F(ServerTest, InvalidServerCertificateOptions) {
    const std::string kMissingPrivateKeyPath = "jstests/libs/ecdsa-ca-ocsp.crt";
    const std::string kNonExistentPath = "non_existent_path";

    auto runInvalidCertTest = [&](Server::Options options) {
        auto server = CommandServiceTestFixtures::makeServer({}, options);
        ASSERT_THROWS(server->start(), DBException);
    };

    {
        auto options = CommandServiceTestFixtures::makeServerOptions();
        options.tlsCertificateKeyFile = kNonExistentPath;
        runInvalidCertTest(options);
    }
    {
        auto options = CommandServiceTestFixtures::makeServerOptions();
        options.tlsCertificateKeyFile = CommandServiceTestFixtures::kServerCertificateKeyFile;
        options.tlsCAFile = kNonExistentPath;
        runInvalidCertTest(options);
    }
    {
        auto options = CommandServiceTestFixtures::makeServerOptions();
        options.tlsCertificateKeyFile = kMissingPrivateKeyPath;
        runInvalidCertTest(options);
    }
}

TEST_F(ServerTest, DefaultClientCertificateValidation) {
    runCertificateValidationTest(
        CommandServiceTestFixtures::makeServerOptions(),
        /* valid client cert succeeds */ ::grpc::StatusCode::OK,
        /* no client cert fails */ ::grpc::StatusCode::UNAVAILABLE,
        /* self-signed client cert fails */ ::grpc::StatusCode::UNAVAILABLE);
}

TEST_F(ServerTest, AllowConnectionsWithoutCertificates) {
    auto serverOptions = CommandServiceTestFixtures::makeServerOptions();
    serverOptions.tlsAllowConnectionsWithoutCertificates = true;

    runCertificateValidationTest(
        serverOptions,
        /* valid client cert succeeds */ ::grpc::StatusCode::OK,
        /* no client cert succeeds */ ::grpc::StatusCode::OK,
        /* self-signed client cert fails */ ::grpc::StatusCode::UNAVAILABLE);
}

TEST_F(ServerTest, AllowInvalidClientCertificate) {
    auto serverOptions = CommandServiceTestFixtures::makeServerOptions();
    serverOptions.tlsAllowInvalidCertificates = true;

    runCertificateValidationTest(serverOptions,
                                 /* valid client cert succeeds */ ::grpc::StatusCode::OK,
                                 /* no client cert fails */ ::grpc::StatusCode::UNAVAILABLE,
                                 /* self-signed client cert succeeds */ ::grpc::StatusCode::OK);
}

TEST_F(ServerTest, DisableCertificateValidation) {
    auto serverOptions = CommandServiceTestFixtures::makeServerOptions();
    serverOptions.tlsAllowInvalidCertificates = true;
    serverOptions.tlsAllowConnectionsWithoutCertificates = true;

    runCertificateValidationTest(serverOptions,
                                 /* valid client cert succeeds */ ::grpc::StatusCode::OK,
                                 /* no client cert succeeds */ ::grpc::StatusCode::OK,
                                 /* self-signed client cert succeeds */ ::grpc::StatusCode::OK);
}

TEST_F(ServerTest, MultipleAddresses) {
    std::vector<HostAndPort> addresses{
        HostAndPort("localhost", test::kLetKernelChoosePort),
        HostAndPort("127.0.0.1", test::kLetKernelChoosePort),
        HostAndPort("::1", test::kLetKernelChoosePort),
        HostAndPort(makeUnixSockPath(test::kLetKernelChoosePort, "grpc-multiple-addresses-test"))};

    Server::Options options = CommandServiceTestFixtures::makeServerOptions();
    options.addresses = addresses;

    CommandServiceTestFixtures::runWithServer(
        [](auto) {},
        [&addresses](auto& server, auto&) {
            auto boundAddresses = server.getListeningAddresses();
            ASSERT_EQ(boundAddresses.size(), addresses.size())
                << "not all provided addresses were bound to";
            for (auto& address : boundAddresses) {
                auto uri = util::toGRPCFormattedURI(address);
                auto stub = CommandServiceTestFixtures::makeStub(uri);
                ASSERT_EQ(stub.connect().error_code(), ::grpc::StatusCode::OK)
                    << "failed to connect to " << address;
            }
        },
        options);
}

}  // namespace mongo::transport::grpc
