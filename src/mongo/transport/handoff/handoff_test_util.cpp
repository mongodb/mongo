// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/handoff/handoff_test_util.h"

#include "mongo/base/data_range.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/rpc/message.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/handoff/handoff_s2n_init.h"
#include "mongo/transport/handoff/session_handoff_message_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/shared_buffer.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>

namespace mongo::transport {
namespace {

void appendU16BE(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v >> 8));
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
}

void appendTLV(std::vector<uint8_t>& buf, uint8_t type, const uint8_t* val, uint16_t len) {
    buf.push_back(type);
    appendU16BE(buf, len);
    buf.insert(buf.end(), val, val + len);
}

struct sockaddr_storage makeTestIpv4Addr() {
    struct sockaddr_storage addr{};
    auto* sin = reinterpret_cast<struct sockaddr_in*>(&addr);
    sin->sin_family = AF_INET;
    const int rc = inet_pton(AF_INET, kTestClientAddr, &sin->sin_addr);
    invariant(rc == 1);
    sin->sin_port = htons(kTestClientPort);
    return addr;
}

constexpr char kServerCertKeyFile[] = "jstests/libs/server.pem";

}  // namespace

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

void writeAll(int fd, const char* buf, size_t len) {
    size_t written = 0;
    while (written < len) {
        ssize_t n;
        do {
            n = ::send(fd, buf + written, len - written, 0);
        } while (n == -1 && errno == EINTR);
        ASSERT_GT(n, 0) << "send failed: " << errorMessage(lastSocketError());
        written += n;
    }
}

iovec fromMessage(const Message& msg) {
    iovec result;
    result.iov_base = const_cast<char*>(msg.buf());
    result.iov_len = msg.size();
    return result;
}

void sendMessageWithFds(int udsFd, const Message& msg, std::initializer_list<int> fds) {
    iovec iov = fromMessage(msg);

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
        n = ::sendmsg(udsFd, &hdr, 0);
    } while (n < 0 && errno == EINTR);
    ASSERT_GT(n, 0) << "sendmsg failed: " << errorMessage(lastSocketError());
}

Message buildSessionHandoffMessage(const std::vector<uint8_t>& serializedState,
                                   const std::vector<uint8_t>& extraCleartext,
                                   SessionHandoffMessageOptions options) {
    SessionHandoffMessage handoffMsg;
    handoffMsg.setS2nState(ConstDataRange(serializedState.data(), serializedState.size()));
    if (!extraCleartext.empty() || !options.omitEmptyExtraCleartext) {
        handoffMsg.setExtraCleartext(ConstDataRange(extraCleartext.data(), extraCleartext.size()));
    }
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

std::vector<uint8_t> buildProxyHeader(const char* sniName,
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

HandoffTlsState::HandoffTlsState() {
    ASSERT_OK(s2nInitOnce());

    // Build the server config with TLS 1.2, serialization enabled, and a server cert.
    auto serverConfig = std::unique_ptr<s2n_config, decltype(&s2n_config_free)>(
        s2n_config_new_minimal(), &s2n_config_free);
    ASSERT_TRUE(serverConfig);
    ASSERT_EQ(s2n_config_set_serialization_version(serverConfig.get(), S2N_SERIALIZED_CONN_V1),
              S2N_SUCCESS);
    ASSERT_EQ(
        s2n_config_set_cipher_preferences(serverConfig.get(), "ELBSecurityPolicy-TLS-1-2-2017-01"),
        S2N_SUCCESS);
    auto certPem = readFile(kServerCertKeyFile);
    auto [certChainPem, keyPem] = splitCertAndKey(certPem);
    auto certChainAndKey =
        std::unique_ptr<s2n_cert_chain_and_key, decltype(&s2n_cert_chain_and_key_free)>(
            s2n_cert_chain_and_key_new(), &s2n_cert_chain_and_key_free);
    ASSERT_TRUE(certChainAndKey);
    ASSERT_EQ(s2n_cert_chain_and_key_load_pem(
                  certChainAndKey.get(), certChainPem.c_str(), keyPem.c_str()),
              S2N_SUCCESS);
    ASSERT_EQ(s2n_config_add_cert_chain_and_key_to_store(serverConfig.get(), certChainAndKey.get()),
              S2N_SUCCESS);
    ASSERT_EQ(s2n_config_disable_x509_verification(serverConfig.get()), S2N_SUCCESS);

    // Build the client config with TLS 1.2, serialization enabled, and no cert verification.
    auto clientConfig = std::unique_ptr<s2n_config, decltype(&s2n_config_free)>(
        s2n_config_new_minimal(), &s2n_config_free);
    ASSERT_TRUE(clientConfig);
    ASSERT_EQ(s2n_config_set_serialization_version(clientConfig.get(), S2N_SERIALIZED_CONN_V1),
              S2N_SUCCESS);
    ASSERT_EQ(
        s2n_config_set_cipher_preferences(clientConfig.get(), "ELBSecurityPolicy-TLS-1-2-2017-01"),
        S2N_SUCCESS);
    ASSERT_EQ(s2n_config_disable_x509_verification(clientConfig.get()), S2N_SUCCESS);

    // Build the deserialization config for the HandoffSession.
    auto deserConfig = std::unique_ptr<s2n_config, decltype(&s2n_config_free)>(
        s2n_config_new_minimal(), &s2n_config_free);
    ASSERT_TRUE(deserConfig);
    ASSERT_EQ(s2n_config_set_serialization_version(deserConfig.get(), S2N_SERIALIZED_CONN_V1),
              S2N_SUCCESS);

    // TLS socketpair (proxy <-> client):
    //   proxyTlsFd  is the proxy end. It is sent inside OP_HANDOFF via SCM_RIGHTS. On a
    //               successful handoff, the session uses it as its new active fd for TLS I/O.
    //   clientTlsFd is the client end. clientConn uses this fd to send and receive data.
    int tlsFds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, tlsFds), 0);
    ScopeGuard closeTlsFds([&] {
        ::close(tlsFds[0]);
        ::close(tlsFds[1]);
    });
    proxyTlsFd = tlsFds[0];
    clientTlsFd = tlsFds[1];

    // Create and connect s2n server and client connections.
    auto serverConn = std::unique_ptr<s2n_connection, decltype(&s2n_connection_free)>(
        s2n_connection_new(S2N_SERVER), &s2n_connection_free);
    ASSERT_TRUE(serverConn);
    ASSERT_EQ(s2n_connection_set_config(serverConn.get(), serverConfig.get()), S2N_SUCCESS);
    ASSERT_EQ(s2n_connection_set_fd(serverConn.get(), proxyTlsFd), S2N_SUCCESS);

    auto clientConn = std::unique_ptr<s2n_connection, decltype(&s2n_connection_free)>(
        s2n_connection_new(S2N_CLIENT), &s2n_connection_free);
    ASSERT_TRUE(clientConn);
    ASSERT_EQ(s2n_connection_set_config(clientConn.get(), clientConfig.get()), S2N_SUCCESS);
    ASSERT_EQ(s2n_connection_set_fd(clientConn.get(), clientTlsFd), S2N_SUCCESS);

    stdx::thread serverThread([&] {
        ASSERT_EQ(negotiateBlocking(serverConn.get()), S2N_SUCCESS)
            << s2n_strerror(s2n_errno, "EN");
    });
    ASSERT_EQ(negotiateBlocking(clientConn.get()), S2N_SUCCESS) << s2n_strerror(s2n_errno, "EN");
    serverThread.join();

    // All initialization succeeded. Transfer ownership to members.
    _serverConfig = serverConfig.release();
    _clientConfig = clientConfig.release();
    _deserConfig = deserConfig.release();
    _certChainAndKey = certChainAndKey.release();
    this->serverConn = serverConn.release();
    this->clientConn = clientConn.release();
    closeTlsFds.dismiss();
}

