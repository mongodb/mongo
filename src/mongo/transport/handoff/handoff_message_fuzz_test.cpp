// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/initializer.h"
#include "mongo/rpc/message.h"
#include "mongo/transport/handoff/handoff_session.h"
#include "mongo/transport/handoff/handoff_test_util.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/shared_buffer.h"

#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <variant>
#include <vector>

#include <unistd.h>

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"
#include <sys/socket.h>

namespace mongo::transport {
namespace {

/**
 * FuzzTest's gtest main does not run MongoDB's MONGO_INITIALIZERs, and does not ignore SIGPIPE (as
 * mongod/s do). Register a gtest Environment so both are set up before any test body.
 */
class MongoInfrastructureEnv : public ::testing::Environment {
public:
    void SetUp() override {
        runGlobalInitializersOrDie({});
        signal(SIGPIPE, SIG_IGN);
    }
};

[[maybe_unused]] const auto kRegisterMongoInfraEnv =
    ::testing::AddGlobalTestEnvironment(new MongoInfrastructureEnv);

/**
 * Builds a wire protocol message wrapping a raw body. Unlike `makeMessage`, the body need not be
 * valid BSON, since the session returns the message as-is without parsing it.
 */
Message makeRawMessage(NetworkOp op, const std::vector<int>& bytes) {
    const int bodyLen = static_cast<int>(bytes.size());
    const int totalLen = sizeof(MSGHEADER::Value) + bodyLen;
    auto buf = SharedBuffer::allocate(totalLen);

    MsgData::View view(buf.get());
    view.setLen(totalLen);
    view.setId(nextMessageId());
    view.setResponseToMsgId(0);
    view.setOperation(op);
    for (int i = 0; i < bodyLen; ++i) {
        view.data()[i] = static_cast<char>(bytes[i]);
    }

    return Message(std::move(buf));
}

/**
 * Returns the raw bytes of a message as ints, for use as fuzzer input (e.g. `extraCleartext`).
 */
std::vector<int> messageBytes(const Message& msg) {
    std::vector<int> bytes(msg.size());
    for (size_t i = 0; i < msg.size(); ++i) {
        bytes[i] = static_cast<unsigned char>(msg.buf()[i]);
    }
    return bytes;
}

/**
 * If `bytes` begins with a complete wire message, returns its length. Otherwise returns -1. A
 * message is complete when the header's declared length is within bounds and the full body is
 * present, which is exactly what sourceMessage() needs in order to return it from the buffered
 * extra cleartext rather than falling through to the socket.
 */
int completeMessageLength(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < sizeof(MSGHEADER::Value)) {
        return -1;
    }
    MSGHEADER::ConstView headerView(reinterpret_cast<const char*>(bytes.data()));
    const int32_t msgLen = headerView.getMessageLength();
    if (msgLen < static_cast<int32_t>(sizeof(MSGHEADER::Value)) ||
        msgLen > static_cast<int32_t>(MaxMessageSizeBytes)) {
        return -1;
    }
    if (bytes.size() < static_cast<size_t>(msgLen)) {
        return -1;
    }
    return msgLen;
}

/** A valid s2n state, serialized by the fixture. */
struct ValidStateSpec {};

/** A raw s2n state, sent verbatim as bytes. */
struct RawStateSpec {
    std::vector<int> raw;
};

/**
 * The serialized s2n state carried by the OP_HANDOFF message, as one of two shapes. The variant
 * keeps the domain from generating raw bytes when the valid state is used.
 */
using S2nStateSpec = std::variant<ValidStateSpec, RawStateSpec>;

/**
 * A fuzz input, holding the pre-handoff cleartext message bodies, the s2n state carried by the
 * OP_HANDOFF message, and any extra cleartext carried alongside it.
 *
 * When the s2n state is valid, the handoff runs to completion but no TLS records cross the
 * handed-off socket, whose peer is half-closed. The post-handoff read uses the buffered
 * `extraCleartext` first, so a complete message is returned and an incomplete one exhausts the
 * buffer and reads the half-closed socket, which returns EOF and fails the read. The handoff has
 * already completed, so the session stays in TLS state either way. An invalid s2n state fails the
 * handoff and leaves the session in Cleartext.
 */
struct HandoffSpec {
    // The pre-handoff cleartext message bodies, at least one.
    std::vector<std::vector<int>> preHandoffMessageBodies;
    S2nStateSpec s2nState;
    std::vector<int> extraCleartext;
};

/**
 * Builds seed HandoffSpec cases, giving the fuzzer valid and invalid examples to start from.
 */
std::vector<HandoffSpec> buildHandoffSeeds() {
    return {
        // Valid. No extra cleartext, so the post-handoff read hits EOF on the half-closed socket.
        {.preHandoffMessageBodies = {{0}}, .s2nState = ValidStateSpec{}, .extraCleartext = {}},
        // Valid. Multiple pre-handoff messages and no extra cleartext, so the post-handoff read
        // hits EOF.
        {.preHandoffMessageBodies = {{1}, {2, 3}},
         .s2nState = ValidStateSpec{},
         .extraCleartext = {}},
        // Valid. Extra cleartext that is not a complete message, so the post-handoff read hits EOF.
        {.preHandoffMessageBodies = {{0}},
         .s2nState = ValidStateSpec{},
         .extraCleartext = {255, 254}},
        // Valid. Extra cleartext that is a complete message, so the handoff succeeds and the
        // session enters TLS state.
        {.preHandoffMessageBodies = {{0}},
         .s2nState = ValidStateSpec{},
         .extraCleartext = messageBytes(makeRawMessage(dbMsg, {0x2a}))},
        // Invalid. All-zero s2n state, which fails deserialization.
        {.preHandoffMessageBodies = {{0}},
         .s2nState = RawStateSpec{.raw = std::vector<int>(256, 0)},
         .extraCleartext = {}},
        // Invalid. Truncated s2n state, only 10 bytes.
        {.preHandoffMessageBodies = {{0}},
         .s2nState = RawStateSpec{.raw = std::vector<int>(10, 1)},
         .extraCleartext = {}},
    };
}

/**
 * Builds the fuzztest domain for HandoffSpec, seeded with the cases above.
 */
auto buildHandoffDomain() {
    using namespace fuzztest;

    return StructOf<HandoffSpec>(
               VectorOf(VectorOf(InRange(0, 255)).WithMaxSize(64)).WithMinSize(1).WithMaxSize(4),
               VariantOf<S2nStateSpec>(
                   StructOf<ValidStateSpec>(),
                   StructOf<RawStateSpec>(VectorOf(InRange(0, 255)).WithMaxSize(1024))),
               VectorOf(InRange(0, 255)).WithMaxSize(1024))
        .WithSeeds(buildHandoffSeeds);
}

/**
 * Fuzzes the OP_HANDOFF message within a realistic sequence.
 *
 * The sequence is a valid PROXY v2 header, then one or more cleartext messages, then one
 * OP_HANDOFF. The header is valid by construction, so prelude() succeeds and the OP_HANDOFF logic
 * is reached. The fuzzer then half-closes the proxy end, so a short read is seen as EOF instead of
 * blocking until the read timeout.
 *
 * The fuzzed input is a HandoffSpec that carries the following.
 * - The pre-handoff cleartext message bodies, at least one.
 * - The s2n state in the OP_HANDOFF message, either the fixture's valid state or arbitrary raw
 *   bytes.
 * - The extra cleartext carried alongside the s2n state.
 *
 * The assertions are as follows.
 * - The pre-handoff messages are returned unchanged in order while the session stays in Cleartext.
 * - A valid s2n state completes the handoff and the session enters TLS, while an invalid one fails
 *   the handoff and leaves the session in Cleartext.
 * - The post-handoff read is served from the buffered extra cleartext, returning a complete
 *   message and hitting EOF on an incomplete one.
 * - No file descriptors leak across the session's lifetime.
 *
 * The s2n state begins with an 8-byte version tag that deserialization validates, so random bytes
 * and mutation will not reach the success path. The domain is seeded with valid and invalid cases,
 * and the fixture supplies one serialized state from a single TLS handshake for the whole run.
 */
class HandoffMessageFuzz : public HandoffTlsState {
public:
    HandoffMessageFuzz() {
        // Do one handshake for the whole run.
        serializeConnection();
    }

