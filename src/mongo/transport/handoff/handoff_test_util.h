// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/transport/handoff/handoff_posix_interface.h"
#include "mongo/transport/session_manager.h"
#include "mongo/unittest/unittest.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <queue>
#include <system_error>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

namespace mongo::transport {

/**
 * Alternative to `unittest::TempDir` that creates shorter temporary file names. Unix domain
 * sockets have a rather small path length limit.
 */
class TemporaryDirectory {
    std::filesystem::path _path;

public:
    const std::filesystem::path& path() const {
        return _path;
    }

    explicit TemporaryDirectory(std::string pathPattern = "/tmp/HandoffTest-XXXXXX") {
        if (::mkdtemp(pathPattern.data()) == nullptr) {
            throw std::system_error(errno, std::system_category());
        }
        // `mkdtemp` modified `pathPattern` in place.
        _path = std::move(pathPattern);
    }

    ~TemporaryDirectory() {
        std::filesystem::remove_all(_path);
    }
};

/**
 * Overrides some of `POSIXInterface`'s member functions (e.g. `chmod`) with callbacks that can be
 * installed in unit tests to simulate failures.
 */
class MockPOSIXInterface : public POSIXInterface {
public:
    std::function<int(int, struct sockaddr*, socklen_t*)> onAccept;
    std::function<int(int, const struct sockaddr*, socklen_t)> onBind;
    std::function<int(const char*, mode_t)> onChmod;
    std::function<int(const char*, uid_t, gid_t)> onChown;
    std::function<int(int, int)> onFcntl;
    std::function<int(int, int, int)> onFcntlArg;
    std::function<int(int, uid_t*, gid_t*)> onGetpeereid;
    std::function<int(int, int)> onListen;
    std::function<int(int*)> onPipe;
    std::function<int(struct pollfd*, nfds_t, int)> onPoll;
    std::function<int(int, int, int)> onSocket;
    std::function<ssize_t(int, const void*, size_t)> onWrite;

    int accept(int s, struct sockaddr* a, socklen_t* l) override {
        return onAccept ? onAccept(s, a, l) : POSIXInterface::accept(s, a, l);
    }
    int bind(int s, const struct sockaddr* a, socklen_t l) override {
        return onBind ? onBind(s, a, l) : POSIXInterface::bind(s, a, l);
    }
    int chmod(const char* p, mode_t m) override {
        return onChmod ? onChmod(p, m) : POSIXInterface::chmod(p, m);
    }
    int chown(const char* p, uid_t u, gid_t g) override {
        return onChown ? onChown(p, u, g) : POSIXInterface::chown(p, u, g);
    }
    int fcntl(int fd, int cmd) override {
        return onFcntl ? onFcntl(fd, cmd) : POSIXInterface::fcntl(fd, cmd);
    }
    int fcntl(int fd, int cmd, int arg) override {
        return onFcntlArg ? onFcntlArg(fd, cmd, arg) : POSIXInterface::fcntl(fd, cmd, arg);
    }
    int getpeereid(int s, uid_t* u, gid_t* g) override {
        return onGetpeereid ? onGetpeereid(s, u, g) : POSIXInterface::getpeereid(s, u, g);
    }
    int listen(int s, int b) override {
        return onListen ? onListen(s, b) : POSIXInterface::listen(s, b);
    }
    int pipe(int fd[2]) override {
        return onPipe ? onPipe(fd) : POSIXInterface::pipe(fd);
    }
    int poll(struct pollfd* fds, nfds_t n, int t) override {
        return onPoll ? onPoll(fds, n, t) : POSIXInterface::poll(fds, n, t);
    }
    int socket(int domain, int type, int protocol) override {
        return onSocket ? onSocket(domain, type, protocol)
                        : POSIXInterface::socket(domain, type, protocol);
    }
    ssize_t write(int fd, const void* buf, size_t n) override {
        return onWrite ? onWrite(fd, buf, n) : POSIXInterface::write(fd, buf, n);
    }
};

/**
 * Implementation of `SessionManager` that allows tests to observe the `Session` objects that are
 * created by a transport layer or listener thread.
 * Orchestrates synchronization between the accepting thread and the test driver.
 */
class TestSessionManager : public SessionManager {
public:
    struct Params {
        /** Value returned by shutdown(): true -> success, false -> timeout */
        bool shutdownResult = true;
    };

    TestSessionManager() : TestSessionManager(Params{}) {}

    explicit TestSessionManager(Params params) : _shutdownResult(params.shutdownResult) {}

    void startSession(std::shared_ptr<Session> session) override {
        {
            std::lock_guard lk(_mu);
            _sessions.push(std::move(session));
            ++_numSessionsStarted;
        }
        _cv.notify_one();
    }

    // Returns the next session, waiting up to `timeout`. Returns nullptr on timeout.
    std::shared_ptr<Session> popSession(std::chrono::milliseconds timeout = std::chrono::seconds{
                                            5}) {
        std::unique_lock lk(_mu);
        if (!_cv.wait_for(lk, timeout, [&] { return !_sessions.empty(); }))
            return nullptr;
        auto session = std::move(_sessions.front());
        _sessions.pop();
        return session;
    }

    int getNumSessionsStarted() {
        std::lock_guard lk(_mu);
        return _numSessionsStarted;
    }

    bool shutdown(Milliseconds) override {
        return _shutdownResult;
    }

    /** unused stubs */
    void endSessionByClient(Client*) override {}
    void endAllSessions(Client::TagMask) override {}
    std::size_t numOpenSessions() const override {
        return 0;
    }
    std::vector<std::pair<SessionId, std::string>> getOpenSessionIDs() const override {
        return {};
    }
    void onLoadBalancerPeerSet(bool) override {}

private:
    bool _shutdownResult;
    int _numSessionsStarted = 0;
    std::mutex _mu;
    std::condition_variable _cv;
    std::queue<std::shared_ptr<Session>> _sessions;
};

/** Returns the number of file descriptors currently open in this process. */
inline std::size_t numberOfOpenFileDescriptors() {
    namespace fs = std::filesystem;
    return std::distance(fs::directory_iterator(fs::path("/dev/fd")), fs::directory_iterator{});
}

/** Returns a file descriptor connected to the given unix domain socket, or -1 on failure. */
inline int connectUnixSocket(const std::filesystem::path& path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1)
        return -1;
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == -1) {
        ::close(fd);
        return -1;
    }
    return fd;
}

/**
 * Waits for up to 5 seconds for the remote end of `fd` to close or shutdown. Returns true on
 * success or false on timeout.
 * This is used to wait for the condition "the listener thread closed the client connection."
 */
inline bool waitForEOF(int fd) {
    struct pollfd pfd{fd, POLLIN, 0};
    if (::poll(&pfd, 1, 5000) <= 0)
        return false;
    char buf[1];
    return ::read(fd, buf, sizeof(buf)) == 0;
}

// Connect to a socket, wait for the transport layer to hand the session to the session manager,
// close the client fd, and return the session.
inline std::shared_ptr<Session> connectAndGetSession(const std::filesystem::path& socketPath,
                                                     TestSessionManager& sessionManager) {
    int clientFd = connectUnixSocket(socketPath);
    ASSERT_NE(clientFd, -1);
    auto session = sessionManager.popSession();
    ASSERT_NE(session, nullptr);
    ::close(clientFd);
    return session;
}

}  // namespace mongo::transport
