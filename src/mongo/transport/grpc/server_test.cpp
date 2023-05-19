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

#include <string>

#include <grpcpp/client_context.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/support/status.h>
#include <grpcpp/support/status_code_enum.h>
#include <grpcpp/support/sync_stream.h>

#include "mongo/logv2/log.h"
#include "mongo/stdx/chrono.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/grpc/server.h"
#include "mongo/transport/grpc/test_fixtures.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/net/socket_utils.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::transport::grpc {

class ServerTest : public unittest::Test {
public:
    void runCertificateValidationTest(Server::Options options,
                                      ::grpc::StatusCode validCertResult,
                                      ::grpc::StatusCode noClientCertResult,
                                      ::grpc::StatusCode selfSignedClientCertResult) {
        auto callback = [](IngressSession& session) {
            return ::grpc::Status::OK;
        };

        auto clientThread = [&](Server&, unittest::ThreadAssertionMonitor& monitor) {
            {
                auto stub = CommandServiceTestFixtures::makeStub();
                ASSERT_EQ(stub.connect().error_code(), validCertResult);
            }
            {
                CommandServiceTestFixtures::Stub::Options options;
                options.tlsCAFile = CommandServiceTestFixtures::kCAFile;
                auto stub = CommandServiceTestFixtures::makeStub(options);

                ASSERT_EQ(stub.connect().error_code(), noClientCertResult);
            }
            {
                CommandServiceTestFixtures::Stub::Options options;
                options.tlsCertificateKeyFile =
                    CommandServiceTestFixtures::kClientSelfSignedCertificateKeyFile;
                options.tlsCAFile = CommandServiceTestFixtures::kCAFile;
                auto stub = CommandServiceTestFixtures::makeStub(options);
                ASSERT_EQ(stub.connect().error_code(), selfSignedClientCertResult);
            }
        };

        CommandServiceTestFixtures::runWithServer(callback, clientThread, options);
    }
};

TEST_F(ServerTest, MaxThreads) {
    unittest::Barrier waitForWorkerThreads(CommandServiceTestFixtures::kMaxThreads);
    Notification<void> okayToReturn;

    auto callback = [&](IngressSession& session) {
        waitForWorkerThreads.countDownAndWait();
        ASSERT_OK(session.sinkMessage(makeUniqueMessage()));
        // Block this thread until the test thread notifies it to return.
        okayToReturn.get();
        return ::grpc::Status::OK;
    };

    auto clientThread = [&](Server&, unittest::ThreadAssertionMonitor& monitor) {
        auto stub = CommandServiceTestFixtures::makeStub();
        std::vector<stdx::thread> threads;

        // A minimum of one thread is reserved for the completion queue, so we can only create
        // kMaxThreads - 1 streams.
        for (int i = 0; i < CommandServiceTestFixtures::kMaxThreads - 1; i++) {
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

    CommandServiceTestFixtures::runWithServer(callback, clientThread);
}

TEST_F(ServerTest, ECDSACertificates) {
    const std::string kECDSACAFile = "jstests/libs/ecdsa-ca.pem";

    auto options = CommandServiceTestFixtures::makeServerOptions();
    options.tlsPEMKeyFile = "jstests/libs/ecdsa-server.pem";
    options.tlsCAFile = kECDSACAFile;

    auto callback = [](auto&) {
        return ::grpc::Status::OK;
    };

    CommandServiceTestFixtures::runWithServer(
        callback,
        [&](auto&, auto&) {
            auto stubOptions = CommandServiceTestFixtures::Stub::Options{};
            stubOptions.tlsCAFile = kECDSACAFile;
            stubOptions.tlsCertificateKeyFile = "jstests/libs/ecdsa-client.pem";

            auto stub = CommandServiceTestFixtures::makeStub(stubOptions);
            ASSERT_EQ(stub.connect().error_code(), ::grpc::StatusCode::OK);
        },
        options);
}

TEST_F(ServerTest, IntermediateCA) {
    auto options = CommandServiceTestFixtures::makeServerOptions();
    options.tlsPEMKeyFile = "jstests/libs/server-intermediate-ca.pem";

    auto callback = [](auto&) {
        return ::grpc::Status::OK;
    };

    CommandServiceTestFixtures::runWithServer(
        callback,
        [&](auto&, auto&) {
            auto stub = CommandServiceTestFixtures::makeStub();
            ASSERT_EQ(stub.connect().error_code(), ::grpc::StatusCode::OK);
        },
        options);
}

TEST_F(ServerTest, InvalidServerCertificateOptions) {
    const std::string kMissingPrivateKeyPath = "jstests/libs/ecdsa-ca-ocsp.crt";
    const std::string kNonExistentPath = "non_existent_path";

    auto runInvalidCertTest = [&](Server::Options options) {
        auto callback = [](auto session) {
            return ::grpc::Status::OK;
        };
        auto server = CommandServiceTestFixtures::makeServer(callback, options);
        ASSERT_THROWS(server.start(), DBException);
    };

    {
        auto options = CommandServiceTestFixtures::makeServerOptions();
        options.tlsPEMKeyFile = kNonExistentPath;
        runInvalidCertTest(options);
    }
    {
        auto options = CommandServiceTestFixtures::makeServerOptions();
        options.tlsPEMKeyFile = CommandServiceTestFixtures::kServerCertificateKeyFile;
        options.tlsCAFile = kNonExistentPath;
        runInvalidCertTest(options);
    }
    {
        auto options = CommandServiceTestFixtures::makeServerOptions();
        options.tlsPEMKeyFile = kMissingPrivateKeyPath;
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
    auto addresses = std::vector<std::string>{
        "localhost", "127.0.0.1", "[::1]", makeUnixSockPath(CommandServiceTestFixtures::kBindPort)};

    Server::Options options = CommandServiceTestFixtures::makeServerOptions();
    options.addresses = addresses;
    // Use this certificate because the default one doesn't have SANs associated with all of the
    // addresses being tested here.
    options.tlsPEMKeyFile = "jstests/libs/server_SAN.pem";

    auto callback = [](IngressSession& session) {
        return ::grpc::Status::OK;
    };

    CommandServiceTestFixtures::runWithServer(
        callback,
        [&addresses](auto&, auto&) {
            for (auto& address : addresses) {
                std::string fullAddress;
                if (isUnixDomainSocket(address)) {
                    fullAddress = "unix://{}"_format(address);
                } else {
                    fullAddress = "{}:{}"_format(address, CommandServiceTestFixtures::kBindPort);
                }
                auto stub = CommandServiceTestFixtures::makeStub(fullAddress);
                ASSERT_EQ(stub.connect().error_code(), ::grpc::StatusCode::OK)
                    << "failed to connect to " << address;
            }
        },
        options);
}

}  // namespace mongo::transport::grpc
