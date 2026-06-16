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

#include "mongo/transport/handoff/handoff_session.h"

#include "mongo/base/data_range.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/auth_options_gen.h"
#include "mongo/db/auth/restriction_environment.h"
#include "mongo/rpc/message.h"
#include "mongo/transport/handoff/session_handoff_message_gen.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/ssl_peer_info.h"
#include "mongo/util/shared_buffer.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <future>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <s2n.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace mongo::transport::handoff_transport {
namespace {

/**
 * Builds a wire protocol message with the given opcode wrapping a BSON body.
 */
Message makeMessage(NetworkOp op, const BSONObj& body) {
    int totalLen = sizeof(MSGHEADER::Value) + body.objsize();
    auto buf = SharedBuffer::allocate(totalLen);

    MsgData::View view(buf.get());
    view.setLen(totalLen);
    view.setId(nextMessageId());
    view.setResponseToMsgId(0);
    view.setOperation(op);
    memcpy(view.data(), body.objdata(), body.objsize());

    return Message(std::move(buf));
}

/**
 * Writes an entire buffer to an fd, retrying short writes.
 */
void writeAll(int fd, const char* buf, size_t len) {
    size_t written = 0;
    while (written < len) {
        ssize_t n = ::send(fd, buf + written, len - written, MSG_NOSIGNAL);
        ASSERT_GT(n, 0) << "send failed: " << errorMessage(lastSocketError());
        written += n;
    }
}

/**
 * Reads an entire buffer from an fd, retrying short reads.
 */
void readAll(int fd, char* buf, size_t len) {
    size_t totalRead = 0;
    while (totalRead < len) {
        ssize_t n = ::recv(fd, buf + totalRead, len - totalRead, 0);
        ASSERT_GT(n, 0) << "recv failed or closed: " << errorMessage(lastSocketError());
        totalRead += n;
    }
}

/**
 * Sends a wire message over a UDS with the given fds packed into a single SCM_RIGHTS cmsghdr.
 */
void sendMessageWithFds(int udsFd, const Message& msg, std::initializer_list<int> fds) {
    struct iovec iov;
    iov.iov_base = const_cast<char*>(msg.buf());
    iov.iov_len = msg.size();

    size_t fdBytes = fds.size() * sizeof(int);
    std::vector<char> controlBuf(CMSG_SPACE(fdBytes), 0);

    struct msghdr hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.msg_iov = &iov;
    hdr.msg_iovlen = 1;
    hdr.msg_control = controlBuf.data();
    hdr.msg_controllen = controlBuf.size();

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&hdr);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(fdBytes);
    memcpy(CMSG_DATA(cmsg), fds.begin(), fdBytes);

    ssize_t n;
    do {
        n = ::sendmsg(udsFd, &hdr, MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);
    ASSERT_GT(n, 0) << "sendmsg failed: " << errorMessage(lastSocketError());
}

/**
 * Builds an OP_HANDOFF wire message containing the serialized s2n session state.
 * The state is encoded as a BSON BinData field named "s2nState". Metadata (SNI, cert DN,
 * roles) is carried in the PROXY v2 header sent before this message, not in the BSON body.
 */
inline Message buildSessionHandoffMessage(const std::vector<uint8_t>& serializedState) {
    SessionHandoffMessage handoffMsg;
    handoffMsg.setS2nState(ConstDataRange(serializedState.data(), serializedState.size()));
    BSONObj body = handoffMsg.toBSON();

    int totalLen = sizeof(MSGHEADER::Value) + body.objsize();
    auto buf = SharedBuffer::allocate(totalLen);

    MsgData::View view(buf.get());
    view.setLen(totalLen);
    view.setId(nextMessageId());
    view.setResponseToMsgId(0);
    view.setOperation(dbSessionHandoff);
    memcpy(view.data(), body.objdata(), body.objsize());

    return Message(std::move(buf));
}

/**
 * Base fixture for tests that operate in Cleartext mode. setUp creates a UDS socketpair.
 * Tests call makeSession() to construct a HandoffSession on one end and drive the other
 * end via proxyFd(). The null TransportLayer and null s2n_config suffice because no TLS
 * deserialization is needed.
 */
class HandoffSessionCleartextFixture : public unittest::Test {
public:
    void setUp() override {
        int fds[2];
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0)
            << "socketpair failed: " << errorMessage(lastSocketError());
        mongodFd = fds[0];
        _proxyFd = fds[1];
    }

    void tearDown() override {
        closeProxy();
    }

    std::shared_ptr<HandoffSession> makeSession() {
        return std::make_shared<HandoffSession>(
            nullptr, mongodFd, HostAndPort("proxy", 0), HostAndPort("mongod", 0), nullptr);
    }

    int proxyFd() const {
        return _proxyFd;
    }

    void closeProxy() {
        if (_proxyFd >= 0) {
            ::close(_proxyFd);
            _proxyFd = -1;
        }
    }

private:
    int mongodFd = -1;
    int _proxyFd = -1;
};

//
// Properties: static session interface properties that hold regardless of mode or operation.
//

class HandoffSessionPropertiesTest : public HandoffSessionCleartextFixture {};

/** A freshly constructed session begins in Cleartext mode. */
TEST_F(HandoffSessionPropertiesTest, InitialStateIsCleartext) {
    auto session = makeSession();
    ASSERT_EQ(session->getState(), HandoffSessionState::Cleartext);
}

/** A freshly constructed session is connected. */
TEST_F(HandoffSessionPropertiesTest, InitialStateIsConnected) {
    auto session = makeSession();
    ASSERT_TRUE(session->isConnected());
}

/** A handoff session is never a load balancer connection. */
TEST_F(HandoffSessionPropertiesTest, NotALoadBalancerConnection) {
    auto session = makeSession();
    ASSERT_FALSE(session->isLoadBalancerPeer());
    ASSERT_FALSE(session->isConnectedToLoadBalancerPort());
}

/** A handoff session is never connected to a priority port. */
TEST_F(HandoffSessionPropertiesTest, NotConnectedToPriorityPort) {
    auto session = makeSession();
    ASSERT_FALSE(session->isConnectedToPriorityPort());
}

/** A handoff session is always connected via a proxy unix socket. */
TEST_F(HandoffSessionPropertiesTest, IsConnectedToProxyUnixSocket) {
    auto session = makeSession();
    ASSERT_TRUE(session->isConnectedToProxyUnixSocket());
}

/** A handoff session is never exempted by a CIDR list. */
TEST_F(HandoffSessionPropertiesTest, NotExemptedByCIDRList) {
    auto session = makeSession();
    ASSERT_FALSE(session->isExemptedByCIDRList({}));
}

/** A handoff session binds to operation state. */
TEST_F(HandoffSessionPropertiesTest, SupportsBindingToOperationState) {
    auto session = makeSession();
    ASSERT_TRUE(session->bindsToOperationState());
}

/**
 * validateProxyUnixSocketPeerPermissions() returns OK. The handoff transport layer validates the
 * peer at accept time. By the time a HandoffSession exists, the check has already passed.
 */
TEST_F(HandoffSessionPropertiesTest, ValidateProxyUnixSocketPeerPermissionsReturnsOK) {
    auto session = makeSession();
    ASSERT_OK(session->validateProxyUnixSocketPeerPermissions());
}

/**
 * isConnected() returns false when the peer half-closes its write direction via shutdown(SHUT_WR).
 * shutdown(SHUT_WR) sets POLLIN|POLLRDHUP in revents but not POLLHUP (which only appears once both
 * directions are closed). Without POLLRDHUP in the events mask and revents check, isConnected()
 * would incorrectly return true.
 */
TEST_F(HandoffSessionPropertiesTest, IsConnectedReturnsFalseOnPeerShutdownWrite) {
    auto session = makeSession();
    ::shutdown(proxyFd(), SHUT_WR);
    ASSERT_FALSE(session->isConnected());
}

class HandoffSessionPropertiesDeathTest : public HandoffSessionCleartextFixture {};

/** cancelAsyncOperations() is a no-op and does not crash. */
TEST_F(HandoffSessionPropertiesTest, CancelAsyncOperationsIsNoOp) {
    auto session = makeSession();
    session->cancelAsyncOperations();
}

DEATH_TEST_F(HandoffSessionPropertiesDeathTest,
             AsyncSourceMessageIsUnsupported,
             "Hit a MONGO_UNREACHABLE") {
    (void)makeSession()->asyncSourceMessage();
}

DEATH_TEST_F(HandoffSessionPropertiesDeathTest,
             AsyncSinkMessageIsUnsupported,
             "Hit a MONGO_UNREACHABLE") {
    (void)makeSession()->asyncSinkMessage(makeMessage(dbMsg, BSON("a" << 1)));
}

DEATH_TEST_F(HandoffSessionPropertiesDeathTest,
             AsyncWaitForDataIsUnsupported,
             "Hit a MONGO_UNREACHABLE") {
    (void)makeSession()->asyncWaitForData();
}

DEATH_TEST_F(HandoffSessionPropertiesDeathTest,
             SetIsLoadBalancerPeerIsUnsupported,
             "Hit a MONGO_UNREACHABLE") {
    auto session = makeSession();
    session->setIsLoadBalancerPeer(false);
}

//
// Pre-handoff: message behavior in Cleartext mode.
//

class HandoffSessionPreHandoffMessageTest : public HandoffSessionCleartextFixture {};

/**
 * appendToBSON includes the session id, Cleartext state, and remote/local addresses from the
 * constructor.
 */
TEST_F(HandoffSessionPreHandoffMessageTest, AppendToBSONIncludesSessionInfo) {
    auto session = makeSession();

    BSONObjBuilder bb;
    session->appendToBSON(bb);
    auto obj = bb.obj();
    ASSERT_TRUE(obj.hasField("id"));
    ASSERT_EQ(obj.getStringField("state"), "cleartext");
    ASSERT_EQ(obj.getStringField("remote"), "proxy:0");
    ASSERT_EQ(obj.getStringField("local"), "mongod:0");
}

/** sourceMessage() returns the message. */
TEST_F(HandoffSessionPreHandoffMessageTest, SourceMessageReturnsMessage) {
    auto session = makeSession();

    Message msg = makeMessage(dbMsg, BSON("hello" << 1));
    writeAll(proxyFd(), msg.buf(), msg.size());

    auto result = session->sourceMessage();
    ASSERT_OK(result.getStatus());
    ASSERT_EQ(result.getValue().size(), msg.size());
    ASSERT_EQ(memcmp(result.getValue().buf(), msg.buf(), msg.size()), 0);
}

/** Several messages sent back-to-back are sourced in order. */
TEST_F(HandoffSessionPreHandoffMessageTest, SourceMessagePreservesOrder) {
    auto session = makeSession();

    std::vector<Message> sent;
    for (int i = 0; i < 3; ++i) {
        sent.push_back(makeMessage(dbMsg, BSON("n" << i)));
    }
    for (const auto& m : sent) {
        writeAll(proxyFd(), m.buf(), m.size());
    }

    for (const auto& m : sent) {
        auto result = session->sourceMessage();
        ASSERT_OK(result.getStatus());
        ASSERT_EQ(result.getValue().size(), m.size());
        ASSERT_EQ(memcmp(result.getValue().buf(), m.buf(), m.size()), 0);
    }
}

