// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/rpc/message.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/transport/handoff/handoff_posix_interface.h"
#include "mongo/transport/session_manager.h"
#include "mongo/unittest/unittest.h"

#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <mutex>
#include <queue>
#include <span>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <s2n.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
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
    std::function<int(int, int, int, const void*, socklen_t)> onSetsockopt;
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
    int setsockopt(int socket,
                   int level,
                   int option_name,
                   const void* option_value,
                   socklen_t option_len) override {
        return onSetsockopt
            ? onSetsockopt(socket, level, option_name, option_value, option_len)
            : POSIXInterface::setsockopt(socket, level, option_name, option_value, option_len);
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
    stdx::condition_variable _cv;
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

/**
 * Builds a wire protocol message with the given opcode wrapping a BSON body.
 */
Message makeMessage(NetworkOp op, const BSONObj& body);

/**
 * Writes an entire buffer to an fd, retrying short writes.
 */
void writeAll(int fd, const char* buf, size_t len);

/**
 * Returns an iovec pointing at the message's bytes, for scatter/gather I/O.
 */
iovec fromMessage(const Message& msg);

/**
 * Sends a wire message over a UDS with the given fds packed into a single SCM_RIGHTS cmsghdr.
 */
void sendMessageWithFds(int udsFd, const Message& msg, std::initializer_list<int> fds);

struct SessionHandoffMessageOptions {
    bool omitEmptyExtraCleartext = true;
};

/**
 * Builds an OP_HANDOFF wire message containing the serialized s2n session state and extra
 * cleartext.
 * The state is encoded as a BSON BinData field named "s2nState".
 * The extra cleartext is encoded as a BSON BinData field named "extraCleartext" if either of the
 * following are true:
 * - extraCleartext is nonempty
 * - the optionally specified options has `.omitEmptyExtraCleartext = false`. It defaults to `true`.
 */
Message buildSessionHandoffMessage(const std::vector<uint8_t>& serializedState,
                                   const std::vector<uint8_t>& extraCleartext,
                                   SessionHandoffMessageOptions options = {});

// Values used by buildProxyHeader and by tests that assert on the parsed source endpoint.
inline constexpr uint16_t kTestClientPort = 12345;
inline constexpr char kTestClientAddr[] = "127.0.0.1";
inline constexpr char kTestSniName[] = "test.example.com";

/**
 * Builds a PROXY v2 binary header. sniName, subjectDN, and rolesBytes/rolesLen are optional
 * — pass nullptr/0 to omit each. Always includes at least a NOOP TLV since the parser requires
 * a non-empty TLV block on the proxy unix socket (enforced by the parser).
 */
std::vector<uint8_t> buildProxyHeader(const char* sniName,
                                      const char* subjectDN,
                                      const uint8_t* rolesBytes,
                                      size_t rolesLen);

/**
 * Runs a single TLS handshake and exposes the artifacts needed to drive an OP_HANDOFF.
 *
 * Not a gtest fixture: the handshake is expensive and produces immutable outputs, so construct it
 * once and reuse across iterations. Derived classes (the gtest fixture, the fuzz test) add the
 * per-test/per-iteration pieces (the UDS socketpair and the HandoffSession).
 */
class HandoffTlsState {
public:
    HandoffTlsState();
    ~HandoffTlsState();

    HandoffTlsState(const HandoffTlsState&) = delete;
    HandoffTlsState& operator=(const HandoffTlsState&) = delete;

    template <typename... Messages>
    requires(std::same_as<Messages, Message> && ...)
    void clientSend(const Message& first, const Messages&... rest) {
        const iovec iovs[] = {fromMessage(first), fromMessage(rest)...};
        clientSend(iovs);
    }

    void clientSend(const char* data, size_t len);
    void clientSend(std::span<const iovec> buffers);

    Message clientReceive(int expectedSize);
    Message serverReceive(int expectedSize);

    /** Serializes the server-side TLS state and remaining decrypted data. */
    void serializeConnection();

protected:
    const s2n_config* deserConfig() const {
        return _deserConfig;
    }

    int proxyTlsFd = -1;
    int clientTlsFd = -1;
    struct s2n_connection* clientConn = nullptr;
    struct s2n_connection* serverConn = nullptr;
    std::vector<uint8_t> serializedState;
    std::vector<uint8_t> extraCleartext;

private:
    static std::string readFile(const std::string& path);
    static std::pair<std::string, std::string> splitCertAndKey(const std::string& combined);
    static int negotiateBlocking(struct s2n_connection* conn);
    Message _receive(s2n_connection* conn, int expectedSize);

    struct s2n_config* _serverConfig = nullptr;
    struct s2n_config* _clientConfig = nullptr;
    struct s2n_config* _deserConfig = nullptr;
    struct s2n_cert_chain_and_key* _certChainAndKey = nullptr;
};

}  // namespace mongo::transport
