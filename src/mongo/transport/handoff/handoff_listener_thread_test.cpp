// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/handoff/handoff_listener_thread.h"

#include "mongo/base/error_codes.h"
#include "mongo/transport/handoff/handoff_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/processinfo.h"

#include <atomic>
#include <barrier>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <utility>

#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>

namespace mongo::transport {
namespace {

using ListeningSocket = HandoffListenerThread::ListeningSocket;

/**
 * Constructs Params having a single socket at `path`, and using the provided mock session manager
 * and system calls. This is a shorthand for brevity.
 */
HandoffListenerThread::Params singleSocketParams(const std::filesystem::path& path,
                                                 TestSessionManager& sessionManager,
                                                 POSIXInterface& posix) {
    return {
        .sockets = {ListeningSocket{.path = path, .isLoadBalanced = false, .isPriority = false}},
        .listenBacklog = ProcessInfo::getDefaultListenBacklog(),
        .sessionManager = &sessionManager,
        // The `HandoffSession` objects created by the listener need a `TransportLayer*` on account
        // of their interface, but they don't actually use the transport layer, so in test we can
        // use nullptr.
        .transportLayer = nullptr,
        .posix = posix,
    };
}

// setup() tests
// -------------

TEST(HandoffListenerThreadTest, SocketFileCreated) {
    TemporaryDirectory dir;
    POSIXInterface posix;
    TestSessionManager sessionManager;
    const std::filesystem::path socketPath = dir.path() / "handoff-mongodb.sock";
    HandoffListenerThread listener(singleSocketParams(socketPath, sessionManager, posix));
    ASSERT_OK(listener.setup({.s2nConfig = nullptr}));
    ASSERT_EQ(std::filesystem::status(socketPath).type(), std::filesystem::file_type::socket);
    listener.shutdown();
}

TEST(HandoffListenerThreadTest, SocketFilePermissions) {
    TemporaryDirectory dir;
    POSIXInterface posix;
    TestSessionManager sessionManager;
    const std::filesystem::path socketPath = dir.path() / "handoff-mongodb.sock";
    HandoffListenerThread listener(singleSocketParams(socketPath, sessionManager, posix));
    ASSERT_OK(listener.setup({.s2nConfig = nullptr}));
    struct stat st;
    ASSERT_EQ(::stat(socketPath.c_str(), &st), 0);
    // Socket files should be "owner read/write, group read/write, other none" i.e. octal 660.
    ASSERT_EQ(st.st_mode & 0777, 0660u);
    listener.shutdown();
}

// setup() calls unlink() before binding, so a stale file at the socket path must not block it.
TEST(HandoffListenerThreadTest, StaleSocketUnlinked) {
    TemporaryDirectory dir;
    POSIXInterface posix;
    TestSessionManager sessionManager;
    const std::filesystem::path socketPath = dir.path() / "handoff-mongodb.sock";
    {
        // Create an empty file at socketPath and then close it.
        std::ofstream{socketPath};
    }
    HandoffListenerThread listener(singleSocketParams(socketPath, sessionManager, posix));
    // No trouble binding.
    ASSERT_OK(listener.setup({.s2nConfig = nullptr}));
    listener.shutdown();
}

// setup() fails when the path length is >= sizeof(sun_path), because in order to bind() a unix
// domain socket its path must fit in sun_path, including a null terminator.
TEST(HandoffListenerThreadTest, PathTooLong) {
    TemporaryDirectory dir;
    POSIXInterface posix;
    TestSessionManager sessionManager;
    constexpr std::size_t kSunPathSize = sizeof(::sockaddr_un::sun_path);
    // more than large enough
    std::string longSubdir(kSunPathSize, 'x');
    const auto longPrefix = dir.path() / longSubdir;
    std::filesystem::create_directories(longPrefix);
    HandoffListenerThread listener(
        singleSocketParams(longPrefix / "handoff-mongodb.sock", sessionManager, posix));
    ASSERT_EQ(listener.setup({.s2nConfig = nullptr}).code(), ErrorCodes::InvalidOptions);
    listener.shutdown();
}

// After successful setup() and shutdown(), no file descriptors are leaked.
TEST(HandoffListenerThreadTest, NoFdLeakOnSetupAndShutdown) {
    const std::size_t fdsBefore = numberOfOpenFileDescriptors();
    {
        TemporaryDirectory dir;
        POSIXInterface posix;
        TestSessionManager sessionManager;
        const std::filesystem::path socketPath = dir.path() / "handoff-mongodb.sock";
        HandoffListenerThread listener(singleSocketParams(socketPath, sessionManager, posix));
        ASSERT_OK(listener.setup({.s2nConfig = nullptr}));
        listener.shutdown();
    }
    ASSERT_EQ(numberOfOpenFileDescriptors(), fdsBefore);
}

// bind() failure must close the socket fd and leave no socket file.
TEST(HandoffListenerThreadTest, BindFailureCleansUp) {
    const std::size_t fdsBefore = numberOfOpenFileDescriptors();
    TemporaryDirectory dir;
    MockPOSIXInterface posix;
    posix.onBind = [](int, const struct sockaddr*, socklen_t) {
        return -1;
    };
    TestSessionManager sessionManager;
    const std::filesystem::path socketPath = dir.path() / "handoff-mongodb.sock";
    HandoffListenerThread listener(singleSocketParams(socketPath, sessionManager, posix));
    ASSERT_EQ(listener.setup({.s2nConfig = nullptr}).code(), ErrorCodes::SocketException);
    ASSERT_EQ(numberOfOpenFileDescriptors(), fdsBefore);
    ASSERT_FALSE(std::filesystem::exists(socketPath));
}

// chmod() failure must close the fd and unlink the socket file.
TEST(HandoffListenerThreadTest, ChmodFailureCleansUp) {
    const std::size_t fdsBefore = numberOfOpenFileDescriptors();
    TemporaryDirectory dir;
    MockPOSIXInterface posix;
    posix.onChmod = [](const char*, mode_t) {
        return -1;
    };
    TestSessionManager sessionManager;
    const std::filesystem::path socketPath = dir.path() / "handoff-mongodb.sock";
    HandoffListenerThread listener(singleSocketParams(socketPath, sessionManager, posix));
    ASSERT_EQ(listener.setup({.s2nConfig = nullptr}).code(), ErrorCodes::SocketException);
    ASSERT_EQ(numberOfOpenFileDescriptors(), fdsBefore);
    ASSERT_FALSE(std::filesystem::exists(socketPath));
}

// chown() failure must close the fd and unlink the socket file.
TEST(HandoffListenerThreadTest, ChownFailureCleansUp) {
    const std::size_t fdsBefore = numberOfOpenFileDescriptors();
    TemporaryDirectory dir;
    MockPOSIXInterface posix;
    posix.onChown = [](const char*, uid_t, gid_t) {
        return -1;
    };
    TestSessionManager sessionManager;
    const std::filesystem::path socketPath = dir.path() / "handoff-mongodb.sock";
    HandoffListenerThread listener({
        .sockets = {ListeningSocket{
            .path = socketPath, .isLoadBalanced = false, .isPriority = false}},
        .socketGroupID = ::getegid(),
        .listenBacklog = ProcessInfo::getDefaultListenBacklog(),
        .sessionManager = &sessionManager,
        .transportLayer = nullptr,
        .posix = posix,
    });
    ASSERT_EQ(listener.setup({.s2nConfig = nullptr}).code(), ErrorCodes::SocketException);
    ASSERT_EQ(numberOfOpenFileDescriptors(), fdsBefore);
    ASSERT_FALSE(std::filesystem::exists(socketPath));
}

// pipe() failure must close the socket fd and unlink the socket file.
TEST(HandoffListenerThreadTest, PipeFailureCleansUp) {
    const std::size_t fdsBefore = numberOfOpenFileDescriptors();
    TemporaryDirectory dir;
    MockPOSIXInterface posix;
    posix.onPipe = [](int*) {
        return -1;
    };
    TestSessionManager sessionManager;
    const std::filesystem::path socketPath = dir.path() / "handoff-mongodb.sock";
    HandoffListenerThread listener(singleSocketParams(socketPath, sessionManager, posix));
    ASSERT_EQ(listener.setup({.s2nConfig = nullptr}).code(), ErrorCodes::InternalError);
    ASSERT_EQ(numberOfOpenFileDescriptors(), fdsBefore);
    ASSERT_FALSE(std::filesystem::exists(socketPath));
}

// fcntl(F_GETFL) failure on the socket must clean up and leave no fd leak.
TEST(HandoffListenerThreadTest, FcntlGetFlagsFailureCleansUp) {
    const std::size_t fdsBefore = numberOfOpenFileDescriptors();
    TemporaryDirectory dir;
    MockPOSIXInterface posix;
    posix.onFcntl = [](int, int) {
        return -1;
    };
    TestSessionManager sessionManager;
    HandoffListenerThread listener(
        singleSocketParams(dir.path() / "handoff-mongodb.sock", sessionManager, posix));
    ASSERT_EQ(listener.setup({.s2nConfig = nullptr}).code(), ErrorCodes::SocketException);
    ASSERT_EQ(numberOfOpenFileDescriptors(), fdsBefore);
}

// fcntl(F_SETFL, ...) failure on the socket must clean up and leave no fd leak.
TEST(HandoffListenerThreadTest, FcntlSetFlagsFailureCleansUp) {
    const std::size_t fdsBefore = numberOfOpenFileDescriptors();
    TemporaryDirectory dir;
    MockPOSIXInterface posix;
    posix.onFcntl = [](int fd, int cmd) {
        return ::fcntl(fd, cmd);
    };
    posix.onFcntlArg = [](int, int, int) {
        return -1;
    };
    TestSessionManager sessionManager;
    HandoffListenerThread listener(
        singleSocketParams(dir.path() / "handoff-mongodb.sock", sessionManager, posix));
    ASSERT_EQ(listener.setup({.s2nConfig = nullptr}).code(), ErrorCodes::SocketException);
    ASSERT_EQ(numberOfOpenFileDescriptors(), fdsBefore);
}

// Failing fcntl(F_GETFL) on the pipe write-end (the 2nd F_GETFL call, after 1 for the socket)
// must clean up all fds.
TEST(HandoffListenerThreadTest, PipeFcntlGetFlagsFailureCleansUp) {
    const std::size_t fdsBefore = numberOfOpenFileDescriptors();
    TemporaryDirectory dir;
    MockPOSIXInterface posix;
    std::atomic<int> count{0};
    posix.onFcntl = [&count](int fd, int cmd) -> int {
        if (count.fetch_add(1) < 1)
            return ::fcntl(fd, cmd);
        errno = EBADF;
        return -1;
    };
    TestSessionManager sessionManager;
    HandoffListenerThread listener(
        singleSocketParams(dir.path() / "handoff-mongodb.sock", sessionManager, posix));
    ASSERT_EQ(listener.setup({.s2nConfig = nullptr}).code(), ErrorCodes::InternalError);
    ASSERT_EQ(numberOfOpenFileDescriptors(), fdsBefore);
}

// Failing fcntl(F_SETFL, ...) on the pipe write-end (the 2nd F_SETFL call, after 1 for the
// socket) must clean up all fds.
TEST(HandoffListenerThreadTest, PipeFcntlSetFlagsFailureCleansUp) {
    const std::size_t fdsBefore = numberOfOpenFileDescriptors();
    TemporaryDirectory dir;
    MockPOSIXInterface posix;
    std::atomic<int> count{0};
    posix.onFcntlArg = [&count](int fd, int cmd, int arg) -> int {
        if (count.fetch_add(1) < 1)
            return ::fcntl(fd, cmd, arg);
        errno = EBADF;
        return -1;
    };
    TestSessionManager sessionManager;
    HandoffListenerThread listener(
        singleSocketParams(dir.path() / "handoff-mongodb.sock", sessionManager, posix));
    ASSERT_EQ(listener.setup({.s2nConfig = nullptr}).code(), ErrorCodes::InternalError);
    ASSERT_EQ(numberOfOpenFileDescriptors(), fdsBefore);
}

// socket() failure on the second call: the first already-bound socket must be closed and unlinked.
TEST(HandoffListenerThreadTest, SocketCreationFailureCleansUp) {
    const std::size_t fdsBefore = numberOfOpenFileDescriptors();
    TemporaryDirectory dir;
    MockPOSIXInterface posix;
    std::atomic<int> socketCallCount{0};
    posix.onSocket = [&socketCallCount](int domain, int type, int protocol) -> int {
        if (socketCallCount.fetch_add(1) < 1)
            return ::socket(domain, type, protocol);
        errno = EMFILE;
        return -1;
    };
    TestSessionManager sessionManager;
    HandoffListenerThread listener({
        .sockets = {ListeningSocket{.path = dir.path() / "handoff-mongodb.sock",
                                    .isLoadBalanced = false,
                                    .isPriority = false},
                    ListeningSocket{.path = dir.path() / "handoff-mongodb-load-balanced.sock",
                                    .isLoadBalanced = true,
                                    .isPriority = false}},
        .listenBacklog = ProcessInfo::getDefaultListenBacklog(),
        .sessionManager = &sessionManager,
        .transportLayer = nullptr,
        .posix = posix,
    });
    ASSERT_EQ(listener.setup({.s2nConfig = nullptr}).code(), ErrorCodes::SocketException);
    ASSERT_EQ(numberOfOpenFileDescriptors(), fdsBefore);
}

// listen()/start() tests
// ----------------------

// After setup() but before start(), the socket is bound but not listening.
TEST(HandoffListenerThreadTest, CannotConnectBeforeStart) {
    TemporaryDirectory dir;
    POSIXInterface posix;
    TestSessionManager sessionManager;
    const std::filesystem::path socketPath = dir.path() / "handoff-mongodb.sock";
    HandoffListenerThread listener(singleSocketParams(socketPath, sessionManager, posix));
    ASSERT_OK(listener.setup({.s2nConfig = nullptr}));
    ASSERT_EQ(connectUnixSocket(socketPath), -1);
    listener.shutdown();
}

// After start(), we can connect.
TEST(HandoffListenerThreadTest, CanConnectAfterStart) {
    TemporaryDirectory dir;
    POSIXInterface posix;
    TestSessionManager sessionManager;
    const std::filesystem::path socketPath = dir.path() / "handoff-mongodb.sock";
    HandoffListenerThread listener(singleSocketParams(socketPath, sessionManager, posix));
    ASSERT_OK(listener.setup({.s2nConfig = nullptr}));
    ASSERT_OK(listener.listen());
    listener.start();
    const int fd = connectUnixSocket(socketPath);
    ASSERT_NE(fd, -1);
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
    listener.shutdown();
}

// listen() failure means no listener thread is started. shutdown() must still clean up the socket
// files.
TEST(HandoffListenerThreadTest, ListenFailureNoThread) {
    const std::size_t fdsBefore = numberOfOpenFileDescriptors();
    TemporaryDirectory dir;
    MockPOSIXInterface posix;
    TestSessionManager sessionManager;
    posix.onListen = [](int, int) {
        return -1;
    };
    const std::filesystem::path socketPath = dir.path() / "handoff-mongodb.sock";
    HandoffListenerThread listener(singleSocketParams(socketPath, sessionManager, posix));
    ASSERT_OK(listener.setup({.s2nConfig = nullptr}));
    ASSERT_EQ(listener.listen().code(), ErrorCodes::SocketException);
    listener.shutdown();
    ASSERT_EQ(numberOfOpenFileDescriptors(), fdsBefore);
    ASSERT_FALSE(std::filesystem::exists(socketPath));
}

// Listener loop resilience tests
// ------------------------------

// poll() returning EINTR must cause a silent retry; a subsequent connection must be accepted.
TEST(HandoffListenerThreadTest, PollInterruptedBySignalContinuesListening) {
    TemporaryDirectory dir;
    MockPOSIXInterface posix;
    TestSessionManager sessionManager;
    std::atomic<int> pollCount{0};
    posix.onPoll = [&pollCount](struct pollfd* fds, nfds_t n, int t) -> int {
        if (pollCount.fetch_add(1) == 0) {
            errno = EINTR;
            return -1;
        }
        return ::poll(fds, n, t);
    };
    const std::filesystem::path socketPath = dir.path() / "handoff-mongodb.sock";
    HandoffListenerThread listener(singleSocketParams(socketPath, sessionManager, posix));
    ASSERT_OK(listener.setup({.s2nConfig = nullptr}));
    ASSERT_OK(listener.listen());
    listener.start();

    auto session = connectAndGetSession(socketPath, sessionManager);
    ASSERT_NE(session, nullptr);
    session->end();

    listener.shutdown();
}

// poll() returning a non-EINTR error must cause a logged retry; connections must still be accepted.
TEST(HandoffListenerThreadTest, PollTransientFailureContinuesListening) {
    TemporaryDirectory dir;
    MockPOSIXInterface posix;
    TestSessionManager sessionManager;
    std::atomic<int> pollCount{0};
    posix.onPoll = [&pollCount](struct pollfd* fds, nfds_t n, int t) -> int {
        if (pollCount.fetch_add(1) == 0) {
            errno = EAGAIN;
            return -1;
        }
        return ::poll(fds, n, t);
    };
    const std::filesystem::path socketPath = dir.path() / "handoff-mongodb.sock";
    HandoffListenerThread listener(singleSocketParams(socketPath, sessionManager, posix));
    ASSERT_OK(listener.setup({.s2nConfig = nullptr}));
    ASSERT_OK(listener.listen());
    listener.start();

    auto session = connectAndGetSession(socketPath, sessionManager);
    ASSERT_NE(session, nullptr);
    session->end();

    listener.shutdown();
}

// accept() returning EINTR must retry via the poll loop; the pending connection must be accepted.
// Similarly, EAGAIN, EWOULDBLOCK, or ECONNABORTED mean the connection that was there went away
// before we got a chance to call accept(). Realistically that won't happen for unix domain sockets,
// but we allow it and treat it the same as EINTR.
TEST(HandoffListenerThreadTest, AcceptTransientFailureContinuesListening) {
    // EAGAIN and EWOULDBLOCK might be the same value on some platforms. Use a set so that we
    // consider distinct values only.
    for (const int error : std::set<int>{EINTR, EAGAIN, EWOULDBLOCK, ECONNABORTED}) {
        TemporaryDirectory dir;
        MockPOSIXInterface posix;
        TestSessionManager sessionManager;
        std::atomic<int> acceptCount{0};
        posix.onAccept = [&](int s, struct sockaddr* a, socklen_t* l) -> int {
            if (acceptCount.fetch_add(1) == 0) {
                errno = error;
                return -1;
            }
            return ::accept(s, a, l);
        };
        const std::filesystem::path socketPath = dir.path() / "handoff-mongodb.sock";
        HandoffListenerThread listener(singleSocketParams(socketPath, sessionManager, posix));
        ASSERT_OK(listener.setup({.s2nConfig = nullptr}))
            << "for injected errno: " << std::strerror(error);
        ASSERT_OK(listener.listen()) << "for injected errno: " << std::strerror(error);
        listener.start();

        auto session = connectAndGetSession(socketPath, sessionManager);
        ASSERT_NE(session, nullptr) << "for injected errno: " << std::strerror(error);
        session->end();

        listener.shutdown();
    }
}

// accept() returning a non-retryable error (EMFILE) must log and continue; the connection stays
// queued and is accepted on the next poll/accept cycle.
TEST(HandoffListenerThreadTest, AcceptNonRetryableFailureContinuesListening) {
    TemporaryDirectory dir;
    MockPOSIXInterface posix;
    TestSessionManager sessionManager;
    std::atomic<int> acceptCount{0};
    posix.onAccept = [&acceptCount](int s, struct sockaddr* a, socklen_t* l) -> int {
        if (acceptCount.fetch_add(1) == 0) {
            errno = EMFILE;
            return -1;
        }
        return ::accept(s, a, l);
    };
    const std::filesystem::path socketPath = dir.path() / "handoff-mongodb.sock";
    HandoffListenerThread listener(singleSocketParams(socketPath, sessionManager, posix));
    ASSERT_OK(listener.setup({.s2nConfig = nullptr}));
    ASSERT_OK(listener.listen());
    listener.start();

    auto session = connectAndGetSession(socketPath, sessionManager);
    ASSERT_NE(session, nullptr);
    session->end();

    listener.shutdown();
}

// Session predicate tests
// -----------------------

// Sessions created on each socket have the correct load-balancer/priority/proxy flags.
TEST(HandoffListenerThreadTest, SessionFlagsPerSocket) {
    TemporaryDirectory dir;
    POSIXInterface posix;
    TestSessionManager sessionManager;
    const std::filesystem::path standardPath = dir.path() / "handoff-mongodb.sock";
    const std::filesystem::path loadBalancedPath =
        dir.path() / "handoff-mongodb-load-balanced.sock";
    const std::filesystem::path priorityPath = dir.path() / "handoff-mongodb-priority.sock";
    HandoffListenerThread listener({
        .sockets =
            {ListeningSocket{.path = standardPath, .isLoadBalanced = false, .isPriority = false},
             ListeningSocket{.path = loadBalancedPath, .isLoadBalanced = true, .isPriority = false},
             ListeningSocket{.path = priorityPath, .isLoadBalanced = false, .isPriority = true}},
        .listenBacklog = ProcessInfo::getDefaultListenBacklog(),
        .sessionManager = &sessionManager,
        .transportLayer = nullptr,
        .posix = posix,
    });
    ASSERT_OK(listener.setup({.s2nConfig = nullptr}));
    ASSERT_OK(listener.listen());
    listener.start();

    {
        auto session = connectAndGetSession(standardPath, sessionManager);
        ASSERT_FALSE(session->isConnectedToLoadBalancerPort());
        ASSERT_FALSE(session->isConnectedToPriorityPort());
        ASSERT_TRUE(session->isConnectedToProxyUnixSocket());
        session->end();
    }
    {
        auto session = connectAndGetSession(loadBalancedPath, sessionManager);
        ASSERT_TRUE(session->isConnectedToLoadBalancerPort());
        ASSERT_FALSE(session->isConnectedToPriorityPort());
        ASSERT_TRUE(session->isConnectedToProxyUnixSocket());
        session->end();
    }
    {
        auto session = connectAndGetSession(priorityPath, sessionManager);
        ASSERT_FALSE(session->isConnectedToLoadBalancerPort());
        ASSERT_TRUE(session->isConnectedToPriorityPort());
        ASSERT_TRUE(session->isConnectedToProxyUnixSocket());
        session->end();
    }

    listener.shutdown();
}

// session->local() reflects the path of the socket the connection arrived on.
TEST(HandoffListenerThreadTest, SessionLocalAddress) {
    TemporaryDirectory dir;
    POSIXInterface posix;
    TestSessionManager sessionManager;
    const std::filesystem::path standardPath = dir.path() / "handoff-mongodb.sock";
    const std::filesystem::path loadBalancedPath =
        dir.path() / "handoff-mongodb-load-balanced.sock";
    const std::filesystem::path priorityPath = dir.path() / "handoff-mongodb-priority.sock";
    HandoffListenerThread listener({
        .sockets =
            {ListeningSocket{.path = standardPath, .isLoadBalanced = false, .isPriority = false},
             ListeningSocket{.path = loadBalancedPath, .isLoadBalanced = true, .isPriority = false},
             ListeningSocket{.path = priorityPath, .isLoadBalanced = false, .isPriority = true}},
        .listenBacklog = ProcessInfo::getDefaultListenBacklog(),
        .sessionManager = &sessionManager,
        .transportLayer = nullptr,
        .posix = posix,
    });
    ASSERT_OK(listener.setup({.s2nConfig = nullptr}));
    ASSERT_OK(listener.listen());
    listener.start();

    for (const auto& path : {standardPath, loadBalancedPath, priorityPath}) {
        auto session = connectAndGetSession(path, sessionManager);
        ASSERT_EQ(session->local().host(), path.string());
        session->end();
    }

    listener.shutdown();
}

// session->remote() reports the placeholder "host" for unix domain clients.
TEST(HandoffListenerThreadTest, SessionRemoteAddress) {
    TemporaryDirectory dir;
    POSIXInterface posix;
    TestSessionManager sessionManager;
    const std::filesystem::path socketPath = dir.path() / "handoff-mongodb.sock";
    HandoffListenerThread listener(singleSocketParams(socketPath, sessionManager, posix));
    ASSERT_OK(listener.setup({.s2nConfig = nullptr}));
    ASSERT_OK(listener.listen());
    listener.start();

    auto session = connectAndGetSession(socketPath, sessionManager);
    ASSERT_EQ(session->remote().host(), "anonymous unix socket");
    session->end();

    listener.shutdown();
}

// Socket owner group ID validation tests
// --------------------------------------

// When .socketGroupID is not specified, the listener does not call chown on its socket files,
// because they are already owned by the group ID of the current process, which is what
// `.socketGroupID == nullopt` indicates. process.
TEST(HandoffListenerThreadTest, ImplicitGroupOwnershipOfListenerSockets) {
    TemporaryDirectory dir;
    MockPOSIXInterface posix;
    const std::filesystem::path socketPath = dir.path() / "handoff-mongodb.sock";
    std::atomic<int> chownCallCount = 0;
    posix.onChown = [&](const char* /*path*/, uid_t, gid_t /*groupID*/) {
        chownCallCount.fetch_add(1);
        return 0;
    };

    TestSessionManager sessionManager;
    // The fact that we do not specify `.socketGroupID` (that it takes its default nullopt value) is
    // the important thing here.
    HandoffListenerThread listener({
        .sockets = {ListeningSocket{
            .path = socketPath, .isLoadBalanced = false, .isPriority = false}},
        .listenBacklog = ProcessInfo::getDefaultListenBacklog(),
        .sessionManager = &sessionManager,
        .transportLayer = nullptr,
        .posix = posix,
    });

    ASSERT_OK(listener.setup({.s2nConfig = nullptr}));
    ASSERT_EQ(chownCallCount.load(), 0);

    listener.shutdown();
}

// .socketGroupID, when specified, is passed to chown to set the owner group of the created unix
// domain socket(s).
TEST(HandoffListenerThreadTest, ExplicitGroupOwnershipOfListenerSockets) {
    TemporaryDirectory dir;
    MockPOSIXInterface posix;
    const std::filesystem::path socketPath = dir.path() / "handoff-mongodb.sock";
    std::atomic<gid_t> resultingGID = -1;
    posix.onChown = [&](const char* path, uid_t, gid_t groupID) {
        ASSERT_EQ(socketPath.string(), path);
        resultingGID.store(groupID);
        return 0;
    };

    TestSessionManager sessionManager;
    const gid_t expectedGID = 1337;
    // `.socketGroupID` is the important parameter here.
    HandoffListenerThread listener({
        .sockets = {ListeningSocket{
            .path = socketPath, .isLoadBalanced = false, .isPriority = false}},
        .socketGroupID = expectedGID,
        .listenBacklog = ProcessInfo::getDefaultListenBacklog(),
        .sessionManager = &sessionManager,
        .transportLayer = nullptr,
        .posix = posix,
    });

    ASSERT_OK(listener.setup({.s2nConfig = nullptr}));
    ASSERT_EQ(resultingGID.load(), expectedGID);

    listener.shutdown();
}

// When getpeereid() returns a mismatched GID, the connection is closed and no session is created.
TEST(HandoffListenerThreadTest, WrongGidImplicitConnectionClosed) {
    const std::size_t fdsBefore = numberOfOpenFileDescriptors();
    TemporaryDirectory dir;
    MockPOSIXInterface posix;
    posix.onGetpeereid = [](int, uid_t*, gid_t* gid) {
        *gid = ::getegid() + 1;
        return 0;
    };
    TestSessionManager sessionManager;
    const std::filesystem::path socketPath = dir.path() / "handoff-mongodb.sock";
    HandoffListenerThread listener(singleSocketParams(socketPath, sessionManager, posix));
    ASSERT_OK(listener.setup({.s2nConfig = nullptr}));

    // The created socket will be owned by the same group as the current process.
    struct stat st;
    ASSERT_EQ(::stat(socketPath.c_str(), &st), 0) << errorMessage(lastSystemError());
    ASSERT_EQ(st.st_gid, ::getegid());

    ASSERT_OK(listener.listen());
    listener.start();

    // When the listener thread closes the accepted connection, the client will get EOF.
    int clientFd = connectUnixSocket(socketPath);
    ASSERT_NE(clientFd, -1);
    ASSERT_TRUE(waitForEOF(clientFd));
    ::close(clientFd);

    listener.shutdown();
    ASSERT_EQ(sessionManager.getNumSessionsStarted(), 0);
    ASSERT_EQ(numberOfOpenFileDescriptors(), fdsBefore);
}

// When getpeereid() returns a mismatched GID, the connection is closed and no session is created.
TEST(HandoffListenerThreadTest, WrongGidExplicitConnectionClosed) {
    const std::size_t fdsBefore = numberOfOpenFileDescriptors();
    TemporaryDirectory dir;
    MockPOSIXInterface posix;
    const gid_t groupID = 1337;
    posix.onGetpeereid = [&](int, uid_t*, gid_t* gid) {
        *gid = groupID + 1;
        return 0;
    };
    posix.onChown = [](const char*, uid_t, gid_t) {
        // Don't actually change the owner (1337 is bogus), but report success.
        return 0;
    };
    TestSessionManager sessionManager;
    const std::filesystem::path socketPath = dir.path() / "handoff-mongodb.sock";
    // `.socketGroupID` is the important parameter here.
    HandoffListenerThread listener({
        .sockets = {ListeningSocket{
            .path = socketPath, .isLoadBalanced = false, .isPriority = false}},
        .socketGroupID = groupID,
        .listenBacklog = ProcessInfo::getDefaultListenBacklog(),
        .sessionManager = &sessionManager,
        .transportLayer = nullptr,
        .posix = posix,
    });
    ASSERT_OK(listener.setup({.s2nConfig = nullptr}));
    ASSERT_OK(listener.listen());
    listener.start();

    // When the listener thread closes the accepted connection, the client will get EOF.
    int clientFd = connectUnixSocket(socketPath);
    ASSERT_NE(clientFd, -1);
    ASSERT_TRUE(waitForEOF(clientFd));
    ::close(clientFd);

    listener.shutdown();
    ASSERT_EQ(sessionManager.getNumSessionsStarted(), 0);
    ASSERT_EQ(numberOfOpenFileDescriptors(), fdsBefore);
}

// When getpeereid() fails, the connection is closed and no session is created.
TEST(HandoffListenerThreadTest, GetpeereidFailureConnectionClosed) {
    const std::size_t fdsBefore = numberOfOpenFileDescriptors();
    TemporaryDirectory dir;
    MockPOSIXInterface posix;
    posix.onGetpeereid = [](int, uid_t*, gid_t*) {
        errno = ENOTSUP;
        return -1;
    };
    TestSessionManager sessionManager;
    const std::filesystem::path socketPath = dir.path() / "handoff-mongodb.sock";
    HandoffListenerThread listener(singleSocketParams(socketPath, sessionManager, posix));
    ASSERT_OK(listener.setup({.s2nConfig = nullptr}));
    ASSERT_OK(listener.listen());
    listener.start();

    // When the listener thread closes the accepted connection, the client will get EOF.
    int clientFd = connectUnixSocket(socketPath);
    ASSERT_NE(clientFd, -1);
    ASSERT_TRUE(waitForEOF(clientFd));
    ::close(clientFd);

    listener.shutdown();
    ASSERT_EQ(sessionManager.getNumSessionsStarted(), 0);
    ASSERT_EQ(numberOfOpenFileDescriptors(), fdsBefore);
}

// stopAcceptingSessions() and shutdown() tests
// --------------------------------------------

// This is the shared implementation between StopAcceptingSessionsStopsAcceptingSessions and
// ShutdownStopsAcceptingSessions.
// `afterWhat` is a function that calls either `stopAcceptingSessions` or `shutdown` on its
// argument.
// The test then verifies that we are unable to establish new connections afterward.
void verifyCannotConnectAfter(const std::function<void(HandoffListenerThread&)>& afterWhat) {
    TemporaryDirectory dir;
    POSIXInterface posix;
    TestSessionManager sessionManager;
    const std::filesystem::path socketPath = dir.path() / "handoff-mongodb.sock";
    HandoffListenerThread listener(singleSocketParams(socketPath, sessionManager, posix));
    ASSERT_OK(listener.setup({.s2nConfig = nullptr}));
    ASSERT_OK(listener.listen());
    listener.start();

    // Verify that we can connect _before_ `afterWhat`.
    int fd = connectUnixSocket(socketPath);
    ASSERT_NE(fd, -1);
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);