/** sourceMessage() succeeds on a message with no body beyond the header. */
TEST_F(HandoffSessionPreHandoffMessageTest, SourceMessageWithNoBody) {
    auto session = makeSession();

    MSGHEADER::Value header;
    MsgData::View view(reinterpret_cast<char*>(&header));
    view.setLen(sizeof(MSGHEADER::Value));
    view.setId(nextMessageId());
    view.setResponseToMsgId(0);
    view.setOperation(dbMsg);

    writeAll(proxyFd(), reinterpret_cast<const char*>(&header), sizeof(header));

    auto result = session->sourceMessage();
    ASSERT_OK(result.getStatus());
    ASSERT_EQ(result.getValue().size(), static_cast<int>(sizeof(MSGHEADER::Value)));
    ASSERT_EQ(memcmp(result.getValue().buf(), &header, sizeof(MSGHEADER::Value)), 0);
}

/** sourceMessage() reassembles a message delivered in small chunks. */
TEST_F(HandoffSessionPreHandoffMessageTest, SourceMessageReassemblesFragmentedMessage) {
    auto session = makeSession();

    Message msg = makeMessage(dbMsg, BSON("payload" << std::string(64, 'x')));
    const int totalSize = msg.size();

    // Write the message out in tiny chunks from a separate thread. Both the header and body
    // arrive across multiple reads.
    std::thread sender([&] {
        const char* p = msg.buf();
        size_t off = 0;
        constexpr size_t kChunk = 7;
        while (off < static_cast<size_t>(totalSize)) {
            size_t n = std::min(kChunk, static_cast<size_t>(totalSize) - off);
            writeAll(proxyFd(), p + off, n);
            off += n;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    auto result = session->sourceMessage();
    sender.join();

    ASSERT_OK(result.getStatus());
    ASSERT_EQ(result.getValue().size(), totalSize);
    ASSERT_EQ(memcmp(result.getValue().buf(), msg.buf(), totalSize), 0);
}

/** sinkMessage() delivers a message to the peer. */
TEST_F(HandoffSessionPreHandoffMessageTest, SinkMessageDeliversMessage) {
    auto session = makeSession();

    Message msg = makeMessage(dbMsg, BSON("reply" << 1));
    int msgSize = msg.size();
    std::vector<char> original(msg.buf(), msg.buf() + msgSize);
    ASSERT_OK(session->sinkMessage(std::move(msg)));

    auto buf = SharedBuffer::allocate(msgSize);
    readAll(proxyFd(), buf.get(), msgSize);

    ASSERT_EQ(memcmp(buf.get(), original.data(), msgSize), 0);
}

/** sourceMessage() rejects a header whose length is below the minimum. */
TEST_F(HandoffSessionPreHandoffMessageTest, SourceMessageRejectsTooSmallLength) {
    auto session = makeSession();

    MSGHEADER::Value header;
    MsgData::View view(reinterpret_cast<char*>(&header));
    view.setLen(static_cast<int32_t>(sizeof(MSGHEADER::Value)) - 1);
    view.setId(nextMessageId());
    view.setResponseToMsgId(0);
    view.setOperation(dbMsg);

    writeAll(proxyFd(), reinterpret_cast<const char*>(&header), sizeof(header));

    auto result = session->sourceMessage();
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::ProtocolError);
}

/** sourceMessage() rejects a header whose length exceeds MaxMessageSizeBytes. */
TEST_F(HandoffSessionPreHandoffMessageTest, SourceMessageRejectsTooLargeLength) {
    auto session = makeSession();

    MSGHEADER::Value header;
    MsgData::View view(reinterpret_cast<char*>(&header));
    view.setLen(static_cast<int32_t>(MaxMessageSizeBytes) + 1);
    view.setId(nextMessageId());
    view.setResponseToMsgId(0);
    view.setOperation(dbMsg);

    writeAll(proxyFd(), reinterpret_cast<const char*>(&header), sizeof(header));

    auto result = session->sourceMessage();
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::ProtocolError);
}

/**
 * A non-handoff message that arrives with an SCM_RIGHTS fd is returned normally. The stray fd is
 * closed rather than leaked. Closure is verified via a pipe. When the write end is fully closed,
 * a non-blocking read of the read end returns EOF.
 */
TEST_F(HandoffSessionPreHandoffMessageTest, NonHandoffMessageWithFdIsClosed) {
    auto session = makeSession();

    int pipeFds[2];
    ASSERT_EQ(::pipe(pipeFds), 0) << "pipe failed: " << errorMessage(lastSocketError());
    int pipeRead = pipeFds[0];
    int pipeWrite = pipeFds[1];
    ON_BLOCK_EXIT([&] { ::close(pipeRead); });

    Message msg = makeMessage(dbMsg, BSON("ping" << 1));
    sendMessageWithFds(proxyFd(), msg, {pipeWrite});
    // The message is already in the kernel socket buffer, so closing our copy of the pipe
    // write end does not affect message delivery. Once the session also closes its copy, no
    // open file descriptions for the write end remain and the read end returns EOF.
    ::close(pipeWrite);

    auto result = session->sourceMessage();
    ASSERT_OK(result.getStatus());
    ASSERT_EQ(result.getValue().size(), msg.size());
    ASSERT_EQ(memcmp(result.getValue().buf(), msg.buf(), msg.size()), 0);

    // Verify that the session closed the stray fd by checking for EOF on the pipe read end.
    int flags = ::fcntl(pipeRead, F_GETFL, 0);
    ASSERT_EQ(::fcntl(pipeRead, F_SETFL, flags | O_NONBLOCK), 0);
    char buf;
    ssize_t n = ::read(pipeRead, &buf, 1);
    ASSERT_EQ(n, 0) << "received fd was leaked: pipe write end still open (read returned " << n
                    << ", errno " << errno << ")";
}

/** sourceMessage() fails when the peer closes the connection. */
TEST_F(HandoffSessionPreHandoffMessageTest, SourceMessageFailsOnPeerDisconnect) {
    auto session = makeSession();
    closeProxy();

    ASSERT_EQ(session->sourceMessage().getStatus().code(), ErrorCodes::SocketException);
    ASSERT_FALSE(session->isConnected());
}

/** sinkMessage() fails when the peer closes the connection. */
TEST_F(HandoffSessionPreHandoffMessageTest, SinkMessageFailsOnPeerDisconnect) {
    auto session = makeSession();
    closeProxy();

    ASSERT_EQ(session->sinkMessage(makeMessage(dbMsg, BSON("ping" << 1))).code(),
              ErrorCodes::SocketException);
    ASSERT_FALSE(session->isConnected());
}

//
// Pre-handoff: timeout behavior in Cleartext mode.
//

class HandoffSessionPreHandoffTimeoutTest : public HandoffSessionCleartextFixture {
protected:
    // Short deadline used by all timeout tests. It is small enough to keep the suite fast and
    // large enough to be reliably detected on a slow machine.
    static constexpr Milliseconds kTestTimeout{50};
};

/** waitForData() reports NetworkTimeout when no data arrives. */
TEST_F(HandoffSessionPreHandoffTimeoutTest, WaitForDataTimesOut) {
    auto session = makeSession();
    session->setTimeout(kTestTimeout);

    // The peer is connected but sends no data.
    ASSERT_EQ(session->waitForData().code(), ErrorCodes::NetworkTimeout);
}

/** sourceMessage() times out when no data arrives. */
TEST_F(HandoffSessionPreHandoffTimeoutTest, SourceMessageRespectsTimeout) {
    auto session = makeSession();
    session->setTimeout(kTestTimeout);

    ASSERT_EQ(session->sourceMessage().getStatus().code(), ErrorCodes::NetworkTimeout);
}

/** sourceMessage() times out when the header arrives but the body is withheld. */
TEST_F(HandoffSessionPreHandoffTimeoutTest, SourceMessageRespectsTimeoutAfterHeader) {
    auto session = makeSession();
    session->setTimeout(kTestTimeout);

    Message msg = makeMessage(dbMsg, BSON("ping" << 1));
    writeAll(proxyFd(), msg.buf(), sizeof(MSGHEADER::Value));

    ASSERT_EQ(session->sourceMessage().getStatus().code(), ErrorCodes::NetworkTimeout);
}

/**
 * Clearing the timeout with setTimeout(boost::none) restores blocking behavior. A message that
 * arrives after a short delay is received successfully rather than rejected as timed out.
 */
TEST_F(HandoffSessionPreHandoffTimeoutTest, ClearingTimeoutRestoresBlockingBehavior) {
    auto session = makeSession();
    session->setTimeout(kTestTimeout);
    session->setTimeout(boost::none);

    // Deliver a message from a thread after a brief delay. The session must not time out.
    Message msg = makeMessage(dbMsg, BSON("hello" << 1));
    std::thread sender([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        writeAll(proxyFd(), msg.buf(), msg.size());
    });

    auto result = session->sourceMessage();
    sender.join();

    ASSERT_OK(result.getStatus());
    ASSERT_EQ(result.getValue().size(), msg.size());
    ASSERT_EQ(memcmp(result.getValue().buf(), msg.buf(), msg.size()), 0);
}

//
// Pre-handoff: session termination in Cleartext mode.
//

class HandoffSessionPreHandoffEndSessionTest : public HandoffSessionCleartextFixture {};

/** end() disconnects the session and closes the proxy UDS fd. */
TEST_F(HandoffSessionPreHandoffEndSessionTest, EndDisconnectsSessionAndClosesProxyUdsFd) {
    auto session = makeSession();
    session->end();
    ASSERT_FALSE(session->isConnected());

    // Verify that the session closed the UDS fd by checking for EOF on proxyFd().
    char buf;
    ASSERT_EQ(::recv(proxyFd(), &buf, 1, 0), 0);
}

/** end() is idempotent. */
TEST_F(HandoffSessionPreHandoffEndSessionTest, EndIsIdempotent) {
    auto session = makeSession();
    session->end();
    session->end();
    ASSERT_FALSE(session->isConnected());
}

/** sourceMessage() after end() fails. */
TEST_F(HandoffSessionPreHandoffEndSessionTest, SourceMessageAfterEndFails) {
    auto session = makeSession();
    session->end();

    auto result = session->sourceMessage();
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::SocketException);
}

/** sinkMessage() after end() fails. */
TEST_F(HandoffSessionPreHandoffEndSessionTest, SinkMessageAfterEndFails) {
    auto session = makeSession();
    session->end();

    auto status = session->sinkMessage(makeMessage(dbMsg, BSON("a" << 1)));
    ASSERT_EQ(status.code(), ErrorCodes::SocketException);
}

/** waitForData() after end() fails. */
TEST_F(HandoffSessionPreHandoffEndSessionTest, WaitForDataAfterEndFails) {
    auto session = makeSession();
    session->end();

    ASSERT_EQ(session->waitForData().code(), ErrorCodes::SocketException);
}

/**
 * waitForData() fails when the peer half-closes its write direction via shutdown(SHUT_WR).
 * shutdown(SHUT_WR) sets POLLIN|POLLRDHUP in revents but not POLLHUP (which only appears once
 * both directions are closed). Without POLLRDHUP in the revents check, waitForData() would
 * block indefinitely rather than returning an error.
 */
TEST_F(HandoffSessionPreHandoffEndSessionTest, WaitForDataFailsOnPeerShutdownWrite) {
    auto session = makeSession();
    ::shutdown(proxyFd(), SHUT_WR);
    ASSERT_EQ(session->waitForData().code(), ErrorCodes::SocketException);
}

/**
 * end() called concurrently with a blocked waitForData() must wake the blocked poll()
 * immediately. Without ::shutdown(SHUT_RDWR) before ::close(), Linux does not unblock a
 * concurrent poll() on close() alone.
 */