HandoffTlsState::~HandoffTlsState() {
    if (proxyTlsFd >= 0) {
        ::close(proxyTlsFd);
    }
    if (clientTlsFd >= 0) {
        ::close(clientTlsFd);
    }
    if (clientConn) {
        s2n_connection_free(clientConn);
    }
    if (serverConn) {
        s2n_connection_free(serverConn);
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

void HandoffTlsState::clientSend(const char* data, size_t len) {
    iovec bufs[1];
    bufs[0].iov_base = const_cast<char*>(data);
    bufs[0].iov_len = len;
    clientSend(bufs);
}

void HandoffTlsState::clientSend(std::span<const iovec> buffers) {
    size_t len = 0;
    for (const iovec& buf : buffers) {
        len += buf.iov_len;
    }

    size_t written = 0;
    while (written < len) {
        s2n_blocked_status blocked;
        ssize_t n =
            s2n_sendv_with_offset(clientConn, buffers.data(), buffers.size(), written, &blocked);
        uassert(ErrorCodes::InternalError, s2n_strerror(s2n_errno, "EN"), n > 0);
        written += n;
    }
}

Message HandoffTlsState::clientReceive(int expectedSize) {
    return _receive(clientConn, expectedSize);
}

Message HandoffTlsState::serverReceive(int expectedSize) {
    return _receive(serverConn, expectedSize);
}

void HandoffTlsState::serializeConnection() {
    // Serialize the server-side TLS state and remaining decrypted data.
    const uint32_t extraCount = s2n_peek(serverConn);
    extraCleartext.resize(extraCount);
    s2n_blocked_status blocked;
    ASSERT_EQ(s2n_recv(serverConn, extraCleartext.data(), extraCount, &blocked), extraCount);

    uint32_t serializedLen = 0;
    ASSERT_EQ(s2n_connection_serialization_length(serverConn, &serializedLen), S2N_SUCCESS);
    ASSERT_GT(serializedLen, 0u);
    serializedState.resize(serializedLen);
    ASSERT_EQ(s2n_connection_serialize(serverConn, serializedState.data(), serializedLen),
              S2N_SUCCESS);
}

std::string HandoffTlsState::readFile(const std::string& path) {
    std::ifstream file(path);
    ASSERT_TRUE(file.good()) << "Failed to open file: " << path;
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

std::pair<std::string, std::string> HandoffTlsState::splitCertAndKey(const std::string& combined) {
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

int HandoffTlsState::negotiateBlocking(struct s2n_connection* conn) {
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

Message HandoffTlsState::_receive(s2n_connection* conn, int expectedSize) {
    auto buf = SharedBuffer::allocate(expectedSize);
    int totalRead = 0;
    while (totalRead < expectedSize) {
        s2n_blocked_status blocked;
        ssize_t n = s2n_recv(conn, buf.get() + totalRead, expectedSize - totalRead, &blocked);
        uassert(ErrorCodes::InternalError, s2n_strerror(s2n_errno, "EN"), n != S2N_FAILURE);
        uassert(ErrorCodes::InternalError, "client connection closed unexpectedly", n > 0);
        totalRead += n;
    }
    return Message(std::move(buf));
}

}  // namespace mongo::transport