    void HandoffMessageParsing(HandoffSpec spec) {
        const auto numFdsBefore = numberOfOpenFileDescriptors();
        {
            int udsFds[2];
            ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, udsFds), 0);
            // The session takes ownership of udsFds[0] and closes it.
            ScopeGuard closeProxy([&] { ::close(udsFds[1]); });

            MockPOSIXInterface posix;
            auto session = std::make_shared<HandoffSession>(
                HandoffSession::Params{.fd = udsFds[0],
                                       .localAddress = std::filesystem::path("/dummy/mongod"),
                                       .transportLayer = nullptr,
                                       .acceptedOnLoadBalancedSocket = false,
                                       .acceptedOnPrioritySocket = false,
                                       .s2nConfig = deserConfig(),
                                       .posix = posix});

            // Send a valid PROXY v2 header so prelude() succeeds.
            const auto header = buildProxyHeader(nullptr, nullptr, nullptr, 0);
            writeAll(udsFds[1], reinterpret_cast<const char*>(header.data()), header.size());
            try {
                session->prelude();
            } catch (const DBException&) {
                FAIL("prelude() rejected a valid PROXY v2 header");
            }

            // Pre-handoff cleartext messages are returned unchanged, in order, in Cleartext state.
            for (const auto& body : spec.preHandoffMessageBodies) {
                Message m = makeRawMessage(dbMsg, body);
                writeAll(udsFds[1], m.buf(), m.size());
                auto res = session->sourceMessage();
                ASSERT_OK(res) << res.getStatus();
                ASSERT_EQ(res.getValue().operation(), dbMsg);
                ASSERT_EQ(session->getState_forTest(), HandoffSession::State::Cleartext);

                // Verify the message body matches what was sent.
                const Message& received = res.getValue();
                const int expectedBodySize = static_cast<int>(body.size());
                const int headerSize = sizeof(MSGHEADER::Value);
                ASSERT_GE(received.size(), headerSize)
                    << "received message size is smaller than header";
                ASSERT_EQ(received.size(), headerSize + expectedBodySize)
                    << "received message body size does not match";
                const char* receivedBody = received.buf() + headerSize;
                for (int i = 0; i < expectedBodySize; ++i) {
                    ASSERT_EQ(static_cast<unsigned char>(receivedBody[i]),
                              static_cast<unsigned char>(body[i]))
                        << "body byte mismatch at offset " << i;
                }
            }