TEST_F(HandoffSessionPreHandoffEndSessionTest, EndUnblocksBlockedWaitForData) {
    auto session = makeSession();

    auto fut = std::async(std::launch::async, [&] { return session->waitForData(); });

    // The sleep gives waitForData() time to enter poll() before end() closes _fd. On a slow
    // machine, end() may win the race first and the poll() wakeup path goes untested.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    session->end();
    ASSERT_EQ(fut.wait_for(std::chrono::seconds(5)), std::future_status::ready)
        << "end() failed to unblock waitForData()";
    ASSERT_EQ(fut.get().code(), ErrorCodes::SocketException);
}

//
// OP_HANDOFF message validation: error paths that fail before any s2n config is used. A null
// config suffices.
//

class HandoffSessionHandoffMessageValidationTest : public HandoffSessionCleartextFixture {
protected:
    /**
     * This helper sends an OP_HANDOFF message with a throwaway fd attached and closes the
     * test's copy of that fd. The session takes ownership of the received fd and closes it
     * on the expected failure path.
     */
    StatusWith<Message> sourceHandoffWithFd(const std::shared_ptr<HandoffSession>& session,
                                            const Message& msg) {
        int fdToSend = ::open("/dev/null", O_RDONLY);
        ASSERT_GTE(fdToSend, 0) << "open /dev/null failed: " << errorMessage(lastSocketError());
        sendMessageWithFds(proxyFd(), msg, {fdToSend});
        ::close(fdToSend);
        return session->sourceMessage();
    }
};

/** OP_HANDOFF without an accompanying SCM_RIGHTS fd is rejected. */
TEST_F(HandoffSessionHandoffMessageValidationTest, SourceMessageRejectsHandoffWithoutFd) {
    auto session = makeSession();

    Message msg = buildSessionHandoffMessage(/*serializedState=*/std::vector<uint8_t>(16, 0));
    writeAll(proxyFd(), msg.buf(), msg.size());  // Sent without an attached fd.

    ASSERT_EQ(session->sourceMessage().getStatus().code(), ErrorCodes::ProtocolError);
}

/** OP_HANDOFF with an empty (header-only) body is rejected. */
TEST_F(HandoffSessionHandoffMessageValidationTest, SourceMessageRejectsHandoffWithEmptyBody) {
    auto session = makeSession();

    MSGHEADER::Value header;
    MsgData::View view(reinterpret_cast<char*>(&header));
    view.setLen(sizeof(MSGHEADER::Value));
    view.setId(nextMessageId());
    view.setResponseToMsgId(0);
    view.setOperation(dbSessionHandoff);

    auto buf = SharedBuffer::allocate(sizeof(MSGHEADER::Value));
    memcpy(buf.get(), &header, sizeof(MSGHEADER::Value));
    Message msg(std::move(buf));

    ASSERT_EQ(sourceHandoffWithFd(session, msg).getStatus().code(), ErrorCodes::ProtocolError);
}

/**
 * OP_HANDOFF with a BSON length prefix larger than the message body is rejected cleanly, without
 * reading past the buffer. This guards against heap over-reads and is meaningful under ASAN.
 */
TEST_F(HandoffSessionHandoffMessageValidationTest,
       SourceMessageRejectsHandoffWithMalformedBsonBody) {
    auto session = makeSession();

    constexpr int kBodyLen = 16;
    int totalLen = sizeof(MSGHEADER::Value) + kBodyLen;
    auto buf = SharedBuffer::allocate(totalLen);
    MsgData::View view(buf.get());
    view.setLen(totalLen);
    view.setId(nextMessageId());
    view.setResponseToMsgId(0);
    view.setOperation(dbSessionHandoff);

    // Write a BSON length prefix far larger than the actual body. The remaining bytes are
    // non-EOO filler that an unguarded BSONObj would iterate past the allocation boundary.
    int32_t oversizedBsonLength = kBodyLen + 4096;
    DataView(view.data()).write<LittleEndian<int32_t>>(oversizedBsonLength);
    memset(view.data() + sizeof(oversizedBsonLength), 0x10, kBodyLen - sizeof(oversizedBsonLength));
    Message msg(std::move(buf));

    ASSERT_EQ(sourceHandoffWithFd(session, msg).getStatus().code(), ErrorCodes::ProtocolError);
}

/** OP_HANDOFF with a valid BSON body that lacks the s2nState field is rejected. */
TEST_F(HandoffSessionHandoffMessageValidationTest, SourceMessageRejectsHandoffWithMissingS2NState) {
    auto session = makeSession();

    Message msg = makeMessage(dbSessionHandoff, BSONObj());
    ASSERT_EQ(sourceHandoffWithFd(session, msg).getStatus().code(), ErrorCodes::IDLFailedToParse);
}

/**
 * OP_HANDOFF with two fds packed into a single SCM_RIGHTS cmsghdr is rejected with ProtocolError.
 * Both fds fit in CMSG_SPACE(sizeof(int)) without MSG_CTRUNC, so the session must close them
 * explicitly: the extra fd in the SCM_RIGHTS loop and the first fd on rejection.
 */
TEST_F(HandoffSessionHandoffMessageValidationTest, SourceMessageRejectsHandoffWithMultipleFds) {
    auto session = makeSession();

    int pipeFds[2];
    ASSERT_EQ(::pipe(pipeFds), 0) << "pipe failed: " << errorMessage(lastSocketError());
    int pipeRead = pipeFds[0];
    int pipeWrite = pipeFds[1];
    ON_BLOCK_EXIT([&] { ::close(pipeRead); });

    // Duplicate the write end so we can send two distinct fds in the same SCM_RIGHTS cmsghdr.
    int pipeWriteDup = ::dup(pipeWrite);
    ASSERT_GTE(pipeWriteDup, 0) << "dup failed: " << errorMessage(lastSocketError());

    Message msg = buildSessionHandoffMessage(/*serializedState=*/std::vector<uint8_t>(16, 0));
    sendMessageWithFds(proxyFd(), msg, {pipeWrite, pipeWriteDup});
    // Close our copies now that they are queued. The session holds the only remaining
    // write-end references once it receives them.
    ::close(pipeWrite);
    ::close(pipeWriteDup);

    ASSERT_EQ(session->sourceMessage().getStatus().code(), ErrorCodes::ProtocolError);

    // Verify that the session closed both received fds by checking for EOF on the pipe read end.
    // Both fds fit in CMSG_SPACE(sizeof(int)) without MSG_CTRUNC, so the session must
    // close them explicitly rather than relying on the kernel.
    int flags = ::fcntl(pipeRead, F_GETFL, 0);
    ASSERT_EQ(::fcntl(pipeRead, F_SETFL, flags | O_NONBLOCK), 0);
    char buf;
    ssize_t n = ::read(pipeRead, &buf, 1);
    ASSERT_EQ(n, 0) << "received fds were leaked";
}

/**
 * Base fixture for tests that require a real TLS handshake. setUp runs a full TLS handshake,
 * serializes the server-side TLS state, and constructs a HandoffSession on a UDS socketpair.
 * The handoff is not driven. Derived fixtures drive it in their own setUp.
 */
class HandoffSessionTLSFixture : public unittest::Test {
public:
    void setUp() override {
        S2NInitGuard::instance();

        // Build the server config with TLS 1.2, serialization enabled, and a server cert.
        _serverConfig = s2n_config_new_minimal();
        ASSERT_TRUE(_serverConfig);
        ASSERT_EQ(s2n_config_set_serialization_version(_serverConfig, S2N_SERIALIZED_CONN_V1),
                  S2N_SUCCESS);
        ASSERT_EQ(
            s2n_config_set_cipher_preferences(_serverConfig, "ELBSecurityPolicy-TLS-1-2-2017-01"),
            S2N_SUCCESS);
        auto certPem = readFile(kServerCertKeyFile);
        auto [certChainPem, keyPem] = splitCertAndKey(certPem);
        _certChainAndKey = s2n_cert_chain_and_key_new();
        ASSERT_TRUE(_certChainAndKey);
        ASSERT_EQ(
            s2n_cert_chain_and_key_load_pem(_certChainAndKey, certChainPem.c_str(), keyPem.c_str()),
            S2N_SUCCESS);
        ASSERT_EQ(s2n_config_add_cert_chain_and_key_to_store(_serverConfig, _certChainAndKey),
                  S2N_SUCCESS);
        ASSERT_EQ(s2n_config_disable_x509_verification(_serverConfig), S2N_SUCCESS);

        // Build the client config with TLS 1.2, serialization enabled, and no cert verification.
        _clientConfig = s2n_config_new_minimal();
        ASSERT_TRUE(_clientConfig);
        ASSERT_EQ(s2n_config_set_serialization_version(_clientConfig, S2N_SERIALIZED_CONN_V1),
                  S2N_SUCCESS);
        ASSERT_EQ(
            s2n_config_set_cipher_preferences(_clientConfig, "ELBSecurityPolicy-TLS-1-2-2017-01"),
            S2N_SUCCESS);
        ASSERT_EQ(s2n_config_disable_x509_verification(_clientConfig), S2N_SUCCESS);

        // Build the deserialization config for the HandoffSession with the same cert as the server.
        _deserConfig = s2n_config_new_minimal();
        ASSERT_TRUE(_deserConfig);
        ASSERT_EQ(s2n_config_set_serialization_version(_deserConfig, S2N_SERIALIZED_CONN_V1),
                  S2N_SUCCESS);
        ASSERT_EQ(
            s2n_config_set_cipher_preferences(_deserConfig, "ELBSecurityPolicy-TLS-1-2-2017-01"),
            S2N_SUCCESS);
        ASSERT_EQ(s2n_config_add_cert_chain_and_key_to_store(_deserConfig, _certChainAndKey),
                  S2N_SUCCESS);
        ASSERT_EQ(s2n_config_disable_x509_verification(_deserConfig), S2N_SUCCESS);

        // Two socketpairs model the handoff protocol.
        //
        // UDS socketpair (proxy <-> mongod):
        //   proxyUdsFd  is the proxy end. The test sends OP_HANDOFF through it. After the
        //               handoff (success or failure), proxyUdsFd reaches EOF when the session
        //               closes mongodUdsFd.
        //   mongodUdsFd is the mongod end. It is passed to HandoffSession as its initial fd.
        //               The session reads OP_HANDOFF here and closes it whether the handoff
        //               succeeds or fails.
        //
        // TLS socketpair (proxy <-> client):
        //   proxyTlsFd  is the proxy end. It is sent inside OP_HANDOFF via SCM_RIGHTS. On a
        //               successful handoff, the session uses it as its new active fd for TLS I/O.
        //   clientTlsFd is the client end. clientConn uses this fd to send and receive data.
        //
        // When the session calls recvmsg on mongodUdsFd, the kernel gives it a duplicate of
        // proxyTlsFd. On a successful handoff, the session holds and uses that dup. On failure,
        // the session closes it.

        // Create a socketpair for the TLS channel between proxy and client.
        int tlsFds[2];
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, tlsFds), 0);
        proxyTlsFd = tlsFds[0];   // proxy's end.
        clientTlsFd = tlsFds[1];  // client's end.

        // Create and connect s2n server and client connections.
        auto* serverConn = s2n_connection_new(S2N_SERVER);
        ASSERT_TRUE(serverConn);
        ASSERT_EQ(s2n_connection_set_config(serverConn, _serverConfig), S2N_SUCCESS);
        ASSERT_EQ(s2n_connection_set_fd(serverConn, proxyTlsFd), S2N_SUCCESS);

        clientConn = s2n_connection_new(S2N_CLIENT);
        ASSERT_TRUE(clientConn);
        ASSERT_EQ(s2n_connection_set_config(clientConn, _clientConfig), S2N_SUCCESS);
        ASSERT_EQ(s2n_connection_set_fd(clientConn, clientTlsFd), S2N_SUCCESS);

        std::thread serverThread([&] {
            ASSERT_EQ(_negotiateBlocking(serverConn), S2N_SUCCESS) << s2n_strerror(s2n_errno, "EN");
        });
        ASSERT_EQ(_negotiateBlocking(clientConn), S2N_SUCCESS) << s2n_strerror(s2n_errno, "EN");
        serverThread.join();

        // Serialize the server-side TLS state.
        uint32_t serializedLen = 0;
        ASSERT_EQ(s2n_connection_serialization_length(serverConn, &serializedLen), S2N_SUCCESS);
        ASSERT_GT(serializedLen, 0u);
        serializedState.resize(serializedLen);
        ASSERT_EQ(s2n_connection_serialize(serverConn, serializedState.data(), serializedLen),
                  S2N_SUCCESS);
        s2n_connection_free(serverConn);

        // Create a UDS socketpair for the HandoffSession.
        int udsFds[2];
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, udsFds), 0);
        int mongodUdsFd = udsFds[0];  // mongod's HandoffSession end.
        proxyUdsFd = udsFds[1];       // proxy's end.

        session = std::make_shared<HandoffSession>(
            nullptr, mongodUdsFd, HostAndPort("proxy", 0), HostAndPort("mongod", 0), _deserConfig);
    }

    void tearDown() override {
        if (session) {
            session->end();
            session.reset();
        }
        if (proxyTlsFd >= 0) {
            ::close(proxyTlsFd);
            proxyTlsFd = -1;
        }
        if (proxyUdsFd >= 0) {
            ::close(proxyUdsFd);
            proxyUdsFd = -1;
        }
        // Close the client fd before freeing the s2n connection; session->end() has
        // already shut down the server side.
        if (clientTlsFd >= 0) {
            ::close(clientTlsFd);
            clientTlsFd = -1;
        }
        if (clientConn) {
            s2n_connection_free(clientConn);
            clientConn = nullptr;
        }
        if (_serverConfig) {
            s2n_config_free(_serverConfig);
        }
        if (_clientConfig) {
            s2n_config_free(_clientConfig);
        }
        if (_deserConfig) {
            s2n_config_free(_deserConfig);
        }
        if (_certChainAndKey) {
            s2n_cert_chain_and_key_free(_certChainAndKey);
        }
    }

    /**
     * Blocks until the session closes its UDS end (signalling the handoff is complete), then
     * closes and invalidates proxyUdsFd. Used by tests to verify that session handoff closes
     * mongodUdsFd.
     */
    void waitForSessionHandoff() {
        char eofBuf;
        ASSERT_EQ(::recv(proxyUdsFd, &eofBuf, 1, 0), 0);
        ::close(proxyUdsFd);
        proxyUdsFd = -1;
    }

    void clientSend(const Message& msg) {
        clientSend(msg.buf(), msg.size());
    }

    void clientSend(const char* buf, size_t len) {
        size_t written = 0;
        while (written < len) {
            s2n_blocked_status blocked;
            ssize_t n = s2n_send(clientConn, buf + written, len - written, &blocked);
            if (n < 0) {
                uassert(ErrorCodes::InternalError,
                        s2n_strerror(s2n_errno, "EN"),
                        s2n_error_get_type(s2n_errno) == S2N_ERR_T_BLOCKED);
                continue;
            }
            written += n;
        }
    }

    Message clientReceive(int expectedSize) {
        auto buf = SharedBuffer::allocate(expectedSize);
        size_t totalRead = 0;
        while (static_cast<int>(totalRead) < expectedSize) {
            s2n_blocked_status blocked;
            ssize_t n =
                s2n_recv(clientConn, buf.get() + totalRead, expectedSize - totalRead, &blocked);
            if (n < 0) {
                uassert(ErrorCodes::InternalError,
                        s2n_strerror(s2n_errno, "EN"),
                        s2n_error_get_type(s2n_errno) == S2N_ERR_T_BLOCKED);
                continue;
            }
            uassert(ErrorCodes::InternalError, "client connection closed unexpectedly", n > 0);
            totalRead += n;
        }
        return Message(std::move(buf));
    }

