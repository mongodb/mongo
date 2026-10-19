// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/data_range.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/http_client.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

namespace mongo {
namespace {

int createListenSocket(int* outPort) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    int opt = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    if (::listen(fd, 64) != 0) {
        ::close(fd);
        return -1;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &len) != 0) {
        ::close(fd);
        return -1;
    }
    *outPort = ntohs(addr.sin_port);
    return fd;
}

/**
 * Minimal HTTP/1.1 server that records the method, headers, completeness, and
 * body of every request it receives. Supports keep-alive so that curl does not
 * need to rewind uploads.
 */
class RecordingHttpServer {
public:
    struct Request {
        std::string method;
        std::string headers;
        std::string body;
        bool complete = true;
    };

    RecordingHttpServer() {
        _listenFd = createListenSocket(&_port);
        ASSERT_GTE(_listenFd, 0);
        _acceptThread = stdx::thread([this] { _acceptLoop(); });
    }

    ~RecordingHttpServer() {
        shutdown();
    }

    void shutdown() {
        if (_shutdown.swap(true)) {
            return;
        }
        ::close(_listenFd);

        // A blocked accept() is not interrupted by closing the listening
        // socket; wake it with a dummy connection.
        int wake = ::socket(AF_INET, SOCK_STREAM, 0);
        if (wake >= 0) {
            struct sockaddr_in addr;
            std::memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(_port);
            (void)::connect(wake, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
            ::close(wake);
        }

        std::vector<std::unique_ptr<stdx::thread>> connThreads;
        {
            std::lock_guard<std::mutex> lock(_connMutex);
            for (int fd : _connFds) {
                ::shutdown(fd, SHUT_RDWR);
            }
            connThreads = std::move(_connThreads);
        }
        for (auto& t : connThreads) {
            t->join();
        }
        _acceptThread.join();
    }

    int port() const {
        return _port;
    }

    std::vector<Request> getRequests() {
        std::lock_guard<std::mutex> lock(_recordsMutex);
        return _records;
    }

private:
    void _acceptLoop() {
        while (!_shutdown.load()) {
            int fd = ::accept(_listenFd, nullptr, nullptr);
            if (fd < 0) {
                continue;
            }
            std::lock_guard<std::mutex> lock(_connMutex);
            if (_shutdown.load()) {
                ::close(fd);
                return;
            }
            _connFds.push_back(fd);
            _connThreads.push_back(std::make_unique<stdx::thread>([this, fd] { _serve(fd); }));
        }
    }

    static bool _recvMore(int fd, std::string& buf) {
        char chunk[4096];
        ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            return false;
        }
        buf.append(chunk, n);
        return true;
    }

    static bool _sendAll(int fd, const std::string& data) {
        size_t sent = 0;
        while (sent < data.size()) {
            ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
            if (n <= 0) {
                return false;
            }
            sent += n;
        }
        return true;
    }

    void _serve(int fd) {
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        const std::string okReply = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
        std::string buf;
        while (!_shutdown.load()) {
            size_t headerEnd = std::string::npos;
            while ((headerEnd = buf.find("\r\n\r\n")) == std::string::npos) {
                if (!_recvMore(fd, buf)) {
                    ::close(fd);
                    return;
                }
            }

            std::string method = buf.substr(0, buf.find(' '));

            size_t contentLength = 0;
            size_t pos = buf.find("Content-Length:");
            if (pos != std::string::npos && pos < headerEnd) {
                contentLength =
                    std::strtoul(buf.c_str() + pos + sizeof("Content-Length:") - 1, nullptr, 10);
            }

            pos = buf.find("Expect: 100-continue");
            if (pos != std::string::npos && pos < headerEnd) {
                if (!_sendAll(fd, "HTTP/1.1 100 Continue\r\n\r\n")) {
                    ::close(fd);
                    return;
                }
            }

            size_t endOfRequest = headerEnd + 4 + contentLength;
            if (contentLength > 1 << 20) {
                ::close(fd);
                return;
            }
            bool complete = true;
            while (buf.size() < endOfRequest) {
                if (!_recvMore(fd, buf)) {
                    complete = false;
                    break;
                }
            }

            {
                std::lock_guard<std::mutex> lock(_recordsMutex);
                _records.push_back(
                    {method,
                     buf.substr(0, headerEnd),
                     buf.substr(headerEnd + 4, std::min(contentLength, buf.size() - headerEnd - 4)),
                     complete});
            }
            if (!complete) {
                ::close(fd);
                return;
            }

            if (!_sendAll(fd, okReply)) {
                ::close(fd);
                return;
            }
            buf.erase(0, endOfRequest);
        }
        ::close(fd);
    }