            // Build and send OP_HANDOFF.
            std::vector<uint8_t> s2nState;
            if (const auto* rawState = std::get_if<RawStateSpec>(&spec.s2nState)) {
                s2nState.assign(rawState->raw.begin(), rawState->raw.end());
            } else {
                s2nState = serializedState;
            }
            const std::vector<uint8_t> extraCleartext(spec.extraCleartext.begin(),
                                                      spec.extraCleartext.end());
            Message handoff = buildSessionHandoffMessage(s2nState, extraCleartext);

            // Fresh socketpair for the SCM_RIGHTS fd. Its write side is half-closed so the
            // post-handoff read sees EOF instead of blocking.
            int tlsFds[2];
            ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, tlsFds), 0);
            sendMessageWithFds(udsFds[1], handoff, {tlsFds[0]});
            ::close(tlsFds[0]);
            ::shutdown(tlsFds[1], SHUT_WR);
            ::shutdown(udsFds[1], SHUT_WR);

            // sourceMessage() consumes OP_HANDOFF inline and recurses to read the first TLS
            // message. That read uses the buffered extra cleartext before the socket, so a complete
            // buffered message is returned and an incomplete one hits EOF and fails.
            auto res = session->sourceMessage();

            // The handoff succeeds if and only if the s2n state deserializes. The fixture's valid
            // state always does; arbitrary bytes may or may not, so branch on the observed state.
            const auto state = session->getState_forTest();
            if (std::holds_alternative<ValidStateSpec>(spec.s2nState)) {
                ASSERT_EQ(state, HandoffSession::State::TLS);
            }

            if (state == HandoffSession::State::TLS) {
                // The handoff completed, so the first TLS message is served from the buffered extra
                // cleartext. sourceMessage() returns exactly one message, so trailing buffered
                // bytes past the first message are not returned.
                const int completeLen = completeMessageLength(extraCleartext);
                if (completeLen >= 0) {
                    ASSERT_OK(res) << res.getStatus();
                    const Message& received = res.getValue();
                    ASSERT_EQ(received.size(), static_cast<size_t>(completeLen));
                    for (int i = 0; i < completeLen; ++i) {
                        ASSERT_EQ(static_cast<unsigned char>(received.buf()[i]), extraCleartext[i])
                            << "buffered TLS message byte mismatch at offset " << i;
                    }
                } else {
                    // Incomplete buffered message, so the read exhausts the buffer and hits the
                    // half-closed socket, which returns EOF and fails.
                    ASSERT_NOT_OK(res) << res.getStatus();
                }
            } else {
                // The handoff failed, so sourceMessage() returns that failure.
                ASSERT_EQ(state, HandoffSession::State::Cleartext);
                ASSERT_NOT_OK(res) << res.getStatus();
            }
            ::close(tlsFds[1]);
        }
        // The session and socketpairs are gone. No fds should have leaked.
        ASSERT_EQ(numberOfOpenFileDescriptors(), numFdsBefore);
    }
};

FUZZ_TEST_F(HandoffMessageFuzz, HandoffMessageParsing).WithDomains(buildHandoffDomain());

}  // namespace
}  // namespace mongo::transport