protected:
    std::shared_ptr<HandoffSession> session;
    std::vector<uint8_t> serializedState;
    int proxyTlsFd = -1;
    int proxyUdsFd = -1;
    int clientTlsFd = -1;
    struct s2n_connection* clientConn = nullptr;

private:
    static constexpr auto kServerCertKeyFile = "jstests/libs/server.pem";

    static std::string readFile(const std::string& path) {
        std::ifstream file(path);
        ASSERT_TRUE(file.good()) << "Failed to open file: " << path;
        std::stringstream ss;
        ss << file.rdbuf();
        return ss.str();
    }

    static std::pair<std::string, std::string> splitCertAndKey(const std::string& combined) {
        const std::string certBegin = "-----BEGIN CERTIFICATE-----";
        const std::string certEnd = "-----END CERTIFICATE-----";
        const std::string keyBegin = "-----BEGIN";

        std::string certChain;
        std::string privateKey;

        size_t pos = 0;
        while ((pos = combined.find(certBegin, pos)) != std::string::npos) {
            size_t endPos = combined.find(certEnd, pos);
            ASSERT_NE(endPos, std::string::npos) << "Unterminated certificate block in PEM";
            endPos += certEnd.size();
            if (endPos < combined.size() && combined[endPos] == '\n') {
                endPos++;
            }
            certChain += combined.substr(pos, endPos - pos);
            pos = endPos;
        }

        pos = 0;
        while ((pos = combined.find(keyBegin, pos)) != std::string::npos) {
            if (combined.substr(pos).starts_with("-----BEGIN CERTIFICATE-----")) {
                pos += certBegin.size();
                continue;
            }
            size_t endPos = combined.find("-----END ", pos + 1);
            ASSERT_NE(endPos, std::string::npos) << "Unterminated key block in PEM";
            endPos = combined.find('\n', endPos);
            if (endPos == std::string::npos) {
                endPos = combined.size();
            } else {
                endPos++;
            }
            privateKey = combined.substr(pos, endPos - pos);
            break;
        }

        ASSERT_FALSE(certChain.empty()) << "No certificate found in PEM file";
        ASSERT_FALSE(privateKey.empty()) << "No private key found in PEM file";
        return {certChain, privateKey};
    }

    // Process-level RAII guard for s2n_init/s2n_cleanup. s2n_init must only be called once per
    // process; use instance() to ensure it is initialized exactly once.
    struct S2NInitGuard {
        static S2NInitGuard& instance() {
            static S2NInitGuard guard;
            return guard;
        }

    private:
        S2NInitGuard() {
            ASSERT_EQ(s2n_init(), S2N_SUCCESS) << "s2n_init: " << s2n_strerror(s2n_errno, "EN");
        }
        ~S2NInitGuard() {
            s2n_cleanup();
        }
    };

    static int _negotiateBlocking(struct s2n_connection* conn) {
        s2n_blocked_status blocked;
        while (true) {
            int rc = s2n_negotiate(conn, &blocked);
            if (rc == S2N_SUCCESS) {
                return S2N_SUCCESS;
            }
            if (s2n_error_get_type(s2n_errno) != S2N_ERR_T_BLOCKED) {
                return S2N_FAILURE;
            }
        }
    }

    struct s2n_config* _serverConfig = nullptr;
    struct s2n_config* _clientConfig = nullptr;
    struct s2n_config* _deserConfig = nullptr;
    struct s2n_cert_chain_and_key* _certChainAndKey = nullptr;
};

//
// Handoff transition: the handoff protocol's success path and s2n-level failure path.
//

class HandoffSessionHandoffTransitionTest : public HandoffSessionTLSFixture {};

/**
 * A valid OP_HANDOFF transitions the session to TLS mode and causes proxyUdsFd to reach EOF.
 */
TEST_F(HandoffSessionHandoffTransitionTest, SuccessfulHandoff) {
    Message handoffMsg = buildSessionHandoffMessage(serializedState);
    sendMessageWithFds(proxyUdsFd, handoffMsg, {proxyTlsFd});
    // The session receives a dup of proxyTlsFd via SCM_RIGHTS and uses it for TLS I/O.
    // Our copy is no longer needed.
    ::close(proxyTlsFd);
    proxyTlsFd = -1;

    // sourceMessage() processes the handoff inline, closes mongodUdsFd, then recurses and blocks
    // waiting for the first TLS message. Run it on a thread so we can confirm the handoff
    // completed (via EOF on proxyUdsFd) before sending the TLS message to unblock it.
    Message msg = makeMessage(dbMsg, BSON("hello" << 1));
    StatusWith<Message> result = Status(ErrorCodes::InternalError, "not set");
    std::thread t([&] { result = session->sourceMessage(); });

    waitForSessionHandoff();

    ASSERT_EQ(session->getState(), HandoffSessionState::TLS);
    ASSERT_TRUE(session->isConnected());

    // Send a TLS message to unblock sourceMessage().
    clientSend(msg);
    t.join();

    ASSERT_OK(result.getStatus());
    ASSERT_EQ(result.getValue().size(), msg.size());
    ASSERT_EQ(memcmp(result.getValue().buf(), msg.buf(), msg.size()), 0);
    ASSERT_EQ(session->getState(), HandoffSessionState::TLS);
    ASSERT_TRUE(session->isConnected());
}

/**
 * After a successful handoff, remote() and local() reflect the actual addresses of the client TLS
 * fd, not the proxy UDS addresses passed to the constructor.
 */
TEST_F(HandoffSessionHandoffTransitionTest, SuccessfulHandoffUpdatesEndpoints) {
    ASSERT_EQ(session->remote(), HostAndPort("proxy", 0));
    ASSERT_EQ(session->local(), HostAndPort("mongod", 0));

    Message handoffMsg = buildSessionHandoffMessage(serializedState);
    sendMessageWithFds(proxyUdsFd, handoffMsg, {proxyTlsFd});
    ::close(proxyTlsFd);
    proxyTlsFd = -1;

    StatusWith<Message> result = Status(ErrorCodes::InternalError, "not set");
    std::thread t([&] { result = session->sourceMessage(); });

    waitForSessionHandoff();

    clientSend(makeMessage(dbMsg, BSON("ping" << 1)));
    t.join();
    ASSERT_OK(result.getStatus());

    // The TLS socketpair is unnamed (AF_UNIX, not bound to a path), so getpeername/getsockname
    // return an implementation-defined address. Assert NE rather than exact values.
    ASSERT_NE(session->remote(), HostAndPort("proxy", 0));
    ASSERT_NE(session->local(), HostAndPort("mongod", 0));
}

/**
 * OP_HANDOFF with corrupt s2n state causes sourceMessage() to return InternalError. Both the
 * mongodUdsFd and the received client fd are closed. The session is left disconnected.
 */