    afterWhat(listener);
    // Verify that we can't connect _after_ `afterWhat`.
    fd = connectUnixSocket(socketPath);
    ASSERT_EQ(fd, -1);

    listener.shutdown();
}

// stopAcceptingSessions() stops accepting sessions.
TEST(HandoffListenerThread, StopAcceptingSessionsStopsAcceptingSessions) {
    verifyCannotConnectAfter(
        [](HandoffListenerThread& listener) { listener.stopAcceptingSessions(); });
}

// shutdown() stops accepting sessions.
TEST(HandoffListenerThreadTest, ShutdownStopsAcceptingSessions) {
    verifyCannotConnectAfter([](HandoffListenerThread& listener) { listener.shutdown(); });
}

// If write() to the wake pipe is interrupted by EINTR, it must be retried so the listener wakes up.
TEST(HandoffListenerThreadTest, ShutdownWriteInterruptedBySignalWakesListener) {
    TemporaryDirectory dir;
    MockPOSIXInterface posix;
    TestSessionManager sessionManager;
    std::atomic<int> writeCount{0};
    posix.onWrite = [&writeCount](int fd, const void* buf, size_t n) -> ssize_t {
        if (writeCount.fetch_add(1) == 0) {
            errno = EINTR;
            return -1;
        }
        return ::write(fd, buf, n);
    };
    const std::filesystem::path socketPath = dir.path() / "handoff-mongodb.sock";
    HandoffListenerThread listener(singleSocketParams(socketPath, sessionManager, posix));
    ASSERT_OK(listener.setup({.s2nConfig = nullptr}));
    ASSERT_OK(listener.listen());
    listener.start();
    listener.stopAcceptingSessions();
    // Interrupted the first time, succeeded the second time.
    ASSERT_EQ(writeCount.load(), 2);
    listener.shutdown();
}

// accept() failure backoff tests
// ------------------------------

// errno values for which the listener thread should back off rather than
// treat the failure as transient.
class AcceptBackoffTest : public testing::TestWithParam<int> {
protected:
    AcceptBackoffTest();