    int _listenFd = -1;
    int _port = 0;
    Atomic<bool> _shutdown{false};
    stdx::thread _acceptThread;

    std::mutex _connMutex;
    std::vector<int> _connFds;
    std::vector<std::unique_ptr<stdx::thread>> _connThreads;

    std::mutex _recordsMutex;
    std::vector<Request> _records;
};

std::unique_ptr<HttpClient> makeInsecureClient() {
    auto client = HttpClient::createWithFirewall({});
    client->allowInsecureHTTP(true);
    client->setConnectTimeout(Seconds(5));
    client->setTimeout(Seconds(15));
    return client;
}

// Filled with repeats of "UAF-LEAK" so any 8-byte slice is the marker.
char leakTarget[16384];
bool leakTargetInitialized = false;

/**
 * BufReader is three consecutive pointer fields (start, pos, end), so any
 * 8-aligned address in this function's overwritten frame reads as a BufReader
 * with remaining() == 8 pointing into leakTarget.
 */
__attribute__((noinline)) void poisonStackSlots() {
    if (!leakTargetInitialized) {
        for (size_t i = 0; i < sizeof(leakTarget); i += 8) {
            std::memcpy(leakTarget + i, "UAF-LEAK", 8);
        }
        leakTargetInitialized = true;
    }

    // Volatile stores so the frame writes cannot be optimized out.
    char buf[16384];
    auto* words = reinterpret_cast<volatile uintptr_t*>(buf);
    constexpr size_t kNumWords = 2000;
    for (size_t i = 0; i < kNumWords; ++i) {
        words[i] = reinterpret_cast<uintptr_t>(leakTarget) + 8 * i;
    }
}

// A PUT stores its BufReader's stack address in the pooled handle's read
// data. The canary pattern makes carried-over read state show up as uploaded
// canary bytes rather than a crash.
TEST(HttpClientCurl, DeleteOnPooledHandleDoesNotUploadPoisonedStackBytes) {
    if (!hasGlobalServiceContext()) {
        setGlobalServiceContext(ServiceContext::make());
    }

    RecordingHttpServer server;
    const std::string url = "http://127.0.0.1:" + std::to_string(server.port()) + "/upload";

    auto primeClient = makeInsecureClient();
    auto deleteClient = makeInsecureClient();

    // 8-byte body so it matches the remaining() reported by the patterned
    // stack region.
    const std::string primeBody = "12345678";

    std::string putError;
    std::string deleteError;

    Notification<void> putAndPatternDone;
    Notification<void> deleteCompleted;

    stdx::thread primeThread([&] {
        try {
            (void)primeClient->request(HttpClient::HttpMethod::kPUT,
                                       url,
                                       ConstDataRange(primeBody.data(), primeBody.size()));
        } catch (const DBException& e) {
            putError = e.what();
        }
        // The PUT's stack address is still stored in the pooled handle;
        // overwrite the dead frame with the canary pattern and park so it
        // stays mapped for the DELETE below.
        poisonStackSlots();
        putAndPatternDone.set();
        // Block here so this thread's stack, which the pooled handle points
        // into, stays mapped while the follow-up request runs.
        deleteCompleted.get();
    });

    putAndPatternDone.get();

    bool canaryLeaked = false;
    try {
        (void)deleteClient->request(HttpClient::HttpMethod::kDELETE, url);
    } catch (const DBException& e) {
        deleteError = e.what();
    }

    deleteCompleted.set();
    primeThread.join();
    server.shutdown();

    ASSERT_TRUE(putError.empty()) << "PUT failed: " << putError;
    ASSERT_TRUE(deleteError.empty()) << "DELETE failed: " << deleteError;

    for (const auto& req : server.getRequests()) {
        if (req.method != "DELETE") {
            continue;
        }
        if (req.body.size() == 8 && std::memcmp(req.body.data(), "UAF-LEAK", 8) == 0) {
            canaryLeaked = true;
        }
    }

    ASSERT_FALSE(canaryLeaked) << "the DELETE uploaded stale stack bytes; request count: "
                               << server.getRequests().size();
}