TEST_F(HandoffSessionHandoffTransitionTest, FailedHandoffWithCorruptState) {
    // Use a pipe instead of proxyTlsFd to verify the session closes its received client fd on
    // failure. isConnected() only confirms mongodUdsFd was closed. A raw recv on clientTlsFd
    // would bypass the live s2n session that setUp established. A pipe has no such baggage.
    // When the session closes its dup of pipeWrite, pipeRead sees EOF.
    int pipeFds[2];
    ASSERT_EQ(::pipe(pipeFds), 0) << "pipe failed: " << errorMessage(lastSocketError());
    int pipeRead = pipeFds[0];
    int pipeWrite = pipeFds[1];

    std::vector<uint8_t> corruptState(serializedState.size(), 0xDE);
    Message handoffMsg = buildSessionHandoffMessage(corruptState);
    sendMessageWithFds(proxyUdsFd, handoffMsg, {pipeWrite});
    // Close our copy so the session's dup is the only remaining write-end reference. When the
    // session closes its dup on failure, the write end becomes fully closed and pipeRead sees
    // EOF. If we kept our copy open, pipeRead would never see EOF and the check below would hang.
    ::close(pipeWrite);

    auto result = session->sourceMessage();
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::InternalError);
    ASSERT_FALSE(session->isConnected());

    waitForSessionHandoff();

    // Verify that the session closed the received client fd on failure by checking for EOF
    // on the pipe read end. Our copy of pipeWrite is already closed, so EOF means the
    // session's dup is gone too.
    int flags = ::fcntl(pipeRead, F_GETFL, 0);
    ASSERT_EQ(::fcntl(pipeRead, F_SETFL, flags | O_NONBLOCK), 0);
    char buf;
    ssize_t n = ::read(pipeRead, &buf, 1);
    ASSERT_EQ(n, 0) << "received fd was not closed by the session";
    ::close(pipeRead);
}

/**
 * A timeout set before handoff is propagated to the client TLS fd during handoff and is
 * respected by post-handoff sourceMessage() calls.
 */
TEST_F(HandoffSessionHandoffTransitionTest, TimeoutSetBeforeHandoffIsRespectedAfterHandoff) {
    static constexpr Milliseconds timeout{50};
    session->setTimeout(timeout);

    Message handoffMsg = buildSessionHandoffMessage(serializedState);
    sendMessageWithFds(proxyUdsFd, handoffMsg, {proxyTlsFd});
    ::close(proxyTlsFd);
    proxyTlsFd = -1;

    StatusWith<Message> firstResult = Status(ErrorCodes::InternalError, "not set");
    std::thread t([&] { firstResult = session->sourceMessage(); });
    waitForSessionHandoff();
    clientSend(makeMessage(dbMsg, BSON("ping" << 1)));
    t.join();
    ASSERT_OK(firstResult.getStatus());

    // No data sent after handoff. The timeout set before handoff must still fire.
    ASSERT_EQ(session->sourceMessage().getStatus().code(), ErrorCodes::NetworkTimeout);
}

/**
 * Fixture for post-handoff tests. Inherits HandoffSessionTLSFixture and drives the handoff to
 * completion in setUp. Tests communicate with the session via clientConn.
 */
class HandoffSessionPostHandoffFixture : public HandoffSessionTLSFixture {
public:
    void setUp() override {
        HandoffSessionTLSFixture::setUp();

        // Send OP_HANDOFF to the session.
        Message handoffMsg = buildSessionHandoffMessage(serializedState);
        sendMessageWithFds(proxyUdsFd, handoffMsg, {proxyTlsFd});
        // The session receives a dup of proxyTlsFd via SCM_RIGHTS and uses it for TLS I/O.
        // Our copy is no longer needed.
        ::close(proxyTlsFd);
        proxyTlsFd = -1;

        // sourceMessage() processes the handoff inline, closes mongodUdsFd, then recurses and
        // blocks waiting for the first TLS message. Run it on a thread so we can confirm the
        // handoff completed (via EOF on proxyUdsFd) before sending the TLS message to unblock it.
        StatusWith<Message> result = Status(ErrorCodes::InternalError, "not set");
        std::thread t([&] { result = session->sourceMessage(); });

        waitForSessionHandoff();

        // Send a message to unblock sourceMessage(), then join.
        clientSend(makeMessage(dbMsg, BSON("ping" << 1)));
        t.join();
        ASSERT_OK(result.getStatus());
    }
};

//
// Post-handoff: message behavior in TLS mode.
//

class HandoffSessionPostHandoffMessageTest : public HandoffSessionPostHandoffFixture {};

/**
 * appendToBSON() includes the session id, TLS state, and updated remote/local addresses after the
 * handoff.
 */
TEST_F(HandoffSessionPostHandoffMessageTest, AppendToBSONIncludesSessionInfo) {
    BSONObjBuilder bb;
    session->appendToBSON(bb);
    auto obj = bb.obj();
    ASSERT_TRUE(obj.hasField("id"));
    ASSERT_EQ(obj.getStringField("state"), "tls");
    ASSERT_TRUE(obj.hasField("remote"));
    ASSERT_TRUE(obj.hasField("local"));
    ASSERT_NE(obj.getStringField("remote"), "proxy:0");
    ASSERT_NE(obj.getStringField("local"), "mongod:0");
}

/** sourceMessage() returns the message. */
TEST_F(HandoffSessionPostHandoffMessageTest, SourceMessageReturnsMessage) {
    Message msg = makeMessage(dbMsg, BSON("hello" << 1));
    std::thread sender([&] { clientSend(msg); });

    auto result = session->sourceMessage();
    sender.join();

    ASSERT_OK(result.getStatus());
    ASSERT_EQ(result.getValue().size(), msg.size());
    ASSERT_EQ(memcmp(result.getValue().buf(), msg.buf(), msg.size()), 0);
}

/** Several messages sent back-to-back are sourced in order. */
TEST_F(HandoffSessionPostHandoffMessageTest, SourceMessagePreservesOrder) {
    std::vector<Message> sent;
    for (int i = 0; i < 3; ++i) {
        sent.push_back(makeMessage(dbMsg, BSON("n" << i)));
    }

    std::thread sender([&] {
        for (const auto& m : sent)
            clientSend(m);
    });

    for (const auto& m : sent) {
        auto result = session->sourceMessage();
        ASSERT_OK(result.getStatus());
        ASSERT_EQ(result.getValue().size(), m.size());
        ASSERT_EQ(memcmp(result.getValue().buf(), m.buf(), m.size()), 0);
    }
    sender.join();
}

/** sourceMessage() succeeds on a message with no body beyond the header. */
TEST_F(HandoffSessionPostHandoffMessageTest, SourceMessageWithNoBody) {
    MSGHEADER::Value header;
    MsgData::View view(reinterpret_cast<char*>(&header));
    view.setLen(sizeof(MSGHEADER::Value));
    view.setId(nextMessageId());
    view.setResponseToMsgId(0);
    view.setOperation(dbMsg);

    auto buf = SharedBuffer::allocate(sizeof(MSGHEADER::Value));
    memcpy(buf.get(), &header, sizeof(MSGHEADER::Value));
    Message msg(std::move(buf));

    std::thread sender([&] { clientSend(msg); });
    auto result = session->sourceMessage();
    sender.join();

    ASSERT_OK(result.getStatus());
    ASSERT_EQ(result.getValue().size(), static_cast<int>(sizeof(MSGHEADER::Value)));
    ASSERT_EQ(memcmp(result.getValue().buf(), &header, sizeof(MSGHEADER::Value)), 0);
}