    void setupBackoff();

    TemporaryDirectory dir;
    MockPOSIXInterface posix;
    TestSessionManager sessionManager;
    std::function<int(pollfd* fds, nfds_t count, int timeoutMillis)> onPoll;
    std::barrier<> onPollBegin;
    std::barrier<> onPollEnd;
    std::function<int(int socket, sockaddr* address, socklen_t* length)> onAccept;
    std::barrier<> onAcceptBegin;
    HandoffListenerThread listener;
};

AcceptBackoffTest::AcceptBackoffTest()
    : onPollBegin(2),
      onPollEnd(2),
      onAcceptBegin(2),
      listener(singleSocketParams(dir.path() / "handoff-mongodb.sock", sessionManager, posix)) {
    posix.onPoll = [&](pollfd* fds, nfds_t count, int timeoutMillis) {
        onPollBegin.arrive_and_wait();
        std::for_each_n(fds, count, [](pollfd& interest) { interest.revents = 0; });
        return onPoll(fds, count, timeoutMillis);
    };

    posix.onAccept = [&](int socket, sockaddr* address, socklen_t* length) {
        onAcceptBegin.arrive_and_wait();
        return onAccept(socket, address, length);
    };
}

void AcceptBackoffTest::setupBackoff() {
    ASSERT_OK(listener.setup({.s2nConfig = nullptr}));
    ASSERT_OK(listener.listen());
    listener.start();

    // poll succeeds, but then accept() fails
    onPoll = [&](pollfd* fds, nfds_t count, int timeoutMillis) {
        // `fds` has two elements: The one listening socket, and the pipe.
        ASSERT_EQ(count, 2);
        // There's no timeout, because we're not backing off yet.
        ASSERT_EQ(timeoutMillis, -1);
        // Pretend that the listener has a connection to accept.
        fds[0].revents = POLLIN;
        return 1;
    };
    onPollBegin.arrive_and_wait();

    onAccept = [&](int, sockaddr*, socklen_t*) {
        // Return an error for which the listener thread will back off.
        errno = GetParam();
        return -1;
    };
    onAcceptBegin.arrive_and_wait();
}

std::string errorName(int errorCode) {
    switch (errorCode) {
        case EMFILE:
            return "EMFILE";
        case ENFILE:
            return "ENFILE";
        case ENOBUFS:
            return "ENOBUFS";
        case ENOMEM:
            return "ENOMEM";
        case EINVAL:
            return "EINVAL";
        case ENOTSOCK:
            return "ENOTSOCK";
        case EOPNOTSUPP:
            return "EOPNOTSUPP";
        default:
            MONGO_UNREACHABLE;
    }
}

INSTANTIATE_TEST_SUITE_P(
    HandoffListenerThread,
    AcceptBackoffTest,
    testing::Values(EMFILE, ENFILE, ENOBUFS, ENOMEM, EINVAL, ENOTSOCK, EOPNOTSUPP),
    [](const testing::TestParamInfo<int>& info) {
        // Readable test names, e.g. AcceptFailuresThatTriggerBackoff/EMFILE
        return errorName(info.param);
    });

// Certain errors reported by accept() cause the listener thread to stop listening for a time, while
// still handling shutdown events through the pipe.
TEST_P(AcceptBackoffTest, SomeAcceptFailuresTriggerBackoff) {
    setupBackoff();

    // The listener thread will reenter poll(), but this time with a timeout and without the
    // listener socket.
    onPoll = [&](pollfd* fds, nfds_t count, int timeoutMillis) {
        ASSERT_EQ(count, 1);
        ASSERT_NE(timeoutMillis, -1);
        // Pretend that there's data available on the shutdown pipe, so the listener thread
        // shuts down. We'll also shut it down below, but we don't want it to block in accept().
        fds[0].revents = POLLIN;
        return 1;
    };
    onPollBegin.arrive_and_wait();

    listener.shutdown();
}

// If a backoff times out (which is expected unless we shut down), then the listener thread reenters
// poll() to wait for connections again.
TEST_P(AcceptBackoffTest, RetriesAfterBackoff) {
    setupBackoff();

    // The listener thread will reenter poll(), but this time with a timeout and without the
    // listener socket.
    // If we pretend to have timed out, the listener thread will reenter poll() with the listening
    // socket and no timeout again (retry after backoff).
    onPoll = [&](pollfd* fds, nfds_t count, int timeoutMillis) {
        ASSERT_EQ(count, 1);
        ASSERT_NE(timeoutMillis, -1);
        (void)onPollEnd.arrive();
        return 0;  // timeout
    };
    onPollBegin.arrive_and_wait();
    onPollEnd.arrive_and_wait();

    onPoll = [&](pollfd* fds, nfds_t count, int timeoutMillis) {
        ASSERT_EQ(count, 2);
        ASSERT_EQ(timeoutMillis, -1);
        // Pretend we're shutting down.
        fds[1].revents = POLLIN;
        return 1;
    };
    onPollBegin.arrive_and_wait();

    listener.shutdown();
}

// When the listener thread has backed off, and then reenters poll(), and later calls accept(), if
// no accept() fails that time around the event loop, the listener thread then does not reenter
// backoff, i.e. lack of failure to accept cancels backoff.
TEST_P(AcceptBackoffTest, SuccessfulAcceptCancelsBackoff) {
    setupBackoff();

    // The listener thread will reenter poll(), but this time with a timeout and without the
    // listener socket.
    // If we pretend to have timed out, the listener thread will reenter poll() with the listening
    // socket and no timeout again (retry after backoff).

    onPoll = [&](pollfd* fds, nfds_t count, int timeoutMillis) {
        ASSERT_EQ(count, 1);
        ASSERT_NE(timeoutMillis, -1);
        (void)onPollEnd.arrive();
        return 0;  // timeout
    };
    onPollBegin.arrive_and_wait();
    onPollEnd.arrive_and_wait();

    // Say that a connection is available. Then return success from accept().
    // The listener thread will create a session and return to poll without backoff.
    onPoll = [&](pollfd* fds, nfds_t count, int timeoutMillis) {
        ASSERT_EQ(count, 2);
        ASSERT_EQ(timeoutMillis, -1);
        fds[0].revents = POLLIN;
        return 1;
    };
    onPollBegin.arrive_and_wait();

    // Give the listener thread a real socket, not that we intend to use it.
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    onAccept = [&](int, sockaddr*, socklen_t*) {
        return fds[0];
    };
    onAcceptBegin.arrive_and_wait();

    const std::shared_ptr<Session> session = sessionManager.popSession();
    ASSERT_NE(session, nullptr);
    session->end();
    ::close(fds[1]);

    // Listener reenters poll without backoff.
    onPoll = [&](pollfd* fds, nfds_t count, int timeoutMillis) {
        ASSERT_EQ(count, 2);
        ASSERT_EQ(timeoutMillis, -1);
        // Pretend we're shutting down.
        fds[1].revents = POLLIN;
        return 1;
    };
    onPollBegin.arrive_and_wait();

    listener.shutdown();
}

// Repeated failures cause the listener thread to back off for longer and longer times: powers of
// four milliseconds from 1 ms up to at most 1000 milliseconds.
TEST_P(AcceptBackoffTest, BackoffTimeoutSequence) {
    setupBackoff();

    for (const int expectedMillis : {1, 4, 16, 64, 256, 1000, 1000}) {
        onPoll = [&](pollfd* fds, nfds_t count, int timeoutMillis) {
            ASSERT_EQ(count, 1);
            ASSERT_EQ(timeoutMillis, expectedMillis);
            (void)onPollEnd.arrive();
            return 0;  // timeout
        };
        onPollBegin.arrive_and_wait();
        onPollEnd.arrive_and_wait();

        // Say that a connection is available. Then return failure from accept().
        // The listener thread will go back into poll() with an increased backoff timeout.
        onPoll = [&](pollfd* fds, nfds_t count, int timeoutMillis) {
            ASSERT_EQ(count, 2);
            ASSERT_EQ(timeoutMillis, -1);
            fds[0].revents = POLLIN;
            return 1;
        };
        onPollBegin.arrive_and_wait();

        onAccept = [&](int, sockaddr*, socklen_t*) {
            // Return an error for which the listener thread will back off.
            errno = GetParam();
            return -1;
        };
        onAcceptBegin.arrive_and_wait();
    }

    // Say that the pipe is ready in poll(). The listener thread will shut down.
    onPoll = [&](pollfd* fds, nfds_t count, int timeoutMillis) {
        ASSERT_EQ(count, 1);
        ASSERT_NE(timeoutMillis, -1);
        fds[0].revents = POLLIN;
        return 1;
    };
    onPollBegin.arrive_and_wait();
    listener.shutdown();
}

// If the listener thread was backing off and then accept() succeeded, the backoff timeout is reset.
// The next time backoff occurs, the timeout begins again at 1 ms.
TEST_P(AcceptBackoffTest, SuccessfulAcceptResetsBackoffTimeout) {
    setupBackoff();

    onPoll = [&](pollfd* fds, nfds_t count, int timeoutMillis) {
        ASSERT_EQ(count, 1);
        ASSERT_NE(timeoutMillis, -1);
        (void)onPollEnd.arrive();
        return 0;  // timeout
    };
    onPollBegin.arrive_and_wait();
    onPollEnd.arrive_and_wait();

    // Say that a connection is available. Then return success from accept().
    // The listener thread will create a session and return to poll without backoff.
    onPoll = [&](pollfd* fds, nfds_t count, int timeoutMillis) {
        ASSERT_EQ(count, 2);
        ASSERT_EQ(timeoutMillis, -1);
        fds[0].revents = POLLIN;
        return 1;
    };
    onPollBegin.arrive_and_wait();

    // Give the listener thread a real socket, not that we intend to use it.
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    onAccept = [&](int, sockaddr*, socklen_t*) {
        return fds[0];
    };
    onAcceptBegin.arrive_and_wait();

    const std::shared_ptr<Session> session = sessionManager.popSession();
    ASSERT_NE(session, nullptr);
    session->end();
    ::close(fds[1]);

    // Listener reenters poll without backoff.
    onPoll = [&](pollfd* fds, nfds_t count, int timeoutMillis) {
        ASSERT_EQ(count, 2);
        ASSERT_EQ(timeoutMillis, -1);
        // Pretend that another connection is ready, but this time accept() will fail.
        fds[0].revents = POLLIN;
        return 1;
    };
    onPollBegin.arrive_and_wait();

    onAccept = [&](int, sockaddr*, socklen_t*) {
        // Return an error for which the listener thread will back off.
        errno = GetParam();
        return -1;
    };
    onAcceptBegin.arrive_and_wait();

    // The listener thread does a round of backoff, but the timeout has not increased from last
    // time: it's reset to one millisecond.
    onPoll = [&](pollfd* fds, nfds_t count, int timeoutMillis) {
        ASSERT_EQ(count, 1);
        ASSERT_EQ(timeoutMillis, 1);
        // Pretend that the pipe is ready so that the listener thread shuts down.
        fds[0].revents = POLLIN;
        return 1;
    };
    onPollBegin.arrive_and_wait();

    listener.shutdown();
}

}  // namespace
}  // namespace mongo::transport