const char* methodName(HttpClient::HttpMethod method) {
    switch (method) {
        case HttpClient::HttpMethod::kGET:
            return "GET";
        case HttpClient::HttpMethod::kPOST:
            return "POST";
        case HttpClient::HttpMethod::kPUT:
            return "PUT";
        case HttpClient::HttpMethod::kPATCH:
            return "PATCH";
        case HttpClient::HttpMethod::kDELETE:
            return "DELETE";
    }
    return "unknown";
}

TEST(HttpClientCurl, PooledHandleCarriesNoStateBetweenMethods) {
    if (!hasGlobalServiceContext()) {
        setGlobalServiceContext(ServiceContext::make());
    }

    RecordingHttpServer server;
    const std::string url = "http://127.0.0.1:" + std::to_string(server.port()) + "/upload";

    auto primeClient = makeInsecureClient();
    auto followClient = makeInsecureClient();

    const std::string body = "method-matrix-body";

    for (auto primeMethod : {HttpClient::HttpMethod::kPUT,
                             HttpClient::HttpMethod::kPOST,
                             HttpClient::HttpMethod::kPATCH}) {
        for (auto followMethod : {HttpClient::HttpMethod::kGET, HttpClient::HttpMethod::kDELETE}) {
            Notification<void> primed;
            Notification<void> followDone;
            std::string primeError;
            std::string followError;

            stdx::thread primeThread([&] {
                try {
                    (void)primeClient->request(
                        primeMethod, url, ConstDataRange(body.data(), body.size()));
                } catch (const DBException& e) {
                    primeError = e.what();
                }
                primed.set();
                // Block here so this thread's stack, which the pooled handle
                // points into, stays mapped while the follow-up request runs.
                followDone.get();
            });

            primed.get();
            try {
                (void)followClient->request(followMethod, url);
            } catch (const DBException& e) {
                followError = e.what();
            }
            followDone.set();
            primeThread.join();

            ASSERT_TRUE(primeError.empty()) << methodName(primeMethod) << " failed: " << primeError;
            ASSERT_TRUE(followError.empty())
                << methodName(followMethod) << " failed: " << followError;

            auto reqs = server.getRequests();
            ASSERT_GTE(reqs.size(), 2);
            const auto& primeReq = reqs[reqs.size() - 2];
            const auto& followReq = reqs.back();
            ASSERT_EQ(primeReq.method, methodName(primeMethod));
            ASSERT_EQ(primeReq.body, body) << "prime request body was not sent intact";
            ASSERT_EQ(followReq.method, methodName(followMethod));
            ASSERT_TRUE(followReq.complete)
                << methodName(followMethod) << " sent fewer bytes than it promised";
            ASSERT_EQ(followReq.body.size(), 0)
                << methodName(followMethod) << " carried over body state from the "
                << methodName(primeMethod) << " request";
        }
    }
}

}  // namespace
}  // namespace mongo