/** sourceMessage() reassembles a message delivered in small chunks. */
TEST_F(HandoffSessionPostHandoffMessageTest, SourceMessageReassemblesFragmentedMessage) {
    Message msg = makeMessage(dbMsg, BSON("payload" << std::string(64, 'x')));
    const int totalSize = msg.size();

    std::thread sender([&] {
        constexpr size_t kChunk = 7;
        size_t off = 0;
        while (off < static_cast<size_t>(totalSize)) {
            size_t n = std::min(kChunk, static_cast<size_t>(totalSize) - off);
            clientSend(msg.buf() + off, n);
            off += n;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    auto result = session->sourceMessage();
    sender.join();

    ASSERT_OK(result.getStatus());
    ASSERT_EQ(result.getValue().size(), totalSize);
    ASSERT_EQ(memcmp(result.getValue().buf(), msg.buf(), totalSize), 0);
}

/** sinkMessage() delivers a message to the peer. */
TEST_F(HandoffSessionPostHandoffMessageTest, SinkMessageDeliversMessage) {
    Message msg = makeMessage(dbMsg, BSON("reply" << 1));
    int msgSize = msg.size();
    std::vector<char> original(msg.buf(), msg.buf() + msgSize);
    ASSERT_OK(session->sinkMessage(std::move(msg)));

    Message received = clientReceive(msgSize);
    ASSERT_EQ(received.size(), msgSize);
    ASSERT_EQ(memcmp(received.buf(), original.data(), msgSize), 0);
}

/** sourceMessage() rejects a header whose length is below the minimum. */
TEST_F(HandoffSessionPostHandoffMessageTest, SourceMessageRejectsTooSmallLength) {
    MSGHEADER::Value header;
    MsgData::View view(reinterpret_cast<char*>(&header));
    view.setLen(static_cast<int32_t>(sizeof(MSGHEADER::Value)) - 1);
    view.setId(nextMessageId());
    view.setResponseToMsgId(0);
    view.setOperation(dbMsg);

    std::thread sender(
        [&] { clientSend(reinterpret_cast<const char*>(&header), sizeof(MSGHEADER::Value)); });
    auto result = session->sourceMessage();
    sender.join();

    ASSERT_EQ(result.getStatus().code(), ErrorCodes::ProtocolError);
}

/** sourceMessage() rejects a header whose length exceeds MaxMessageSizeBytes. */
TEST_F(HandoffSessionPostHandoffMessageTest, SourceMessageRejectsTooLargeLength) {
    MSGHEADER::Value header;
    MsgData::View view(reinterpret_cast<char*>(&header));
    view.setLen(static_cast<int32_t>(MaxMessageSizeBytes) + 1);
    view.setId(nextMessageId());
    view.setResponseToMsgId(0);
    view.setOperation(dbMsg);

    std::thread sender(
        [&] { clientSend(reinterpret_cast<const char*>(&header), sizeof(MSGHEADER::Value)); });
    auto result = session->sourceMessage();
    sender.join();

    ASSERT_EQ(result.getStatus().code(), ErrorCodes::ProtocolError);
}

/** sourceMessage() fails when the peer closes the connection. */
TEST_F(HandoffSessionPostHandoffMessageTest, SourceMessageFailsOnPeerDisconnect) {
    ::close(clientTlsFd);
    clientTlsFd = -1;

    ASSERT_EQ(session->sourceMessage().getStatus().code(), ErrorCodes::SocketException);
    ASSERT_FALSE(session->isConnected());
}

/** sinkMessage() fails when the peer closes the connection. */
TEST_F(HandoffSessionPostHandoffMessageTest, SinkMessageFailsOnPeerDisconnect) {
    ::close(clientTlsFd);
    clientTlsFd = -1;

    ASSERT_EQ(session->sinkMessage(makeMessage(dbMsg, BSON("ping" << 1))).code(),
              ErrorCodes::SocketException);
    ASSERT_FALSE(session->isConnected());
}

//
// Post-handoff: timeout behavior in TLS mode.
//

class HandoffSessionPostHandoffTimeoutTest : public HandoffSessionPostHandoffFixture {
protected:
    static constexpr Milliseconds kTestTimeout{50};
};

/** waitForData() reports NetworkTimeout when no data arrives. */
TEST_F(HandoffSessionPostHandoffTimeoutTest, WaitForDataTimesOut) {
    session->setTimeout(kTestTimeout);

    // The peer is connected but sends no data.
    ASSERT_EQ(session->waitForData().code(), ErrorCodes::NetworkTimeout);
}

/** sourceMessage() times out when no data arrives. */
TEST_F(HandoffSessionPostHandoffTimeoutTest, SourceMessageRespectsTimeout) {
    session->setTimeout(kTestTimeout);
    ASSERT_EQ(session->sourceMessage().getStatus().code(), ErrorCodes::NetworkTimeout);
}

/** sourceMessage() times out when the header arrives but the body is withheld. */
TEST_F(HandoffSessionPostHandoffTimeoutTest, SourceMessageRespectsTimeoutAfterHeader) {
    session->setTimeout(kTestTimeout);

    Message msg = makeMessage(dbMsg, BSON("ping" << 1));
    std::thread sender([&] { clientSend(msg.buf(), sizeof(MSGHEADER::Value)); });

    auto result = session->sourceMessage();
    sender.join();

    ASSERT_EQ(result.getStatus().code(), ErrorCodes::NetworkTimeout);
}

/**
 * Clearing the timeout with setTimeout(boost::none) restores blocking behavior. A message that
 * arrives after a short delay is received successfully rather than rejected as timed out.
 */
TEST_F(HandoffSessionPostHandoffTimeoutTest, ClearingTimeoutRestoresBlockingBehavior) {
    session->setTimeout(kTestTimeout);
    session->setTimeout(boost::none);

    Message msg = makeMessage(dbMsg, BSON("hello" << 1));
    std::thread sender([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        clientSend(msg);
    });

    auto result = session->sourceMessage();
    sender.join();

    ASSERT_OK(result.getStatus());
    ASSERT_EQ(result.getValue().size(), msg.size());
    ASSERT_EQ(memcmp(result.getValue().buf(), msg.buf(), msg.size()), 0);
}

//
// Post-handoff: session termination in TLS mode.
//

class HandoffSessionPostHandoffEndSessionTest : public HandoffSessionPostHandoffFixture {};

/** end() disconnects the session and closes the client TLS fd. */
TEST_F(HandoffSessionPostHandoffEndSessionTest, EndDisconnectsSessionAndClosesClientTlsFd) {
    session->end();
    ASSERT_FALSE(session->isConnected());

    // Verify that the session closed the received client fd by draining clientTlsFd until EOF.
    char buf[256];
    ssize_t n;
    do {
        n = ::recv(clientTlsFd, buf, sizeof(buf), 0);
    } while (n > 0);
    ASSERT_EQ(n, 0) << "client TLS fd was not closed by end()";
}

/** end() is idempotent. */
TEST_F(HandoffSessionPostHandoffEndSessionTest, EndIsIdempotent) {
    session->end();
    session->end();
    ASSERT_FALSE(session->isConnected());
}

/** sourceMessage() after end() fails. */
TEST_F(HandoffSessionPostHandoffEndSessionTest, SourceMessageAfterEndFails) {
    session->end();
    ASSERT_EQ(session->sourceMessage().getStatus().code(), ErrorCodes::SocketException);
}

/** sinkMessage() after end() fails. */
TEST_F(HandoffSessionPostHandoffEndSessionTest, SinkMessageAfterEndFails) {
    session->end();
    ASSERT_EQ(session->sinkMessage(makeMessage(dbMsg, BSON("a" << 1))).code(),
              ErrorCodes::SocketException);
}

/** waitForData() after end() fails. */
TEST_F(HandoffSessionPostHandoffEndSessionTest, WaitForDataAfterEndFails) {
    session->end();
    ASSERT_EQ(session->waitForData().code(), ErrorCodes::SocketException);
}

/**
 * end() called concurrently with a blocked waitForData() must wake the blocked poll()
 * immediately. Without ::shutdown(SHUT_RDWR) before ::close(), Linux does not unblock a
 * concurrent poll() on close() alone.
 */
TEST_F(HandoffSessionPostHandoffEndSessionTest, EndUnblocksBlockedWaitForData) {
    auto fut = std::async(std::launch::async, [&] { return session->waitForData(); });

    // The sleep gives waitForData() time to enter poll() before end() closes _fd. On a slow
    // machine, end() may win the race first and the poll() wakeup path goes untested.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    session->end();
    ASSERT_EQ(fut.wait_for(std::chrono::seconds(5)), std::future_status::ready)
        << "end() failed to unblock waitForData()";
    ASSERT_EQ(fut.get().code(), ErrorCodes::SocketException);
}

//
// Proxy header: PROXY v2 header parsing, TLV application, and post-handoff persistence.
//

class HandoffSessionProxyHeaderTest : public HandoffSessionTLSFixture {
protected:
    void driveHandoff() {
        Message handoffMsg = buildSessionHandoffMessage(serializedState);
        sendMessageWithFds(proxyUdsFd, handoffMsg, {proxyTlsFd});
        ::close(proxyTlsFd);
        proxyTlsFd = -1;

        StatusWith<Message> result = Status(ErrorCodes::InternalError, "not set");
        std::thread t([&] { result = session->sourceMessage(); });

        waitForSessionHandoff();
        clientSend(makeMessage(dbMsg, BSON("ping" << 1)));
        t.join();
        ASSERT_OK(result.getStatus());
        ASSERT_EQ(session->getState(), HandoffSessionState::TLS);
    }

protected:
    static constexpr uint16_t kTestClientPort = 12345;
    static constexpr char kTestClientAddr[] = "127.0.0.1";
    static constexpr char kTestSniName[] = "test.example.com";

    static void appendU16BE(std::vector<uint8_t>& buf, uint16_t v) {
        buf.push_back(static_cast<uint8_t>(v >> 8));
        buf.push_back(static_cast<uint8_t>(v & 0xFF));
    }

    static void appendTLV(std::vector<uint8_t>& buf,
                          uint8_t type,
                          const uint8_t* val,
                          uint16_t len) {
        buf.push_back(type);
        appendU16BE(buf, len);
        buf.insert(buf.end(), val, val + len);
    }

    static struct sockaddr_storage makeTestIpv4Addr() {
        struct sockaddr_storage addr{};
        auto* sin = reinterpret_cast<struct sockaddr_in*>(&addr);
        sin->sin_family = AF_INET;
        sin->sin_addr.s_addr = htonl(0x7F000001);  // kTestClientAddr
        sin->sin_port = htons(kTestClientPort);
        return addr;
    }

    /**
     * Builds a PROXY v2 binary header. sniName, subjectDN, and rolesBytes/rolesLen are optional
     * — pass nullptr/0 to omit each. Always includes at least a NOOP TLV since the parser requires
     * a non-empty TLV block on the proxy unix socket (enforced by the parser).
     */
    static std::vector<uint8_t> buildProxyHeader(const char* sniName,
                                                 const char* subjectDN,
                                                 const uint8_t* rolesBytes,
                                                 size_t rolesLen) {
        static constexpr uint8_t kSignature[12] = {
            0x0D, 0x0A, 0x0D, 0x0A, 0x00, 0x0D, 0x0A, 0x51, 0x55, 0x49, 0x54, 0x0A};

        auto src = makeTestIpv4Addr();
        const auto* s = reinterpret_cast<const struct sockaddr_in*>(&src);
        const uint8_t* ip = reinterpret_cast<const uint8_t*>(&s->sin_addr.s_addr);

        std::vector<uint8_t> addrBlock;
        addrBlock.insert(addrBlock.end(), ip, ip + 4);
        addrBlock.insert(addrBlock.end(), 4, 0);  // dst addr (zeros)
        appendU16BE(addrBlock, ntohs(s->sin_port));
        appendU16BE(addrBlock, 0);  // dst port (zero)

        std::vector<uint8_t> tlvs;
        if (sniName && *sniName) {
            appendTLV(tlvs,
                      0x02,
                      reinterpret_cast<const uint8_t*>(sniName),
                      static_cast<uint16_t>(strlen(sniName)));
        }
        if ((subjectDN && *subjectDN) || (rolesBytes && rolesLen > 0)) {
            std::vector<uint8_t> subTlvs;
            if (subjectDN && *subjectDN) {
                appendTLV(subTlvs,
                          0xE0,
                          reinterpret_cast<const uint8_t*>(subjectDN),
                          static_cast<uint16_t>(strlen(subjectDN)));
            }
            if (rolesBytes && rolesLen > 0) {
                appendTLV(subTlvs, 0xE1, rolesBytes, static_cast<uint16_t>(rolesLen));
            }
            // SSL TLV value: client_flags(1) + verify(4 big-endian, 0=success) + sub-TLVs.
            std::vector<uint8_t> sslVal = {0x07, 0x00, 0x00, 0x00, 0x00};
            sslVal.insert(sslVal.end(), subTlvs.begin(), subTlvs.end());
            appendTLV(tlvs, 0x20, sslVal.data(), static_cast<uint16_t>(sslVal.size()));
        }

        // The parser requires a non-empty TLV block on the proxy unix socket.
        if (tlvs.empty()) {
            static constexpr uint8_t kNoop[] = {0x04, 0x00, 0x01, 0x00};
            tlvs.insert(tlvs.end(), kNoop, kNoop + sizeof(kNoop));
        }

        uint16_t addrLen = static_cast<uint16_t>(addrBlock.size() + tlvs.size());
        std::vector<uint8_t> header;
        header.insert(header.end(), kSignature, kSignature + 12);
        header.push_back(0x21);  // ver=2, cmd=PROXY
        header.push_back(0x11);  // TCP over IPv4
        appendU16BE(header, addrLen);
        header.insert(header.end(), addrBlock.begin(), addrBlock.end());
        header.insert(header.end(), tlvs.begin(), tlvs.end());
        return header;
    }
};

/**
 * A stream that does not begin with the PROXY v2 signature prefix is rejected with ProtocolError.
 */
TEST_F(HandoffSessionProxyHeaderTest, RejectsNonProxyStream) {
    std::vector<char> notProxyHeader(16, 'A');
    writeAll(proxyUdsFd, notProxyHeader.data(), notProxyHeader.size());

    ASSERT_THROWS_CODE(session->prelude(), DBException, ErrorCodes::ProtocolError);
}

/** A PROXY v1 header is rejected with a clear error indicating only v2 is supported. */
TEST_F(HandoffSessionProxyHeaderTest, RejectsProxyV1Header) {
    // PROXY v1 header starts with "PROXY TCP4 " followed by addresses.
    std::string v1Header = "PROXY TCP4 127.0.0.1 127.0.0.1 12345 27017\r\n";
    writeAll(proxyUdsFd, v1Header.data(), v1Header.size());

    ASSERT_THROWS_CODE_AND_WHAT(
        session->prelude(),
        DBException,
        ErrorCodes::ProtocolError,
        "Proxy UDS connection sent a PROXY v1 header. Only PROXY v2 is supported");
}

/**
 * A declared addr_len that exceeds proxyUnixSocketMaximumHeaderSize is rejected with
 * ProtocolError before any address bytes are read.
 */
TEST_F(HandoffSessionProxyHeaderTest, RejectsOversizedProxyHeader) {
    // Build a 16-byte fixed header with the correct v2 signature prefix (bytes 0-3) and addr_len
    // (bytes 14-15) set to 0xFFFF = 65535, far above the 4096-byte proxyUnixSocketMaximumHeaderSize
    // default. All other bytes are zeroed.
    std::vector<char> header(16, '\x00');
    header[0] = '\x0D';
    header[1] = '\x0A';
    header[2] = '\x0D';
    header[3] = '\x0A';
    header[14] = '\xFF';
    header[15] = '\xFF';
    writeAll(proxyUdsFd, header.data(), header.size());

    ASSERT_THROWS_CODE(session->prelude(), DBException, ErrorCodes::ProtocolError);
}

/**
 * A correct 4-byte signature prefix followed by an invalid full header signature is rejected with
 * FailedToParse.
 */
TEST_F(HandoffSessionProxyHeaderTest, RejectsUnparseableProxyHeader) {
    // Bytes 4-13, which should contain the rest of the 12-byte signature, are zeroed, so
    // parseProxyProtocolHeader rejects the full header. addr_len (bytes 14-15) is 0.
    std::vector<char> header(16, '\x00');
    header[0] = '\x0D';
    header[1] = '\x0A';
    header[2] = '\x0D';
    header[3] = '\x0A';
    writeAll(proxyUdsFd, header.data(), header.size());

    ASSERT_THROWS_CODE(session->prelude(), DBException, ErrorCodes::FailedToParse);
}

/** No SNI or client-cert TLVs. SSLPeerInfo is null both before and after handoff. */
TEST_F(HandoffSessionProxyHeaderTest, ProxyHeaderNoSNIOrCert) {
    auto header = buildProxyHeader(
        /*sniName=*/nullptr, /*subjectDN=*/nullptr, /*rolesBytes=*/nullptr, /*rolesLen=*/0);
    writeAll(proxyUdsFd, reinterpret_cast<const char*>(header.data()), header.size());

    ASSERT_NO_THROW(session->prelude());
    ASSERT_FALSE(SSLPeerInfo::forSession(std::shared_ptr<transport::Session>(session)));

    driveHandoff();
    ASSERT_FALSE(SSLPeerInfo::forSession(std::shared_ptr<transport::Session>(session)));
}

/**
 * SNI TLV (AUTHORITY) present, no client cert. SSLPeerInfo has sniName and empty subject, both
 * before and after handoff.
 */
TEST_F(HandoffSessionProxyHeaderTest, ProxyHeaderWithSNINoCert) {
    auto header = buildProxyHeader(/*sniName=*/kTestSniName,
                                   /*subjectDN=*/nullptr,
                                   /*rolesBytes=*/nullptr,
                                   /*rolesLen=*/0);
    writeAll(proxyUdsFd, reinterpret_cast<const char*>(header.data()), header.size());

    auto validatePeerInfo = [&] {
        auto peerInfo = SSLPeerInfo::forSession(std::shared_ptr<transport::Session>(session));
        ASSERT_TRUE(peerInfo);
        ASSERT_TRUE(peerInfo->sniName().has_value());
        ASSERT_EQ(peerInfo->sniName().value(), kTestSniName);
        ASSERT_TRUE(peerInfo->subjectName().empty());
        ASSERT_TRUE(peerInfo->roles().empty());
    };

    ASSERT_NO_THROW(session->prelude());
    validatePeerInfo();

    driveHandoff();
    validatePeerInfo();
}

/**
 * SNI TLV and SSL TLV block (DN + roles sub-TLVs). SSLPeerInfo has sniName, subjectName, and
 * roles, both before and after handoff.
 */
TEST_F(HandoffSessionProxyHeaderTest, ProxyHeaderWithSNIAndCert) {
    // DER-encoded role data for a single role: role="role_name", db="Third field".
    static constexpr uint8_t roleDer[] = {
        0x31, 0x1a, 0x30, 0x18, 0x0c, 0x09, 0x72, 0x6f, 0x6c, 0x65, 0x5f, 0x6e, 0x61, 0x6d,
        0x65, 0x0c, 0x0b, 0x54, 0x68, 0x69, 0x72, 0x64, 0x20, 0x66, 0x69, 0x65, 0x6c, 0x64};
    auto header = buildProxyHeader(/*sniName=*/kTestSniName,
                                   /*subjectDN=*/"CN=customClient,O=customOrg",
                                   /*rolesBytes=*/roleDer,
                                   /*rolesLen=*/sizeof(roleDer));
    writeAll(proxyUdsFd, reinterpret_cast<const char*>(header.data()), header.size());

    auto validatePeerInfo = [&] {
        auto peerInfo = SSLPeerInfo::forSession(std::shared_ptr<transport::Session>(session));
        ASSERT_TRUE(peerInfo);
        ASSERT_EQ(peerInfo->sniName().value(), kTestSniName);
        auto swCN = peerInfo->subjectName().getOID("2.5.4.3");
        ASSERT_OK(swCN.getStatus());
        ASSERT_EQ(swCN.getValue(), "customClient");
        auto swO = peerInfo->subjectName().getOID("2.5.4.10");
        ASSERT_OK(swO.getStatus());
        ASSERT_EQ(swO.getValue(), "customOrg");
        ASSERT_EQ(peerInfo->roles().size(), 1u);
        ASSERT_EQ(peerInfo->roles().begin()->getRole(), "role_name");
        ASSERT_EQ(peerInfo->roles().begin()->getDB(), "Third field");
    };

    ASSERT_NO_THROW(session->prelude());
    validatePeerInfo();

    driveHandoff();
    validatePeerInfo();
}

/**
 * A PROXY v2 header delivered in two separate sends is reassembled correctly.
 * SSLPeerInfo is set as expected after the handoff.
 */
TEST_F(HandoffSessionProxyHeaderTest, ProxyHeaderArrivesFragmented) {
    auto header = buildProxyHeader(/*sniName=*/kTestSniName,
                                   /*subjectDN=*/nullptr,
                                   /*rolesBytes=*/nullptr,
                                   /*rolesLen=*/0);
    ASSERT_FALSE(header.empty());

    // Send the first quarter, pause, then send the rest.
    size_t firstChunk = header.size() / 4;
    writeAll(proxyUdsFd, reinterpret_cast<const char*>(header.data()), firstChunk);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    writeAll(proxyUdsFd,
             reinterpret_cast<const char*>(header.data() + firstChunk),
             header.size() - firstChunk);

    auto validatePeerInfo = [&] {
        auto peerInfo = SSLPeerInfo::forSession(std::shared_ptr<transport::Session>(session));
        ASSERT_TRUE(peerInfo);
        ASSERT_EQ(peerInfo->sniName().value(), kTestSniName);
    };

    ASSERT_NO_THROW(session->prelude());
    validatePeerInfo();

    driveHandoff();
    validatePeerInfo();
}

/**
 * A PROXY v2 header written immediately before a message: the header is consumed exactly,
 * with no header bytes leaking into the message stream.
 */
TEST_F(HandoffSessionProxyHeaderTest, ProxyHeaderBoundaryIsRespected) {
    auto header = buildProxyHeader(/*sniName=*/kTestSniName,
                                   /*subjectDN=*/nullptr,
                                   /*rolesBytes=*/nullptr,
                                   /*rolesLen=*/0);
    Message msg = makeMessage(dbMsg, BSON("ping" << 1));

    // Write both in one shot so they arrive together in the socket buffer.
    writeAll(proxyUdsFd, reinterpret_cast<const char*>(header.data()), header.size());
    writeAll(proxyUdsFd, msg.buf(), msg.size());

    ASSERT_NO_THROW(session->prelude());
    auto peerInfo = SSLPeerInfo::forSession(std::shared_ptr<transport::Session>(session));
    ASSERT_TRUE(peerInfo);
    ASSERT_EQ(peerInfo->sniName().value(), kTestSniName);

    auto result = session->sourceMessage();
    ASSERT_OK(result.getStatus());
    ASSERT_EQ(result.getValue().size(), msg.size());
    ASSERT_EQ(memcmp(result.getValue().buf(), msg.buf(), msg.size()), 0);
}

/** A peer that closes before sending anything causes prelude() to throw SocketException. */
TEST_F(HandoffSessionProxyHeaderTest, FailsOnPeerClose) {
    ::close(proxyUdsFd);
    proxyUdsFd = -1;

    ASSERT_THROWS_CODE(session->prelude(), DBException, ErrorCodes::SocketException);
}

/**
 * end() called while blocked waiting for the proxy header unblocks the read promptly.
 * Mirrors EndUnblocksBlockedWaitForData but for the proxy-header read path.
 */
TEST_F(HandoffSessionProxyHeaderTest, EndUnblocksProxyHeaderRead) {
    auto fut = std::async(std::launch::async, [&] {
        ASSERT_THROWS_CODE(session->prelude(), DBException, ErrorCodes::SocketException);
    });

    // Give prelude() time to enter the blocking recv() before end() closes the fd.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    session->end();
    ASSERT_EQ(fut.wait_for(std::chrono::seconds(5)), std::future_status::ready)
        << "end() failed to unblock proxy header read";
    fut.get();
}

/**
 * Peer sends a partial signature prefix (3 of 5 bytes) and then stalls. prelude() must report
 * NetworkTimeout, not SocketException. Contrast with ProxyProtocolTimeoutDuringAddressBlockRead,
 * where the peer sends enough to identify the protocol version but stalls on the address block.
 */
TEST_F(HandoffSessionProxyHeaderTest, ProxyProtocolTimeoutDuringPeek) {
    unittest::ServerParameterGuard timeoutParam{"proxyProtocolTimeoutSecs", 1};

    // Send only 3 bytes (fewer than kProxyPeekSize=5) then stall.
    static constexpr char header[] = {'\x0D', '\x0A', '\x0D'};
    writeAll(proxyUdsFd, header, sizeof(header));

    ASSERT_THROWS_CODE(session->prelude(), DBException, ErrorCodes::NetworkTimeout);
}

/**
 * Peer sends a complete fixed header declaring a 12-byte address block but never sends the
 * address bytes. prelude() must report NetworkTimeout. Contrast with
 * ProxyProtocolTimeoutDuringPeek, where the stall happens before the protocol version can even
 * be identified.
 */
TEST_F(HandoffSessionProxyHeaderTest, ProxyProtocolTimeoutDuringAddressBlockRead) {
    unittest::ServerParameterGuard timeoutParam{"proxyProtocolTimeoutSecs", 1};

    // Build only the 16-byte fixed header: correct v2 signature, addr_len = 12 (IPv4 addr block),
    // but never send the addr block so the read blocks until the proxy timeout fires.
    static constexpr uint8_t proxyV2Signature[12] = {
        0x0D, 0x0A, 0x0D, 0x0A, 0x00, 0x0D, 0x0A, 0x51, 0x55, 0x49, 0x54, 0x0A};
    std::vector<uint8_t> header;
    header.insert(header.end(), proxyV2Signature, proxyV2Signature + 12);
    header.push_back(0x21);  // ver=2, cmd=PROXY
    header.push_back(0x11);  // TCP over IPv4
    header.push_back(0x00);
    header.push_back(0x0C);  // addr_len = 12
    writeAll(proxyUdsFd, reinterpret_cast<const char*>(header.data()), header.size());

    ASSERT_THROWS_CODE(session->prelude(), DBException, ErrorCodes::NetworkTimeout);
}

/**
 * With proxyProtocolTimeoutSecs=0, a complete header already in the kernel buffer is parsed
 * successfully.
 */
TEST_F(HandoffSessionProxyHeaderTest, ZeroProxyProtocolTimeoutSucceedsIfHeaderAlreadyBuffered) {
    unittest::ServerParameterGuard timeoutParam{"proxyProtocolTimeoutSecs", 0};

    auto header = buildProxyHeader(nullptr, nullptr, nullptr, 0);
    writeAll(proxyUdsFd, reinterpret_cast<const char*>(header.data()), header.size());

    ASSERT_NO_THROW(session->prelude());
    ASSERT_TRUE(session->isConnected());
}

/**
 * With proxyProtocolTimeoutSecs=0, prelude() must return NetworkTimeout immediately when the
 * addr block is withheld instead of hang waiting for the full header.
 */
TEST_F(HandoffSessionProxyHeaderTest, ZeroProxyProtocolTimeoutFailsIfHeaderNotAlreadyBuffered) {
    unittest::ServerParameterGuard timeoutParam{"proxyProtocolTimeoutSecs", 0};

    // Send only the 16-byte fixed header with addr_len=12, but never send the addr block.
    static constexpr uint8_t proxyV2Signature[12] = {
        0x0D, 0x0A, 0x0D, 0x0A, 0x00, 0x0D, 0x0A, 0x51, 0x55, 0x49, 0x54, 0x0A};
    std::vector<uint8_t> header;
    header.insert(header.end(), proxyV2Signature, proxyV2Signature + 12);
    header.push_back(0x21);  // ver=2, cmd=PROXY
    header.push_back(0x11);  // TCP over IPv4
    header.push_back(0x00);
    header.push_back(0x0C);  // addr_len = 12
    writeAll(proxyUdsFd, reinterpret_cast<const char*>(header.data()), header.size());

    auto fut = std::async(std::launch::async, [&] {
        ASSERT_THROWS_CODE(session->prelude(), DBException, ErrorCodes::NetworkTimeout);
    });

    ASSERT_EQ(fut.wait_for(std::chrono::seconds(5)), std::future_status::ready)
        << "prelude() hung; SO_RCVTIMEO={0,0} may have disabled the recv timeout";
    fut.get();
}

/**
 * With proxyProtocolTimeoutSecs=0, prelude() returns NetworkTimeout immediately when only a
 * partial signature prefix is buffered and the sender stalls before completing the header.
 */
TEST_F(HandoffSessionProxyHeaderTest, ZeroProxyProtocolTimeoutFailsIfSignaturePrefixIsPartial) {
    unittest::ServerParameterGuard timeoutParam{"proxyProtocolTimeoutSecs", 0};

    static constexpr char header[] = {'\x0D', '\x0A', '\x0D'};
    writeAll(proxyUdsFd, header, sizeof(header));

    ASSERT_THROWS_CODE(session->prelude(), DBException, ErrorCodes::NetworkTimeout);
}

/**
 * With proxyProtocolTimeoutSecs=0, prelude() returns NetworkTimeout immediately when no data
 * is present. poll() with a 0ms timeout does not block, so the read expires on the first check.
 */
TEST_F(HandoffSessionProxyHeaderTest, ZeroProxyProtocolTimeoutFailsIfSocketIsEmpty) {
    unittest::ServerParameterGuard timeoutParam{"proxyProtocolTimeoutSecs", 0};

    ASSERT_THROWS_CODE(session->prelude(), DBException, ErrorCodes::NetworkTimeout);
}

/**
 * A session timeout set before prelude() must still be honoured after prelude() returns.
 */
TEST_F(HandoffSessionProxyHeaderTest, SessionTimeoutRestoredAfterPrelude) {
    // Make the proxy protocol timeout larger than the session timeout so the proxy header read
    // cannot time out before the session does, but small enough that wait_for() below detects
    // the bug promptly if the session timeout was not restored.
    constexpr int kProxyTimeoutSecs = 2;
    unittest::ServerParameterGuard proxyTimeout{"proxyProtocolTimeoutSecs", kProxyTimeoutSecs};
    Milliseconds sessionTimeout{100};
    session->setTimeout(sessionTimeout);

    auto header = buildProxyHeader(
        /*sniName=*/nullptr, /*subjectDN=*/nullptr, /*rolesBytes=*/nullptr, /*rolesLen=*/0);
    writeAll(proxyUdsFd, reinterpret_cast<const char*>(header.data()), header.size());
    ASSERT_NO_THROW(session->prelude());

    // Send only the message header so sourceMessage() blocks waiting for the body. If the session
    // timeout was not restored after prelude(), sourceMessage() will block for up to
    // proxyProtocolTimeoutSecs rather than returning promptly.
    Message msg = makeMessage(dbMsg, BSON("ping" << 1));
    writeAll(proxyUdsFd, msg.buf(), sizeof(MSGHEADER::Value));

    auto fut = std::async(std::launch::async, [&] { return session->sourceMessage(); });
    ASSERT_EQ(fut.wait_for(std::chrono::seconds(kProxyTimeoutSecs + 1)), std::future_status::ready)
        << "sourceMessage() did not return. Session timeout was not restored after prelude()";
    ASSERT_EQ(fut.get().getStatus().code(), ErrorCodes::NetworkTimeout);
}

/**
 * Before prelude(), getSourceRemoteEndpoint() returns the UDS proxy address from the constructor.
 * After prelude(), it returns the proxied client IP from the PROXY header, and handoff does not
 * change it.
 */
TEST_F(HandoffSessionProxyHeaderTest, GetSourceRemoteEndpointReturnsProxiedAddress) {
    ASSERT_EQ(session->getSourceRemoteEndpoint(), HostAndPort("proxy", 0));

    auto header = buildProxyHeader(
        /*sniName=*/nullptr, /*subjectDN=*/nullptr, /*rolesBytes=*/nullptr, /*rolesLen=*/0);
    writeAll(proxyUdsFd, reinterpret_cast<const char*>(header.data()), header.size());
    ASSERT_NO_THROW(session->prelude());

    ASSERT_EQ(session->getSourceRemoteEndpoint().host(), kTestClientAddr);
    ASSERT_EQ(session->getSourceRemoteEndpoint().port(), kTestClientPort);

    driveHandoff();

    ASSERT_EQ(session->getSourceRemoteEndpoint().host(), kTestClientAddr);
    ASSERT_EQ(session->getSourceRemoteEndpoint().port(), kTestClientPort);
}

/**
 * When the PROXY v2 header uses the LOCAL command (no address block), getSourceRemoteEndpoint()
 * falls back to remote() both before and after handoff.
 */
TEST_F(HandoffSessionProxyHeaderTest, GetSourceRemoteEndpointFallsBackToRemoteWhenNoAddressBlock) {
    // Build a LOCAL command header (ver=2, cmd=LOCAL, fam=UNSPEC) with no address block.
    // Includes a NOOP TLV to satisfy MongoDB's requirement of a non-empty TLV block.
    static constexpr uint8_t kLocalHeader[] = {
        // 12-byte signature
        0x0D, 0x0A, 0x0D, 0x0A, 0x00, 0x0D, 0x0A, 0x51, 0x55, 0x49, 0x54, 0x0A,
        0x20,                    // ver=2, cmd=LOCAL
        0x00,                    // fam=UNSPEC
        0x00, 0x04,              // addr_len=4 (just the NOOP TLV below)
        0x04, 0x00, 0x01, 0x00,  // NOOP TLV
    };
    writeAll(proxyUdsFd, reinterpret_cast<const char*>(kLocalHeader), sizeof(kLocalHeader));
    ASSERT_NO_THROW(session->prelude());

    ASSERT_EQ(session->getSourceRemoteEndpoint(), session->remote());

    driveHandoff();

    ASSERT_EQ(session->getSourceRemoteEndpoint(), session->remote());
}

/**
 * Before prelude(), getAuthEnvironment() has a default-constructed RestrictionEnvironment with no
 * client source address. prelude() does not update it because _restrictionEnvironment requires
 * both the client source and the server's local address, and the local address comes from
 * getsockname on the client TLS fd which is only known after handoff. After handoff with
 * clientSourceAuthenticationRestrictionMode == "origin", getClientSource() reflects the proxied
 * client IP from the PROXY header.
 */
TEST_F(HandoffSessionProxyHeaderTest, GetAuthEnvironmentInOriginMode) {
    ASSERT_EQ(session->getAuthEnvironment().getClientSource().getAddr(), "(NONE)");

    auto header = buildProxyHeader(
        /*sniName=*/nullptr, /*subjectDN=*/nullptr, /*rolesBytes=*/nullptr, /*rolesLen=*/0);
    writeAll(proxyUdsFd, reinterpret_cast<const char*>(header.data()), header.size());
    ASSERT_NO_THROW(session->prelude());

    ASSERT_EQ(session->getAuthEnvironment().getClientSource().getAddr(), "(NONE)");

    unittest::ServerParameterGuard originMode{"clientSourceAuthenticationRestrictionMode",
                                              "origin"};
    driveHandoff();
    auto& src = session->getAuthEnvironment().getClientSource();
    ASSERT_EQ(src.getAddr(), kTestClientAddr);
    ASSERT_EQ(src.getPort(), kTestClientPort);
}

/**
 * With clientSourceAuthenticationRestrictionMode == "peer" (the default), getClientSource()
 * reflects the TLS socket peer address after handoff, not the proxied IP.
 */
TEST_F(HandoffSessionProxyHeaderTest, GetAuthEnvironmentInPeerMode) {
    ASSERT_EQ(session->getAuthEnvironment().getClientSource().getAddr(), "(NONE)");

    auto header = buildProxyHeader(
        /*sniName=*/nullptr, /*subjectDN=*/nullptr, /*rolesBytes=*/nullptr, /*rolesLen=*/0);
    writeAll(proxyUdsFd, reinterpret_cast<const char*>(header.data()), header.size());
    ASSERT_NO_THROW(session->prelude());

    ASSERT_EQ(session->getAuthEnvironment().getClientSource().getAddr(), "(NONE)");

    unittest::ServerParameterGuard peerMode{"clientSourceAuthenticationRestrictionMode", "peer"};
    driveHandoff();
    ASSERT_NE(session->getAuthEnvironment().getClientSource().getAddr(), kTestClientAddr);
}

/**
 * Exercises the complete session lifecycle in one continuous sequence: prelude() parses the PROXY
 * header, sourceMessage() returns a cleartext message, the handoff transitions the session to TLS,
 * and sourceMessage() returns a post-handoff TLS message. Verifies message content and session
 * state at each step.
 */
TEST_F(HandoffSessionProxyHeaderTest, FullSessionLifecycle) {
    // Pre-buffer the PROXY header and a cleartext ping so prelude() and the first
    // sourceMessage() complete synchronously on the main thread.
    auto header = buildProxyHeader(/*sniName=*/kTestSniName,
                                   /*subjectDN=*/nullptr,
                                   /*rolesBytes=*/nullptr,
                                   /*rolesLen=*/0);
    writeAll(proxyUdsFd, reinterpret_cast<const char*>(header.data()), header.size());

    BSONObj body1 = BSON("ping" << 1);
    BSONObj body2 = BSON("ping" << 2);

    Message msg1 = makeMessage(dbMsg, body1);
    writeAll(proxyUdsFd, msg1.buf(), msg1.size());

    auto validatePeerInfo = [&] {
        auto peerInfo = SSLPeerInfo::forSession(std::shared_ptr<transport::Session>(session));
        ASSERT_TRUE(peerInfo);
        ASSERT_EQ(peerInfo->sniName().value(), kTestSniName);
    };

    ASSERT_NO_THROW(session->prelude());
    ASSERT_EQ(session->getState(), HandoffSessionState::Cleartext);
    ASSERT_EQ(session->getSourceRemoteEndpoint().host(), kTestClientAddr);
    validatePeerInfo();

    auto result1 = session->sourceMessage();
    ASSERT_OK(result1.getStatus());
    ASSERT_BSONOBJ_EQ(BSONObj(result1.getValue().singleData().data()), body1);

    // Drive the handoff inline so the result for the message right after handoff is accessible
    // for content verification.
    Message handoffMsg = buildSessionHandoffMessage(serializedState);
    sendMessageWithFds(proxyUdsFd, handoffMsg, {proxyTlsFd});
    ::close(proxyTlsFd);
    proxyTlsFd = -1;

    StatusWith<Message> result2 = Status(ErrorCodes::InternalError, "not set");
    std::thread t([&] { result2 = session->sourceMessage(); });

    waitForSessionHandoff();
    clientSend(makeMessage(dbMsg, body2));
    t.join();

    ASSERT_OK(result2.getStatus());
    ASSERT_BSONOBJ_EQ(BSONObj(result2.getValue().singleData().data()), body2);
    ASSERT_EQ(session->getState(), HandoffSessionState::TLS);
    ASSERT_EQ(session->getSourceRemoteEndpoint().host(), kTestClientAddr);
    validatePeerInfo();
}

}  // namespace
}  // namespace mongo::transport::handoff_transport
