/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/transport/handoff/handoff_transport_layer.h"

#include "mongo/base/error_codes.h"
#include "mongo/transport/handoff/handoff_test_util.h"
#include "mongo/transport/session.h"
#include "mongo/unittest/join_thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/processinfo.h"

#include <array>
#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <fmt/format.h>

namespace mongo::transport {
namespace {

constexpr int kTestPort = 27017;

struct SocketPaths {
    std::filesystem::path standard, loadBalanced, priority;

    std::array<std::filesystem::path, 3> paths() const {
        return {standard, loadBalanced, priority};
    }

    explicit SocketPaths(const std::filesystem::path& prefix, int port = kTestPort)
        : standard(prefix / fmt::format("handoff-mongodb-{}.sock", port)),
          loadBalanced(prefix / fmt::format("handoff-mongodb-load-balanced-{}.sock", port)),
          priority(prefix / fmt::format("handoff-mongodb-priority-{}.sock", port)) {}
};

// setup() tests
// -------------

/**
 * Verifies that socket filenames include the port number, and that port-free names are not
 * created. This guards against regressions where two processes on the same host with the same
 * socketPrefix would clobber each other's sockets.
 * Additionally verifies that on shutdown, those same socket files are removed.
 */
TEST(HandoffTransportLayerTest, SocketFilenamesIncludePort) {
    TemporaryDirectory dir;
    constexpr int port = 12345;
    HandoffTransportLayer transportLayer({
        .socketPrefix = dir.path(),
        .port = port,
        .listenBacklog = ProcessInfo::getDefaultListenBacklog(),
        .sessionManager = std::make_unique<TestSessionManager>(),
    });
    ASSERT_OK(transportLayer.setup());

    const std::string withPorts[] = {
        "handoff-mongodb-12345.sock",
        "handoff-mongodb-priority-12345.sock",
        "handoff-mongodb-load-balanced-12345.sock",
    };
    const std::string withoutPorts[]{
        "handoff-mongodb.sock",
        "handoff-mongodb-priority.sock",
        "handoff-mongodb-load-balanced.sock",
    };

    for (const std::string& stem : withPorts) {
        ASSERT_TRUE(exists(dir.path() / stem));
    }
    for (const std::string& stem : withoutPorts) {
        ASSERT_FALSE(exists(dir.path() / stem));
    }

    transportLayer.shutdown();
    for (const std::string& stem : withPorts) {
        ASSERT_FALSE(exists(dir.path() / stem));
    }
    for (const std::string& stem : withoutPorts) {
        ASSERT_FALSE(exists(dir.path() / stem));
    }
}

/**
 * Verifies that if anything goes wrong with one of the listener threads in setup(), the other
 * listener threads are also not set up.
 * Cover the failure case for each listening socket.
 */
TEST(HandoffTransportLayerTest, SetupSucceedsCompletelyOrFailsCompletely) {
    const size_t numListeningSockets = SocketPaths("dummy").paths().size();
    for (size_t i = 0; i < numListeningSockets; ++i) {
        TemporaryDirectory dir;
        auto posix = std::make_unique<MockPOSIXInterface>();
        // setup() does a few things: socket(), bind(), etc.
        // Let's make it fail with socket(). Any of its system dependencies would do.
        // Fail only the i'th time, so that the other listener could succeed if we were to try it.
        // What we're testing is that we _don't_ try it.
        std::atomic<size_t> callCount = 0;
        posix->onSocket = [&](int domain, int type, int protocol) {
            if (callCount.fetch_add(1) == i) {
                return -1;
            }
            return ::socket(domain, type, protocol);
        };

        HandoffTransportLayer transportLayer(
            {.socketPrefix = dir.path(),
             .port = kTestPort,
             .listenBacklog = ProcessInfo::getDefaultListenBacklog(),
             .sessionManager = std::make_unique<TestSessionManager>(),
             .posix = std::move(posix)});

        ASSERT_EQ(transportLayer.setup().code(), ErrorCodes::SocketException) << "i=" << i;
        for (const auto& path : SocketPaths(dir.path()).paths()) {
            ASSERT_FALSE(exists(path))
                << "(i=" << i
                << ") expected that setup failure would prevent the creation of: " << path;
        }
    }
}

// start() tests
// -------------

/** Verifies that clients can connect to the unix domain sockets after start() succeeds. */
TEST(HandoffTransportLayerTest, CanConnectAfterStart) {
    TemporaryDirectory dir;
    HandoffTransportLayer transportLayer({
        .socketPrefix = dir.path(),
        .port = kTestPort,
        .listenBacklog = ProcessInfo::getDefaultListenBacklog(),
        .sessionManager = std::make_unique<TestSessionManager>(),
    });
    ASSERT_OK(transportLayer.setup());
    ASSERT_OK(transportLayer.start());

    for (const auto& path : SocketPaths(dir.path()).paths()) {
        const int fd = connectUnixSocket(path);
        ASSERT_NE(fd, -1) << "expected to be able to connect to: " << path;
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }

    transportLayer.shutdown();
}

/** Verifies that, after start() succeeds, an incoming connection results in a new session. */
TEST(HandoffTransportLayerTest, AfterStartConnectionsCreateSessions) {
    TemporaryDirectory dir;
    auto sessionManagerOwned = std::make_unique<TestSessionManager>();
    auto& sessionManager = *sessionManagerOwned;
    HandoffTransportLayer transportLayer({
        .socketPrefix = dir.path(),
        .port = kTestPort,
        .listenBacklog = ProcessInfo::getDefaultListenBacklog(),
        .sessionManager = std::move(sessionManagerOwned),
    });
    ASSERT_OK(transportLayer.setup());
    ASSERT_OK(transportLayer.start());

    for (const auto& path : SocketPaths(dir.path()).paths()) {
        const auto session = connectAndGetSession(path, sessionManager);
        ASSERT_NE(session, nullptr);
        session->end();
    }

    transportLayer.shutdown();
}

/**
 * Verifies that each of the listening sockets produces sessions having the expected properties
 * with respect to priority status. and load balancer support.
 */
TEST(HandoffTransportLayerTest, DifferentSocketsProduceDifferentSessionProperties) {
    TemporaryDirectory dir;
    auto sessionManagerOwned = std::make_unique<TestSessionManager>();
    auto& sessionManager = *sessionManagerOwned;
    HandoffTransportLayer transportLayer({
        .socketPrefix = dir.path(),
        .port = kTestPort,
        .listenBacklog = ProcessInfo::getDefaultListenBacklog(),
        .sessionManager = std::move(sessionManagerOwned),
    });
    ASSERT_OK(transportLayer.setup());
    ASSERT_OK(transportLayer.start());

    const SocketPaths paths(dir.path());

    // standard socket (corresponds to the main port that mongod/s can listen on)
    {
        const auto session = connectAndGetSession(paths.standard, sessionManager);
        ASSERT_NE(session, nullptr);
        ASSERT_FALSE(session->isConnectedToPriorityPort());
        ASSERT_FALSE(session->isConnectedToLoadBalancerPort());
        ASSERT_TRUE(session->isConnectedToProxyUnixSocket());
        session->end();
    }

    // priority socket (corresponds to the priority port that mongod/s can listen on)
    {
        const auto session = connectAndGetSession(paths.priority, sessionManager);
        ASSERT_NE(session, nullptr);
        ASSERT_TRUE(session->isConnectedToPriorityPort());
        ASSERT_FALSE(session->isConnectedToLoadBalancerPort());
        ASSERT_TRUE(session->isConnectedToProxyUnixSocket());
        session->end();
    }

    // load balanced socket (corresponds to the load balanced port that mongod/s can listen on)
    {
        const auto session = connectAndGetSession(paths.loadBalanced, sessionManager);
        ASSERT_NE(session, nullptr);
        ASSERT_FALSE(session->isConnectedToPriorityPort());
        ASSERT_TRUE(session->isConnectedToLoadBalancerPort());
        ASSERT_TRUE(session->isConnectedToProxyUnixSocket());
        session->end();
    }

    transportLayer.shutdown();
}

/**
 * Verifies that any listen() failure causes start() to fail.
 * start() will cause listen() to be called multiple times: once per listening socket, barring
 * failures.
 * Cover the failure case for each listening socket.
 */
TEST(HandoffTransportLayerTest, StartSucceedsCompletelyOrFailsCompletely) {
    const int numListeningSockets = SocketPaths("dummy").paths().size();
    for (int i = 0; i < numListeningSockets; ++i) {
        TemporaryDirectory dir;
        auto posix = std::make_unique<MockPOSIXInterface>();
        // setup() does a few things: socket(), bind(), etc.
        // Let's make it fail with socket(). Any of its system dependencies would do.
        // Fail only the first time, so that the other listener could succeed if we were to try it.
        // What we're testing is that we _don't_ try it.
        std::atomic<int> callCount = 0;
        posix->onListen = [&](int fd, int backlog) {
            if (callCount.fetch_add(1) == i) {
                return -1;
            }
            return ::listen(fd, backlog);
        };

        HandoffTransportLayer transportLayer(
            {.socketPrefix = dir.path(),
             .port = kTestPort,
             .listenBacklog = ProcessInfo::getDefaultListenBacklog(),
             .sessionManager = std::make_unique<TestSessionManager>(),
             .posix = std::move(posix)});

        ASSERT_OK(transportLayer.setup()) << "i=" << i;
        ASSERT_EQ(transportLayer.start().code(), ErrorCodes::SocketException) << "i=" << i;

        transportLayer.shutdown();
    }
}

// stopAcceptingSessions()/shutdown() tests
// ----------------

/**
 * The following two tests verify that stopAcceptingSession() and shutdown() cause subsequent
 * connection attempts to fail. They are very similar to the tests with the same names in
 * `handoff_listener_thread_test.cpp`.
 *
 * This is the shared implementation between StopAcceptingSessionsStopsAcceptingSessions and
 * ShutdownStopsAcceptingSessions.
 */
void verifyCannotConnectAfter(const std::function<void(HandoffTransportLayer&)>& afterWhat) {
    TemporaryDirectory dir;
    const SocketPaths paths(dir.path());
    HandoffTransportLayer transportLayer(
        {.socketPrefix = dir.path(),
         .port = kTestPort,
         .listenBacklog = ProcessInfo::getDefaultListenBacklog(),
         .sessionManager = std::make_unique<TestSessionManager>()});
    ASSERT_OK(transportLayer.setup());
    ASSERT_OK(transportLayer.start());

    // Verify that we can connect _before_ `afterWhat`.
    for (const auto& path : paths.paths()) {
        const int fd = connectUnixSocket(path);
        ASSERT_NE(fd, -1);
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }

    afterWhat(transportLayer);
    // Verify that a connect attempt fails _after_ `afterWhat`.
    for (const auto& path : paths.paths()) {
        const int fd = connectUnixSocket(path);
        ASSERT_EQ(fd, -1);
    }

    transportLayer.shutdown();
}

/** Verifies that stopAcceptingSessions() stops accepting sessions. */
TEST(HandoffTransportLayerTest, StopAcceptingSessionsStopsAcceptingSessions) {
    verifyCannotConnectAfter(
        [](HandoffTransportLayer& transportLayer) { transportLayer.stopAcceptingSessions(); });
}

/** Verifies that shutdown() stops accepting sessions. */
TEST(HandoffTransportLayerTest, ShutdownStopsAcceptingSessions) {
    verifyCannotConnectAfter(
        [](HandoffTransportLayer& transportLayer) { transportLayer.shutdown(); });
}

TEST(HandoffTransportLayerTest, SessionManagerShutdownTimeout) {
    TemporaryDirectory dir;
    auto sessionManagerOwned =
        std::make_unique<TestSessionManager>(TestSessionManager::Params{.shutdownResult = false});
    HandoffTransportLayer transportLayer({
        .socketPrefix = dir.path(),
        .port = kTestPort,
        .listenBacklog = ProcessInfo::getDefaultListenBacklog(),
        .sessionManager = std::move(sessionManagerOwned),
    });
    ASSERT_OK(transportLayer.setup());
    ASSERT_OK(transportLayer.start());
    transportLayer.shutdown();
}

TEST(HandoffTransportLayerTest, ShutdownIsIdempotent) {
    TemporaryDirectory dir;
    HandoffTransportLayer transportLayer({
        .socketPrefix = dir.path(),
        .port = kTestPort,
        .listenBacklog = ProcessInfo::getDefaultListenBacklog(),
        .sessionManager = std::make_unique<TestSessionManager>(),
    });
    ASSERT_OK(transportLayer.setup());
    ASSERT_OK(transportLayer.start());
    for (int i = 0; i < 5; ++i) {
        transportLayer.shutdown();
    }
}

// Unimplemented or hard-coded TransportLayer overrides
// ----------------------------------------------------

TEST(HandoffTransportLayerTest, GetNameForLogging) {
    TemporaryDirectory dir;
    HandoffTransportLayer transportLayer({
        .socketPrefix = dir.path(),
        .port = kTestPort,
        .listenBacklog = ProcessInfo::getDefaultListenBacklog(),
        .sessionManager = std::make_unique<TestSessionManager>(),
    });
    ASSERT_EQ(transportLayer.getNameForLogging(), "HandoffTransportLayer");
}

TEST(HandoffTransportLayerTest, GetReactorReturnsNull) {
    TemporaryDirectory dir;
    HandoffTransportLayer transportLayer({
        .socketPrefix = dir.path(),
        .port = kTestPort,
        .listenBacklog = ProcessInfo::getDefaultListenBacklog(),
        .sessionManager = std::make_unique<TestSessionManager>(),
    });
    ASSERT_EQ(transportLayer.getReactor(TransportLayer::WhichReactor::kIngress), nullptr);
}

TEST(HandoffTransportLayerTest, GetTransportProtocol) {
    TemporaryDirectory dir;
    HandoffTransportLayer transportLayer({
        .socketPrefix = dir.path(),
        .port = kTestPort,
        .listenBacklog = ProcessInfo::getDefaultListenBacklog(),
        .sessionManager = std::make_unique<TestSessionManager>(),
    });
    ASSERT_EQ(transportLayer.getTransportProtocol(), TransportProtocol::MongoRPC);
}

TEST(HandoffTransportLayerTest, GetSessionManager) {
    TemporaryDirectory dir;
    auto sessionManagerOwned = std::make_unique<TestSessionManager>();
    auto* sessionManagerPtr = sessionManagerOwned.get();
    HandoffTransportLayer transportLayer({
        .socketPrefix = dir.path(),
        .port = kTestPort,
        .listenBacklog = ProcessInfo::getDefaultListenBacklog(),
        .sessionManager = std::move(sessionManagerOwned),
    });
    ASSERT_EQ(transportLayer.getSessionManager(), sessionManagerPtr);
    ASSERT_EQ(transportLayer.getSharedSessionManager().get(), sessionManagerPtr);
}

TEST(HandoffTransportLayerTest, IsIngressNotEgress) {
    TemporaryDirectory dir;
    HandoffTransportLayer transportLayer({
        .socketPrefix = dir.path(),
        .port = kTestPort,
        .listenBacklog = ProcessInfo::getDefaultListenBacklog(),
        .sessionManager = std::make_unique<TestSessionManager>(),
    });
    ASSERT_TRUE(transportLayer.isIngress());
    ASSERT_FALSE(transportLayer.isEgress());
}

TEST(HandoffTransportLayerTest, ConnectReturnsIllegalOperation) {
    TemporaryDirectory dir;
    HandoffTransportLayer transportLayer({
        .socketPrefix = dir.path(),
        .port = kTestPort,
        .listenBacklog = ProcessInfo::getDefaultListenBacklog(),
        .sessionManager = std::make_unique<TestSessionManager>(),
    });
    auto result = transportLayer.connect({}, ConnectSSLMode::kDisableSSL, Milliseconds{0}, {});
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::IllegalOperation);
}

TEST(HandoffTransportLayerTest, AsyncConnectReturnsIllegalOperation) {
    TemporaryDirectory dir;
    HandoffTransportLayer transportLayer({
        .socketPrefix = dir.path(),
        .port = kTestPort,
        .listenBacklog = ProcessInfo::getDefaultListenBacklog(),
        .sessionManager = std::make_unique<TestSessionManager>(),
    });
    auto result =
        transportLayer
            .asyncConnect(
                {}, ConnectSSLMode::kDisableSSL, nullptr, Milliseconds{0}, nullptr, nullptr)
            .getNoThrow();
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::IllegalOperation);
}

#ifdef MONGO_CONFIG_SSL
TEST(HandoffTransportLayerTest, RotateCertificatesReturnsOK) {
    TemporaryDirectory dir;
    HandoffTransportLayer transportLayer({
        .socketPrefix = dir.path(),
        .port = kTestPort,
        .listenBacklog = ProcessInfo::getDefaultListenBacklog(),
        .sessionManager = std::make_unique<TestSessionManager>(),
    });
    ASSERT_OK(transportLayer.rotateCertificates(nullptr, false));
}

TEST(HandoffTransportLayerTest, CreateTransientSSLContextReturnsIllegalOperation) {
    TemporaryDirectory dir;
    HandoffTransportLayer transportLayer({
        .socketPrefix = dir.path(),
        .port = kTestPort,
        .listenBacklog = ProcessInfo::getDefaultListenBacklog(),
        .sessionManager = std::make_unique<TestSessionManager>(),
    });
    auto result = transportLayer.createTransientSSLContext(TransientSSLParams{TLSCredentials{}});
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::IllegalOperation);
}
#endif  // MONGO_CONFIG_SSL

// Threading tests
// ---------------
// These tests perform operations on HandoffTransportLayer from multiple threads, mostly to see if
// thread sanitizer catches any data races.

/** Verifies that multiple threads can establish and close sessions concurrently. */
TEST(HandoffTransportLayerTest, MultithreadedConnections) {
    TemporaryDirectory dir;
    auto sessionManagerOwned = std::make_unique<TestSessionManager>();
    auto& sessionManager = *sessionManagerOwned;
    HandoffTransportLayer transportLayer({
        .socketPrefix = dir.path(),
        .port = kTestPort,
        .listenBacklog = ProcessInfo::getDefaultListenBacklog(),
        .sessionManager = std::move(sessionManagerOwned),
    });
    ASSERT_OK(transportLayer.setup());
    ASSERT_OK(transportLayer.start());

    const int numClientThreads = 3;
    const int numConnectionsPerClientThread = 10;
    {
        const SocketPaths paths(dir.path());
        unittest::JoinThread closer([&]() {
            for (int i = 0; i < numClientThreads * numConnectionsPerClientThread; ++i) {
                const auto session = sessionManager.popSession();
                ASSERT_NE(session, nullptr);
                session->end();
            }
        });

        std::vector<unittest::JoinThread> clientThreads;
        for (int i = 0; i < numClientThreads; ++i) {
            clientThreads.emplace_back([&]() {
                const auto pathsArray = paths.paths();
                for (int i = 0; i < numConnectionsPerClientThread; ++i) {
                    const int fd = connectUnixSocket(pathsArray[i % pathsArray.size()]);
                    ASSERT_NE(fd, -1);
                    ::close(fd);
                }
            });
        }
    }

    ASSERT_EQ(sessionManager.getNumSessionsStarted(),
              numClientThreads * numConnectionsPerClientThread);
    transportLayer.shutdown();
}

}  // namespace
}  // namespace mongo::transport
